#include <windows.h>
#include <winioctl.h>
#include <setupapi.h>
#include <initguid.h>
#include <devpkey.h>
#include <cfgmgr32.h>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <string>
#include <vector>
#include <memory>
#include <algorithm>

#pragma comment(lib, "setupapi.lib")

// ============================================================
// RAII wrappers for Win32 handles
// ============================================================

class HandleGuard {
    HANDLE m_handle;
public:
    explicit HandleGuard(HANDLE h = INVALID_HANDLE_VALUE) : m_handle(h) {}
    ~HandleGuard() { if (m_handle != INVALID_HANDLE_VALUE) CloseHandle(m_handle); }
    HandleGuard(const HandleGuard&) = delete;
    HandleGuard& operator=(const HandleGuard&) = delete;
    HandleGuard(HandleGuard&& other) noexcept : m_handle(other.m_handle) { other.m_handle = INVALID_HANDLE_VALUE; }
    HandleGuard& operator=(HandleGuard&& other) noexcept {
        if (this != &other) {
            if (m_handle != INVALID_HANDLE_VALUE) CloseHandle(m_handle);
            m_handle = other.m_handle;
            other.m_handle = INVALID_HANDLE_VALUE;
        }
        return *this;
    }
    HANDLE get() const { return m_handle; }
    bool valid() const { return m_handle != INVALID_HANDLE_VALUE; }
};

class DevInfoGuard {
    HDEVINFO m_info;
public:
    explicit DevInfoGuard(HDEVINFO h = INVALID_HANDLE_VALUE) : m_info(h) {}
    ~DevInfoGuard() { if (m_info != INVALID_HANDLE_VALUE) SetupDiDestroyDeviceInfoList(m_info); }
    DevInfoGuard(const DevInfoGuard&) = delete;
    DevInfoGuard& operator=(const DevInfoGuard&) = delete;
    HDEVINFO get() const { return m_info; }
    bool valid() const { return m_info != INVALID_HANDLE_VALUE; }
};

class FindVolumeGuard {
    HANDLE m_handle;
public:
    explicit FindVolumeGuard(HANDLE h = INVALID_HANDLE_VALUE) : m_handle(h) {}
    ~FindVolumeGuard() { if (m_handle != INVALID_HANDLE_VALUE) FindVolumeClose(m_handle); }
    FindVolumeGuard(const FindVolumeGuard&) = delete;
    FindVolumeGuard& operator=(const FindVolumeGuard&) = delete;
    HANDLE get() const { return m_handle; }
    bool valid() const { return m_handle != INVALID_HANDLE_VALUE; }
};

// ============================================================
// Data structures
// ============================================================

struct StorageDeviceInfo {
    bool queried = false;
    STORAGE_BUS_TYPE busType = BusTypeUnknown;
    BOOLEAN removableMedia = FALSE;
    BYTE deviceType = 0;
    std::string vendorId;
    std::string productId;
    std::string productRevision;
    std::string serialNumber;
};

struct StorageAdapterInfo {
    bool queried = false;
    STORAGE_BUS_TYPE busType = BusTypeUnknown;
    DWORD maxTransferLength = 0;
    DWORD alignmentMask = 0;
};

struct DiskGeometryInfo {
    bool queried = false;
    LONGLONG diskSizeBytes = 0;
    LARGE_INTEGER cylinders = {};
    DWORD tracksPerCylinder = 0;
    DWORD sectorsPerTrack = 0;
    DWORD bytesPerSector = 0;
    MEDIA_TYPE mediaType = Unknown;
};

struct PartitionEntry {
    DWORD partitionNumber;
    LONGLONG startingOffset;
    LONGLONG length;
    PARTITION_STYLE style;
    // MBR fields
    BYTE mbrType;
    BOOLEAN mbrBootIndicator;
    // GPT fields
    GUID gptType;
    GUID gptId;
    std::wstring gptName;
};

struct PartitionLayoutInfo {
    bool queried = false;
    PARTITION_STYLE style = PARTITION_STYLE_RAW;
    std::vector<PartitionEntry> partitions;
    // MBR signature
    DWORD mbrSignature = 0;
    // GPT disk ID
    GUID gptDiskId = {};
};

struct VolumeOnDisk {
    std::wstring volumeGuid;
    std::wstring mountPoint;
    std::wstring fileSystem;
    std::wstring volumeLabel;
    DWORD serialNumber = 0;
    ULARGE_INTEGER totalBytes = {};
    ULARGE_INTEGER freeBytes = {};
};

struct PhysicalDriveInfo {
    DWORD driveIndex = 0;
    DWORD deviceNumber = 0;
    bool deviceNumberQueried = false;
    StorageDeviceInfo device;
    StorageAdapterInfo adapter;
    DiskGeometryInfo geometry;
    PartitionLayoutInfo partitions;
    std::vector<VolumeOnDisk> volumes;
    // SetupDi properties
    std::wstring devicePath;
    std::wstring friendlyName;
    std::wstring hardwareIds;
    std::wstring locationInfo;
    std::wstring enumeratorName;
    DWORD removalPolicy = 0;
    bool isSDCandidate = false;
};

// ============================================================
// Bus type name lookup
// ============================================================

static const char* BusTypeName(STORAGE_BUS_TYPE t)
{
    switch (t) {
    case BusTypeUnknown:            return "Unknown";
    case BusTypeScsi:               return "SCSI";
    case BusTypeAtapi:              return "ATAPI";
    case BusTypeAta:                return "ATA";
    case BusType1394:               return "IEEE 1394";
    case BusTypeSsa:                return "SSA";
    case BusTypeFibre:              return "Fibre Channel";
    case BusTypeUsb:                return "USB";
    case BusTypeRAID:               return "RAID";
    case BusTypeiScsi:              return "iSCSI";
    case BusTypeSas:                return "SAS";
    case BusTypeSata:               return "SATA";
    case BusTypeSd:                 return "SD";
    case BusTypeMmc:                return "MMC";
    case BusTypeVirtual:            return "Virtual";
    case BusTypeFileBackedVirtual:  return "File-Backed Virtual";
    case BusTypeSpaces:             return "Storage Spaces";
    case BusTypeNvme:               return "NVMe";
    case BusTypeSCM:                return "SCM";
    case BusTypeUfs:                return "UFS";
    default:                        return "Other";
    }
}

static const char* MediaTypeName(MEDIA_TYPE m)
{
    switch (m) {
    case Unknown:         return "Unknown";
    case RemovableMedia:  return "Removable";
    case FixedMedia:      return "Fixed";
    default:              return "Other";
    }
}

static const char* PartitionStyleName(PARTITION_STYLE s)
{
    switch (s) {
    case PARTITION_STYLE_MBR: return "MBR";
    case PARTITION_STYLE_GPT: return "GPT";
    case PARTITION_STYLE_RAW: return "RAW";
    default:                  return "Unknown";
    }
}

// ============================================================
// Safe string extraction from STORAGE_DEVICE_DESCRIPTOR
// ============================================================

static std::string SafeExtractString(const BYTE* buffer, DWORD bufferSize, DWORD offset)
{
    if (offset == 0 || offset >= bufferSize)
        return {};

    // Find the null terminator, but don't read past the buffer
    const char* start = reinterpret_cast<const char*>(buffer + offset);
    DWORD maxLen = bufferSize - offset;
    DWORD len = 0;
    while (len < maxLen && start[len] != '\0')
        ++len;

    std::string result(start, len);

    // Trim trailing whitespace
    while (!result.empty() && (result.back() == ' ' || result.back() == '\t'))
        result.pop_back();

    return result;
}

// ============================================================
// Query functions
// ============================================================

static bool QueryStorageDeviceDescriptor(HANDLE hDevice, StorageDeviceInfo& out)
{
    STORAGE_PROPERTY_QUERY query = {};
    query.PropertyId = StorageDeviceProperty;
    query.QueryType = PropertyStandardQuery;

    // Pass 1: get required size
    STORAGE_DESCRIPTOR_HEADER header = {};
    DWORD bytesReturned = 0;
    if (!DeviceIoControl(hDevice, IOCTL_STORAGE_QUERY_PROPERTY,
        &query, sizeof(query), &header, sizeof(header),
        &bytesReturned, nullptr))
    {
        return false;
    }

    if (header.Size < sizeof(STORAGE_DEVICE_DESCRIPTOR))
        return false;

    // Pass 2: get full descriptor
    const DWORD bufSize = header.Size;
    auto buffer = std::make_unique<BYTE[]>(bufSize);
    memset(buffer.get(), 0, bufSize);

    if (!DeviceIoControl(hDevice, IOCTL_STORAGE_QUERY_PROPERTY,
        &query, sizeof(query), buffer.get(), bufSize,
        &bytesReturned, nullptr))
    {
        return false;
    }

    const auto* desc = reinterpret_cast<const STORAGE_DEVICE_DESCRIPTOR*>(buffer.get());
    out.busType = desc->BusType;
    out.removableMedia = desc->RemovableMedia;
    out.deviceType = desc->DeviceType;
    out.vendorId = SafeExtractString(buffer.get(), bufSize, desc->VendorIdOffset);
    out.productId = SafeExtractString(buffer.get(), bufSize, desc->ProductIdOffset);
    out.productRevision = SafeExtractString(buffer.get(), bufSize, desc->ProductRevisionOffset);
    out.serialNumber = SafeExtractString(buffer.get(), bufSize, desc->SerialNumberOffset);
    out.queried = true;
    return true;
}

static bool QueryStorageAdapterDescriptor(HANDLE hDevice, StorageAdapterInfo& out)
{
    STORAGE_PROPERTY_QUERY query = {};
    query.PropertyId = StorageAdapterProperty;
    query.QueryType = PropertyStandardQuery;

    STORAGE_DESCRIPTOR_HEADER header = {};
    DWORD bytesReturned = 0;
    if (!DeviceIoControl(hDevice, IOCTL_STORAGE_QUERY_PROPERTY,
        &query, sizeof(query), &header, sizeof(header),
        &bytesReturned, nullptr))
    {
        return false;
    }

    if (header.Size < sizeof(STORAGE_ADAPTER_DESCRIPTOR))
        return false;

    const DWORD bufSize = header.Size;
    auto buffer = std::make_unique<BYTE[]>(bufSize);
    memset(buffer.get(), 0, bufSize);

    if (!DeviceIoControl(hDevice, IOCTL_STORAGE_QUERY_PROPERTY,
        &query, sizeof(query), buffer.get(), bufSize,
        &bytesReturned, nullptr))
    {
        return false;
    }

    const auto* desc = reinterpret_cast<const STORAGE_ADAPTER_DESCRIPTOR*>(buffer.get());
    out.busType = desc->BusType;
    out.maxTransferLength = desc->MaximumTransferLength;
    out.alignmentMask = desc->AlignmentMask;
    out.queried = true;
    return true;
}

static bool QueryDiskGeometry(HANDLE hDevice, DiskGeometryInfo& out)
{
    DISK_GEOMETRY_EX dgex = {};
    DWORD bytesReturned = 0;
    if (!DeviceIoControl(hDevice, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX,
        nullptr, 0, &dgex, sizeof(dgex),
        &bytesReturned, nullptr))
    {
        // Fallback to basic geometry
        DISK_GEOMETRY dg = {};
        if (!DeviceIoControl(hDevice, IOCTL_DISK_GET_DRIVE_GEOMETRY,
            nullptr, 0, &dg, sizeof(dg),
            &bytesReturned, nullptr))
        {
            return false;
        }
        out.cylinders = dg.Cylinders;
        out.tracksPerCylinder = dg.TracksPerCylinder;
        out.sectorsPerTrack = dg.SectorsPerTrack;
        out.bytesPerSector = dg.BytesPerSector;
        out.mediaType = dg.MediaType;
        out.diskSizeBytes = dg.Cylinders.QuadPart
            * dg.TracksPerCylinder
            * dg.SectorsPerTrack
            * dg.BytesPerSector;
        out.queried = true;
        return true;
    }

    out.diskSizeBytes = dgex.DiskSize.QuadPart;
    out.cylinders = dgex.Geometry.Cylinders;
    out.tracksPerCylinder = dgex.Geometry.TracksPerCylinder;
    out.sectorsPerTrack = dgex.Geometry.SectorsPerTrack;
    out.bytesPerSector = dgex.Geometry.BytesPerSector;
    out.mediaType = dgex.Geometry.MediaType;
    out.queried = true;
    return true;
}

static bool QueryDeviceNumber(HANDLE hDevice, DWORD& diskNumber)
{
    STORAGE_DEVICE_NUMBER sdn = {};
    DWORD bytesReturned = 0;
    if (!DeviceIoControl(hDevice, IOCTL_STORAGE_GET_DEVICE_NUMBER,
        nullptr, 0, &sdn, sizeof(sdn),
        &bytesReturned, nullptr))
    {
        return false;
    }
    diskNumber = sdn.DeviceNumber;
    return true;
}

static bool QueryPartitionLayout(HANDLE hDevice, PartitionLayoutInfo& out)
{
    // Start with space for up to 16 partitions, grow if needed
    DWORD bufSize = sizeof(DRIVE_LAYOUT_INFORMATION_EX)
        + 16 * sizeof(PARTITION_INFORMATION_EX);

    for (int attempt = 0; attempt < 5; ++attempt)
    {
        auto buffer = std::make_unique<BYTE[]>(bufSize);
        memset(buffer.get(), 0, bufSize);

        DWORD bytesReturned = 0;
        if (DeviceIoControl(hDevice, IOCTL_DISK_GET_DRIVE_LAYOUT_EX,
            nullptr, 0, buffer.get(), bufSize,
            &bytesReturned, nullptr))
        {
            const auto* layout = reinterpret_cast<const DRIVE_LAYOUT_INFORMATION_EX*>(buffer.get());
            out.style = layout->PartitionStyle;

            if (layout->PartitionStyle == PARTITION_STYLE_MBR)
                out.mbrSignature = layout->Mbr.Signature;
            else if (layout->PartitionStyle == PARTITION_STYLE_GPT)
                out.gptDiskId = layout->Gpt.DiskId;

            for (DWORD i = 0; i < layout->PartitionCount; ++i)
            {
                const auto& pe = layout->PartitionEntry[i];
                if (pe.PartitionLength.QuadPart == 0)
                    continue;

                PartitionEntry entry = {};
                entry.partitionNumber = pe.PartitionNumber;
                entry.startingOffset = pe.StartingOffset.QuadPart;
                entry.length = pe.PartitionLength.QuadPart;
                entry.style = pe.PartitionStyle;

                if (pe.PartitionStyle == PARTITION_STYLE_MBR)
                {
                    entry.mbrType = pe.Mbr.PartitionType;
                    entry.mbrBootIndicator = pe.Mbr.BootIndicator;
                }
                else if (pe.PartitionStyle == PARTITION_STYLE_GPT)
                {
                    entry.gptType = pe.Gpt.PartitionType;
                    entry.gptId = pe.Gpt.PartitionId;
                    entry.gptName = pe.Gpt.Name;
                }

                out.partitions.push_back(entry);
            }

            out.queried = true;
            return true;
        }

        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER)
            return false;

        bufSize *= 2;
    }

    return false;
}

// ============================================================
// Volume enumeration
// ============================================================

static std::vector<VolumeOnDisk> FindVolumesOnDisk(DWORD targetDiskNumber)
{
    std::vector<VolumeOnDisk> results;
    WCHAR volumeName[MAX_PATH] = {};

    FindVolumeGuard hFind(FindFirstVolumeW(volumeName, ARRAYSIZE(volumeName)));
    if (!hFind.valid())
        return results;

    do
    {
        const size_t len = wcslen(volumeName);
        if (len == 0 || volumeName[len - 1] != L'\\')
            continue;

        // Remove trailing backslash to open as device
        volumeName[len - 1] = L'\0';
        HandleGuard hVolume(CreateFileW(volumeName, 0,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_EXISTING, 0, nullptr));
        // Restore backslash
        volumeName[len - 1] = L'\\';

        if (!hVolume.valid())
            continue;

        // Check which physical disk this volume resides on
        VOLUME_DISK_EXTENTS extents = {};
        DWORD bytesReturned = 0;
        BOOL ok = DeviceIoControl(hVolume.get(),
            IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS,
            nullptr, 0, &extents, sizeof(extents),
            &bytesReturned, nullptr);

        if (!ok && GetLastError() != ERROR_MORE_DATA)
            continue;

        if (extents.NumberOfDiskExtents == 0)
            continue;

        // Check if any extent belongs to our target disk
        bool onTargetDisk = false;
        for (DWORD i = 0; i < extents.NumberOfDiskExtents; ++i)
        {
            if (extents.Extents[i].DiskNumber == targetDiskNumber)
            {
                onTargetDisk = true;
                break;
            }
        }
        if (!onTargetDisk)
            continue;

        VolumeOnDisk vol;
        vol.volumeGuid = volumeName;

        // Get mount point (drive letter)
        WCHAR pathNames[512] = {};
        DWORD charCount = ARRAYSIZE(pathNames);
        if (GetVolumePathNamesForVolumeNameW(volumeName, pathNames, charCount, &charCount))
        {
            if (pathNames[0] != L'\0')
                vol.mountPoint = pathNames;
        }

        // Get volume information (file system, label, serial)
        WCHAR fsName[64] = {};
        WCHAR label[MAX_PATH] = {};
        DWORD volSerial = 0;
        if (GetVolumeInformationW(volumeName, label, ARRAYSIZE(label),
            &volSerial, nullptr, nullptr, fsName, ARRAYSIZE(fsName)))
        {
            vol.fileSystem = fsName;
            vol.volumeLabel = label;
            vol.serialNumber = volSerial;
        }

        // Get disk space
        if (!vol.mountPoint.empty())
        {
            GetDiskFreeSpaceExW(vol.mountPoint.c_str(),
                nullptr, &vol.totalBytes, &vol.freeBytes);
        }

        results.push_back(std::move(vol));

    } while (FindNextVolumeW(hFind.get(), volumeName, ARRAYSIZE(volumeName)));

    return results;
}

// ============================================================
// SetupDi device property enumeration
// ============================================================

DEFINE_GUID(MY_GUID_DEVINTERFACE_DISK,
    0x53f56307L, 0xb6bf, 0x11d0,
    0x94, 0xf2, 0x00, 0xa0, 0xc9, 0x1e, 0xfb, 0x8b);

static std::wstring GetDeviceRegistryStringProperty(
    HDEVINFO hDevInfo, SP_DEVINFO_DATA& devInfoData, DWORD property)
{
    DWORD dataType = 0;
    DWORD bufferSize = 0;
    SetupDiGetDeviceRegistryPropertyW(hDevInfo, &devInfoData,
        property, &dataType, nullptr, 0, &bufferSize);

    if (bufferSize == 0)
        return {};

    auto buffer = std::make_unique<BYTE[]>(bufferSize);
    if (!SetupDiGetDeviceRegistryPropertyW(hDevInfo, &devInfoData,
        property, &dataType, buffer.get(), bufferSize, nullptr))
    {
        return {};
    }

    // REG_SZ or REG_MULTI_SZ
    if (dataType == REG_SZ)
        return reinterpret_cast<const WCHAR*>(buffer.get());

    if (dataType == REG_MULTI_SZ)
    {
        // Concatenate with semicolons
        std::wstring result;
        const WCHAR* p = reinterpret_cast<const WCHAR*>(buffer.get());
        while (*p)
        {
            if (!result.empty()) result += L"; ";
            result += p;
            p += wcslen(p) + 1;
        }
        return result;
    }

    return {};
}

static DWORD GetDeviceRegistryDwordProperty(
    HDEVINFO hDevInfo, SP_DEVINFO_DATA& devInfoData, DWORD property)
{
    DWORD value = 0;
    DWORD dataType = 0;
    DWORD bufferSize = sizeof(DWORD);
    if (SetupDiGetDeviceRegistryPropertyW(hDevInfo, &devInfoData,
        property, &dataType, reinterpret_cast<BYTE*>(&value),
        bufferSize, nullptr))
    {
        return value;
    }
    return 0;
}

struct SetupDiDiskInfo {
    std::wstring devicePath;
    std::wstring friendlyName;
    std::wstring hardwareIds;
    std::wstring locationInfo;
    std::wstring enumeratorName;
    DWORD removalPolicy = 0;
    DWORD deviceNumber = MAXDWORD;
};

static std::vector<SetupDiDiskInfo> EnumerateDiskDevices()
{
    std::vector<SetupDiDiskInfo> results;

    DevInfoGuard hDevInfo(SetupDiGetClassDevsW(
        &MY_GUID_DEVINTERFACE_DISK, nullptr, nullptr,
        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE));

    if (!hDevInfo.valid())
        return results;

    SP_DEVICE_INTERFACE_DATA spdid = {};
    spdid.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);

    for (DWORD idx = 0;
        SetupDiEnumDeviceInterfaces(hDevInfo.get(), nullptr,
            &MY_GUID_DEVINTERFACE_DISK, idx, &spdid);
        ++idx)
    {
        // Get required size
        DWORD requiredSize = 0;
        SetupDiGetDeviceInterfaceDetailW(hDevInfo.get(), &spdid,
            nullptr, 0, &requiredSize, nullptr);

        if (requiredSize == 0)
            continue;

        auto detailBuf = std::make_unique<BYTE[]>(requiredSize);
        auto* pDetail = reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(detailBuf.get());
        pDetail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);

        SP_DEVINFO_DATA devInfoData = {};
        devInfoData.cbSize = sizeof(SP_DEVINFO_DATA);

        if (!SetupDiGetDeviceInterfaceDetailW(hDevInfo.get(), &spdid,
            pDetail, requiredSize, &requiredSize, &devInfoData))
        {
            continue;
        }

        SetupDiDiskInfo info;
        info.devicePath = pDetail->DevicePath;
        info.friendlyName = GetDeviceRegistryStringProperty(
            hDevInfo.get(), devInfoData, SPDRP_FRIENDLYNAME);
        info.hardwareIds = GetDeviceRegistryStringProperty(
            hDevInfo.get(), devInfoData, SPDRP_HARDWAREID);
        info.locationInfo = GetDeviceRegistryStringProperty(
            hDevInfo.get(), devInfoData, SPDRP_LOCATION_INFORMATION);
        info.enumeratorName = GetDeviceRegistryStringProperty(
            hDevInfo.get(), devInfoData, SPDRP_ENUMERATOR_NAME);
        info.removalPolicy = GetDeviceRegistryDwordProperty(
            hDevInfo.get(), devInfoData, SPDRP_REMOVAL_POLICY);

        // Open to get device number for matching to PhysicalDrive
        HandleGuard hDisk(CreateFileW(pDetail->DevicePath, 0,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_EXISTING, 0, nullptr));

        if (hDisk.valid())
        {
            DWORD devNum = 0;
            if (QueryDeviceNumber(hDisk.get(), devNum))
                info.deviceNumber = devNum;
        }

        results.push_back(std::move(info));
    }

    return results;
}

// ============================================================
// SD card classification
// ============================================================

static bool ContainsCaseInsensitive(const std::string& haystack, const char* needle)
{
    std::string lowerHay = haystack;
    std::string lowerNeedle = needle;
    std::transform(lowerHay.begin(), lowerHay.end(), lowerHay.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::transform(lowerNeedle.begin(), lowerNeedle.end(), lowerNeedle.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lowerHay.find(lowerNeedle) != std::string::npos;
}

static bool ContainsCaseInsensitiveW(const std::wstring& haystack, const wchar_t* needle)
{
    std::wstring lowerHay = haystack;
    std::wstring lowerNeedle = needle;
    std::transform(lowerHay.begin(), lowerHay.end(), lowerHay.begin(),
        [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    std::transform(lowerNeedle.begin(), lowerNeedle.end(), lowerNeedle.begin(),
        [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return lowerHay.find(lowerNeedle) != std::wstring::npos;
}

static const char* ClassifyDrive(const PhysicalDriveInfo& info)
{
    if (info.device.queried)
    {
        if (info.device.busType == BusTypeSd)
            return "SD Card (native SD bus)";
        if (info.device.busType == BusTypeMmc)
            return "MMC Card (native MMC bus)";

        if (info.device.busType == BusTypeUsb && info.device.removableMedia)
        {
            // Check product/vendor strings for card reader hints
            if (ContainsCaseInsensitive(info.device.productId, "card reader") ||
                ContainsCaseInsensitive(info.device.productId, "sd/mmc") ||
                ContainsCaseInsensitive(info.device.productId, "sd card") ||
                ContainsCaseInsensitive(info.device.productId, "microsd") ||
                ContainsCaseInsensitive(info.device.productId, "cardreader") ||
                ContainsCaseInsensitive(info.device.productId, "multi-card") ||
                ContainsCaseInsensitive(info.device.vendorId, "card reader"))
            {
                return "Likely SD Card (USB card reader detected)";
            }

            // Check hardware IDs from SetupDi
            if (ContainsCaseInsensitiveW(info.hardwareIds, L"SD\\") ||
                ContainsCaseInsensitiveW(info.hardwareIds, L"SDA\\") ||
                ContainsCaseInsensitiveW(info.hardwareIds, L"SDMMC\\"))
            {
                return "Likely SD Card (hardware ID match)";
            }

            return "USB Removable Media (could be SD in USB reader)";
        }

        if (info.device.busType == BusTypeUsb && !info.device.removableMedia)
            return "USB Fixed Disk";

        if (!info.device.removableMedia)
            return "Fixed Disk";

        return "Removable Media";
    }

    return "Unknown";
}

// ============================================================
// Formatting helpers
// ============================================================

static void FormatBytes(LONGLONG bytes, char* buf, size_t bufLen)
{
    if (bytes < 0)
    {
        sprintf_s(buf, bufLen, "N/A");
        return;
    }

    const double KB = 1024.0;
    const double MB = KB * 1024.0;
    const double GB = MB * 1024.0;
    const double TB = GB * 1024.0;
    const double val = static_cast<double>(bytes);

    if (val >= TB)
        sprintf_s(buf, bufLen, "%.2f TB (%lld bytes)", val / TB, bytes);
    else if (val >= GB)
        sprintf_s(buf, bufLen, "%.2f GB (%lld bytes)", val / GB, bytes);
    else if (val >= MB)
        sprintf_s(buf, bufLen, "%.2f MB (%lld bytes)", val / MB, bytes);
    else if (val >= KB)
        sprintf_s(buf, bufLen, "%.2f KB (%lld bytes)", val / KB, bytes);
    else
        sprintf_s(buf, bufLen, "%lld bytes", bytes);
}

static void FormatGUID(const GUID& g, char* buf, size_t bufLen)
{
    sprintf_s(buf, bufLen,
        "{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
        g.Data1, g.Data2, g.Data3,
        g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3],
        g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
}

static const char* MbrPartitionTypeName(BYTE type)
{
    switch (type) {
    case 0x00: return "Empty";
    case 0x01: return "FAT12";
    case 0x04: return "FAT16 (<32MB)";
    case 0x05: return "Extended";
    case 0x06: return "FAT16 (>32MB)";
    case 0x07: return "NTFS/exFAT/HPFS";
    case 0x0B: return "FAT32 (CHS)";
    case 0x0C: return "FAT32 (LBA)";
    case 0x0E: return "FAT16 (LBA)";
    case 0x0F: return "Extended (LBA)";
    case 0x11: return "Hidden FAT12";
    case 0x14: return "Hidden FAT16 (<32MB)";
    case 0x16: return "Hidden FAT16 (>32MB)";
    case 0x17: return "Hidden NTFS";
    case 0x1B: return "Hidden FAT32 (CHS)";
    case 0x1C: return "Hidden FAT32 (LBA)";
    case 0x1E: return "Hidden FAT16 (LBA)";
    case 0x27: return "Windows RE";
    case 0x42: return "Dynamic Disk";
    case 0x82: return "Linux Swap";
    case 0x83: return "Linux";
    case 0x85: return "Linux Extended";
    case 0x8E: return "Linux LVM";
    case 0xEE: return "GPT Protective";
    case 0xEF: return "EFI System";
    default:   return "Other";
    }
}

static const char* RemovalPolicyName(DWORD policy)
{
    switch (policy) {
    case CM_REMOVAL_POLICY_EXPECT_NO_REMOVAL:       return "ExpectNoRemoval";
    case CM_REMOVAL_POLICY_EXPECT_ORDERLY_REMOVAL:   return "ExpectOrderlyRemoval";
    case CM_REMOVAL_POLICY_EXPECT_SURPRISE_REMOVAL:  return "ExpectSurpriseRemoval";
    default: return "Unknown";
    }
}

// ============================================================
// Print functions
// ============================================================

static void PrintDriveInfo(const PhysicalDriveInfo& info)
{
    const char* classification = ClassifyDrive(info);

    wprintf(L"\n");
    wprintf(L"================================================================\n");
    wprintf(L"  PhysicalDrive%lu", info.driveIndex);
    if (info.isSDCandidate)
        wprintf(L"  *** SD CARD CANDIDATE ***");
    wprintf(L"\n");
    wprintf(L"================================================================\n");

    printf("  Classification:     %s\n", classification);

    // Device descriptor
    if (info.device.queried)
    {
        printf("\n  --- Storage Device Descriptor ---\n");
        printf("  Bus Type:           %s (0x%02X)\n",
            BusTypeName(info.device.busType), static_cast<int>(info.device.busType));
        printf("  Removable Media:    %s\n",
            info.device.removableMedia ? "Yes" : "No");
        printf("  Device Type:        0x%02X\n", info.device.deviceType);
        printf("  Vendor ID:          \"%s\"\n", info.device.vendorId.c_str());
        printf("  Product ID:         \"%s\"\n", info.device.productId.c_str());
        printf("  Product Revision:   \"%s\"\n", info.device.productRevision.c_str());
        printf("  Serial Number:      \"%s\"\n", info.device.serialNumber.c_str());
    }
    else
    {
        printf("  [Could not query device descriptor]\n");
    }

    // Adapter descriptor
    if (info.adapter.queried)
    {
        printf("\n  --- Storage Adapter Descriptor ---\n");
        printf("  Adapter Bus Type:   %s (0x%02X)\n",
            BusTypeName(info.adapter.busType), static_cast<int>(info.adapter.busType));
        printf("  Max Transfer:       %lu bytes\n", info.adapter.maxTransferLength);
        printf("  Alignment Mask:     0x%08lX\n", info.adapter.alignmentMask);
    }

    // Geometry
    if (info.geometry.queried)
    {
        char sizeBuf[128];
        FormatBytes(info.geometry.diskSizeBytes, sizeBuf, sizeof(sizeBuf));

        printf("\n  --- Disk Geometry ---\n");
        printf("  Disk Size:          %s\n", sizeBuf);
        printf("  Media Type:         %s\n", MediaTypeName(info.geometry.mediaType));
        printf("  Cylinders:          %lld\n", info.geometry.cylinders.QuadPart);
        printf("  Tracks/Cylinder:    %lu\n", info.geometry.tracksPerCylinder);
        printf("  Sectors/Track:      %lu\n", info.geometry.sectorsPerTrack);
        printf("  Bytes/Sector:       %lu\n", info.geometry.bytesPerSector);
    }
    else
    {
        printf("  [Could not query disk geometry]\n");
    }

    // SetupDi properties
    if (!info.friendlyName.empty() || !info.hardwareIds.empty() ||
        !info.locationInfo.empty() || !info.enumeratorName.empty())
    {
        printf("\n  --- Device Properties (SetupDi) ---\n");
        if (!info.friendlyName.empty())
            wprintf(L"  Friendly Name:      \"%s\"\n", info.friendlyName.c_str());
        if (!info.enumeratorName.empty())
            wprintf(L"  Enumerator:         \"%s\"\n", info.enumeratorName.c_str());
        if (!info.hardwareIds.empty())
            wprintf(L"  Hardware IDs:       \"%s\"\n", info.hardwareIds.c_str());
        if (!info.locationInfo.empty())
            wprintf(L"  Location:           \"%s\"\n", info.locationInfo.c_str());
        if (!info.devicePath.empty())
            wprintf(L"  Device Path:        \"%s\"\n", info.devicePath.c_str());
        if (info.removalPolicy != 0)
            printf("  Removal Policy:     %s (%lu)\n",
                RemovalPolicyName(info.removalPolicy), info.removalPolicy);
    }

    // Partition layout
    if (info.partitions.queried)
    {
        printf("\n  --- Partition Layout ---\n");
        printf("  Partition Style:    %s\n", PartitionStyleName(info.partitions.style));

        if (info.partitions.style == PARTITION_STYLE_MBR)
            printf("  MBR Signature:      0x%08lX\n", info.partitions.mbrSignature);
        else if (info.partitions.style == PARTITION_STYLE_GPT)
        {
            char guidBuf[64];
            FormatGUID(info.partitions.gptDiskId, guidBuf, sizeof(guidBuf));
            printf("  GPT Disk ID:        %s\n", guidBuf);
        }

        if (info.partitions.partitions.empty())
        {
            printf("  (No partitions found)\n");
        }
        else
        {
            for (const auto& part : info.partitions.partitions)
            {
                char sizeBuf[128];
                char offsetBuf[128];
                FormatBytes(part.length, sizeBuf, sizeof(sizeBuf));
                FormatBytes(part.startingOffset, offsetBuf, sizeof(offsetBuf));

                printf("\n  Partition #%lu:\n", part.partitionNumber);
                printf("    Offset:           %s\n", offsetBuf);
                printf("    Size:             %s\n", sizeBuf);

                if (part.style == PARTITION_STYLE_MBR)
                {
                    printf("    MBR Type:         0x%02X (%s)\n",
                        part.mbrType, MbrPartitionTypeName(part.mbrType));
                    printf("    Boot Indicator:   %s\n",
                        part.mbrBootIndicator ? "Active" : "Inactive");
                }
                else if (part.style == PARTITION_STYLE_GPT)
                {
                    char guidBuf[64];
                    FormatGUID(part.gptType, guidBuf, sizeof(guidBuf));
                    printf("    GPT Type:         %s\n", guidBuf);
                    FormatGUID(part.gptId, guidBuf, sizeof(guidBuf));
                    printf("    GPT Partition ID: %s\n", guidBuf);
                    if (!part.gptName.empty())
                        wprintf(L"    GPT Name:         \"%s\"\n", part.gptName.c_str());
                }
            }
        }
    }
    else
    {
        printf("  [Could not query partition layout]\n");
    }

    // Volumes
    if (!info.volumes.empty())
    {
        printf("\n  --- Mounted Volumes ---\n");
        for (const auto& vol : info.volumes)
        {
            wprintf(L"\n  Volume: %s\n", vol.volumeGuid.c_str());
            if (!vol.mountPoint.empty())
                wprintf(L"    Mount Point:      %s\n", vol.mountPoint.c_str());
            if (!vol.volumeLabel.empty())
                wprintf(L"    Label:            \"%s\"\n", vol.volumeLabel.c_str());
            if (!vol.fileSystem.empty())
                wprintf(L"    File System:      %s\n", vol.fileSystem.c_str());
            if (vol.serialNumber != 0)
                printf("    Volume Serial:    %04X-%04X\n",
                    (vol.serialNumber >> 16) & 0xFFFF,
                    vol.serialNumber & 0xFFFF);
            if (vol.totalBytes.QuadPart > 0)
            {
                char totalBuf[128], freeBuf[128];
                FormatBytes(static_cast<LONGLONG>(vol.totalBytes.QuadPart), totalBuf, sizeof(totalBuf));
                FormatBytes(static_cast<LONGLONG>(vol.freeBytes.QuadPart), freeBuf, sizeof(freeBuf));
                printf("    Total Size:       %s\n", totalBuf);
                printf("    Free Space:       %s\n", freeBuf);
            }
        }
    }
    else
    {
        printf("\n  [No mounted volumes found on this disk]\n");
    }
}

// ============================================================
// Main
// ============================================================

static bool IsRunningElevated()
{
    HANDLE hToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken))
        return false;

    TOKEN_ELEVATION elevation = {};
    DWORD cbSize = sizeof(TOKEN_ELEVATION);
    BOOL success = GetTokenInformation(hToken, TokenElevation,
        &elevation, sizeof(elevation), &cbSize);
    CloseHandle(hToken);

    return success && elevation.TokenIsElevated != 0;
}

int wmain()
{
    printf("SD Card Discovery Tool for Windows\n");
    printf("===================================\n\n");

    if (IsRunningElevated())
    {
        printf("Running as: Administrator (elevated)\n");
    }
    else
    {
        printf("Running as: Standard user (not elevated)\n");
        printf("  Note: Some queries (e.g. partition layout) may return\n");
        printf("  limited results. Run as Administrator for full details.\n");
    }
    printf("\n");

    // Step 1: Gather SetupDi device info for all disk devices
    printf("Enumerating disk device interfaces via SetupDi...\n");
    std::vector<SetupDiDiskInfo> setupDiDevices = EnumerateDiskDevices();
    printf("Found %zu disk device interface(s).\n", setupDiDevices.size());

    // Step 2: Enumerate physical drives and gather all info
    printf("Scanning physical drives...\n");
    std::vector<PhysicalDriveInfo> drives;

    for (DWORD i = 0; i < 64; ++i)
    {
        WCHAR path[64];
        swprintf_s(path, L"\\\\.\\PhysicalDrive%lu", i);

        HandleGuard hDrive(CreateFileW(path, 0,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_EXISTING, 0, nullptr));

        if (!hDrive.valid())
            continue;

        PhysicalDriveInfo info;
        info.driveIndex = i;

        // Device number
        DWORD devNum = 0;
        if (QueryDeviceNumber(hDrive.get(), devNum))
        {
            info.deviceNumber = devNum;
            info.deviceNumberQueried = true;
        }

        // Storage device descriptor
        QueryStorageDeviceDescriptor(hDrive.get(), info.device);

        // Storage adapter descriptor
        QueryStorageAdapterDescriptor(hDrive.get(), info.adapter);

        // Disk geometry
        QueryDiskGeometry(hDrive.get(), info.geometry);

        // Partition layout
        QueryPartitionLayout(hDrive.get(), info.partitions);

        // Match SetupDi info by device number
        if (info.deviceNumberQueried)
        {
            for (const auto& sdi : setupDiDevices)
            {
                if (sdi.deviceNumber == info.deviceNumber)
                {
                    info.devicePath = sdi.devicePath;
                    info.friendlyName = sdi.friendlyName;
                    info.hardwareIds = sdi.hardwareIds;
                    info.locationInfo = sdi.locationInfo;
                    info.enumeratorName = sdi.enumeratorName;
                    info.removalPolicy = sdi.removalPolicy;
                    break;
                }
            }

            // Find volumes mounted on this disk
            info.volumes = FindVolumesOnDisk(info.deviceNumber);
        }

        // Determine if this is an SD card candidate
        if (info.device.queried)
        {
            info.isSDCandidate =
                info.device.busType == BusTypeSd ||
                info.device.busType == BusTypeMmc ||
                (info.device.busType == BusTypeUsb && info.device.removableMedia) ||
                ContainsCaseInsensitiveW(info.hardwareIds, L"SD\\") ||
                ContainsCaseInsensitiveW(info.hardwareIds, L"SDMMC\\");
        }

        drives.push_back(std::move(info));
    }

    printf("Found %zu physical drive(s).\n", drives.size());

    // Step 3: Also list removable drive letters via GetDriveType
    printf("\nRemovable drive letters (GetDriveType): ");
    {
        DWORD logicalDrives = GetLogicalDrives();
        bool foundAny = false;
        for (int i = 0; i < 26; ++i)
        {
            if (!(logicalDrives & (1u << i)))
                continue;

            WCHAR rootPath[4] = { static_cast<WCHAR>(L'A' + i), L':', L'\\', L'\0' };
            if (GetDriveTypeW(rootPath) == DRIVE_REMOVABLE)
            {
                wprintf(L"%c:\\ ", L'A' + i);
                foundAny = true;
            }
        }
        if (!foundAny)
            printf("(none)");
        printf("\n");
    }

    // Step 4: Print detailed info for all drives
    for (const auto& drive : drives)
        PrintDriveInfo(drive);

    // Summary
    printf("\n================================================================\n");
    printf("  Summary\n");
    printf("================================================================\n");
    {
        int sdCount = 0;
        for (const auto& d : drives)
        {
            if (d.isSDCandidate)
            {
                ++sdCount;
                wprintf(L"  -> PhysicalDrive%lu: %hs\n",
                    d.driveIndex, ClassifyDrive(d));
            }
        }
        if (sdCount == 0)
            printf("  No SD card candidates detected.\n");
        else
            printf("  %d SD card candidate(s) found.\n", sdCount);
    }

    printf("\nDone.\n");
    return 0;
}
