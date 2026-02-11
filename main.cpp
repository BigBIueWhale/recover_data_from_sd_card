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
// SFFDISK / SD command structures (normally from WDK headers)
// ============================================================

#define IOCTL_SFFDISK_QUERY_DEVICE_PROTOCOL \
    CTL_CODE(FILE_DEVICE_DISK, 0x7a0, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_SFFDISK_DEVICE_COMMAND \
    CTL_CODE(FILE_DEVICE_DISK, 0x7a1, METHOD_BUFFERED, FILE_WRITE_ACCESS)

static const GUID GUID_SFF_PROTOCOL_SD =
    { 0xAD7536A8, 0xD055, 0x4c40, { 0xAA, 0x4D, 0x96, 0x31, 0x2D, 0xDB, 0x6B, 0x38 } };
static const GUID GUID_SFF_PROTOCOL_MMC =
    { 0x77274D3F, 0x2365, 0x4491, { 0xA0, 0x30, 0x8B, 0xB4, 0x4A, 0xE6, 0x00, 0x97 } };

#pragma pack(push, 1)
typedef struct _SFFDISK_QUERY_DEVICE_PROTOCOL_DATA {
    USHORT Size;
    USHORT Reserved;
    GUID   ProtocolGUID;
} SFFDISK_QUERY_DEVICE_PROTOCOL_DATA;
#pragma pack(pop)

typedef enum _SFFDISK_DCMD {
    SFFDISK_DC_GET_VERSION    = 0,
    SFFDISK_DC_LOCK_CHANNEL   = 1,
    SFFDISK_DC_UNLOCK_CHANNEL = 2,
    SFFDISK_DC_DEVICE_COMMAND = 3,
} SFFDISK_DCMD;

typedef struct _SFFDISK_DEVICE_COMMAND_DATA {
    USHORT        HeaderSize;
    USHORT        Flags;
    SFFDISK_DCMD  Command;
    USHORT        ProtocolArgumentSize;
    ULONG         DeviceDataBufferSize;
    ULONG_PTR     Information;
    UCHAR         Data[1];
} SFFDISK_DEVICE_COMMAND_DATA;

typedef UCHAR SD_COMMAND_CODE;

typedef enum _SD_COMMAND_CLASS {
    SDCC_STANDARD = 0,
    SDCC_APP_CMD  = 1,
} SD_COMMAND_CLASS;

typedef enum _SD_TRANSFER_DIRECTION {
    SDTD_UNSPECIFIED = 0,
    SDTD_READ        = 1,
    SDTD_WRITE       = 2,
} SD_TRANSFER_DIRECTION;

typedef enum _SD_TRANSFER_TYPE {
    SDTT_UNSPECIFIED          = 0,
    SDTT_CMD_ONLY             = 1,
    SDTT_SINGLE_BLOCK         = 2,
    SDTT_MULTI_BLOCK          = 3,
    SDTT_MULTI_BLOCK_NO_CMD12 = 4,
} SD_TRANSFER_TYPE;

typedef enum _SD_RESPONSE_TYPE {
    SDRT_UNSPECIFIED = 0,
    SDRT_NONE        = 1,
    SDRT_1           = 2,
    SDRT_1B          = 3,
    SDRT_2           = 4,
    SDRT_3           = 5,
    SDRT_4           = 6,
    SDRT_5           = 7,
    SDRT_5B          = 8,
    SDRT_6           = 9,
} SD_RESPONSE_TYPE;

typedef struct _SDCMD_DESCRIPTOR {
    SD_COMMAND_CODE       Cmd;
    SD_COMMAND_CLASS      CmdClass;
    SD_TRANSFER_DIRECTION TransferDirection;
    SD_TRANSFER_TYPE      TransferType;
    SD_RESPONSE_TYPE      ResponseType;
} SDCMD_DESCRIPTOR;

// ============================================================
// Storage property IDs not always in all SDK versions
// ============================================================

#ifndef StorageDeviceWriteCacheProperty
#define StorageDeviceWriteCacheProperty ((STORAGE_PROPERTY_ID)4)
#endif
#ifndef StorageAccessAlignmentProperty
#define StorageAccessAlignmentProperty ((STORAGE_PROPERTY_ID)6)
#endif
#ifndef StorageDeviceSeekPenaltyProperty
#define StorageDeviceSeekPenaltyProperty ((STORAGE_PROPERTY_ID)7)
#endif
#ifndef StorageDeviceTrimProperty
#define StorageDeviceTrimProperty ((STORAGE_PROPERTY_ID)8)
#endif
#ifndef StorageDevicePowerProperty
#define StorageDevicePowerProperty ((STORAGE_PROPERTY_ID)12)
#endif
#ifndef StorageDeviceMediumProductType
#define StorageDeviceMediumProductType ((STORAGE_PROPERTY_ID)15)
#endif
#ifndef StorageDeviceIoCapabilityProperty
#define StorageDeviceIoCapabilityProperty ((STORAGE_PROPERTY_ID)48)
#endif
#ifndef StorageAdapterTemperatureProperty
#define StorageAdapterTemperatureProperty ((STORAGE_PROPERTY_ID)51)
#endif
#ifndef StorageDeviceTemperatureProperty
#define StorageDeviceTemperatureProperty ((STORAGE_PROPERTY_ID)52)
#endif

// Padded header buffer for storage property header queries.
// Some drivers (e.g. Realtek RTS5208) write more than sizeof(STORAGE_DESCRIPTOR_HEADER)
// during the header pass, corrupting the stack if the buffer is only 8 bytes.
struct StoragePropertyHeaderBuffer {
    STORAGE_DESCRIPTOR_HEADER header;
    BYTE _padding[248]; // Total: 256 bytes
};

// ============================================================
// Descriptor structs (custom names to avoid SDK conflicts)
// ============================================================

struct OurWriteCacheProperty {
    DWORD Version;
    DWORD Size;
    DWORD WriteCacheType;
    DWORD WriteCacheEnabled;
    DWORD WriteCacheChangeable;
    DWORD WriteThroughSupported;
    BOOLEAN FlushCacheSupported;
    BOOLEAN UserDefinedPowerProtection;
    BOOLEAN NVCacheEnabled;
};

struct OurAccessAlignmentDescriptor {
    DWORD Version;
    DWORD Size;
    DWORD BytesPerCacheLine;
    DWORD BytesOffsetForCacheAlignment;
    DWORD BytesPerLogicalSector;
    DWORD BytesPerPhysicalSector;
    DWORD BytesOffsetForSectorAlignment;
};

struct OurSeekPenaltyDescriptor {
    DWORD Version;
    DWORD Size;
    BOOLEAN IncursSeekPenalty;
};

struct OurTrimDescriptor {
    DWORD Version;
    DWORD Size;
    BOOLEAN TrimEnabled;
};

struct OurPowerDescriptor {
    DWORD Version;
    DWORD Size;
    BOOLEAN DeviceAttentionSupported;
    BOOLEAN AsynchronousNotificationSupported;
    BOOLEAN IdlePowerManagementEnabled;
    BOOLEAN D3ColdEnabled;
    BOOLEAN D3ColdSupported;
    BOOLEAN NoVerifyDuringIdlePower;
    BYTE Reserved[2];
    DWORD IdleTimeoutInMS;
};

struct OurMediumProductTypeDescriptor {
    DWORD Version;
    DWORD Size;
    DWORD MediumProductType;
};

struct OurIoCapabilityDescriptor {
    DWORD Version;
    DWORD Size;
    DWORD LunMaxIoCount;
    DWORD AdapterMaxIoCount;
};

struct OurTemperatureInfo {
    WORD Index;
    SHORT Temperature;
    SHORT OverThreshold;
    SHORT UnderThreshold;
    BOOLEAN OverThresholdChangable;
    BOOLEAN UnderThresholdChangable;
    BOOLEAN EventGenerated;
    BYTE Reserved0;
    DWORD Reserved1;
};

struct OurTemperatureDataDescriptor {
    DWORD Version;
    DWORD Size;
    SHORT CriticalTemperature;
    SHORT WarningTemperature;
    WORD InfoCount;
    BYTE Reserved0[2];
    OurTemperatureInfo TemperatureInfo[1];
};

// ============================================================
// Error handling
// ============================================================

// Error codes that mean "this property/IOCTL is not supported by the driver."
// ERROR_INVALID_FUNCTION (1): STATUS_INVALID_DEVICE_REQUEST or STATUS_NOT_IMPLEMENTED
// ERROR_NOT_SUPPORTED (50): STATUS_NOT_SUPPORTED
// ERROR_INVALID_PARAMETER (87): Invalid PropertyId on older Windows versions
static bool IsNotSupportedError(DWORD err)
{
    return err == ERROR_INVALID_FUNCTION
        || err == ERROR_NOT_SUPPORTED
        || err == ERROR_INVALID_PARAMETER;
}

// ============================================================
// Fatal error reporting
// ============================================================

[[noreturn]] static void FatalError(const char* context)
{
    const DWORD err = GetLastError();
    LPSTR msgBuf = nullptr;
    FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, err, MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US),
        reinterpret_cast<LPSTR>(&msgBuf), 0, nullptr);

    fprintf(stderr, "\nFATAL ERROR: %s\n", context);
    fprintf(stderr, "  Win32 error code: %lu (0x%08lX)\n", err, err);
    if (msgBuf)
    {
        fprintf(stderr, "  System message: %s", msgBuf);
        LocalFree(msgBuf);
    }
    fprintf(stderr, "\n");
    exit(1);
}

[[noreturn]] static void FatalErrorMsg(const char* message)
{
    fprintf(stderr, "\nFATAL ERROR: %s\n\n", message);
    exit(1);
}

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
    STORAGE_BUS_TYPE busType = BusTypeUnknown;
    BOOLEAN removableMedia = FALSE;
    BYTE deviceType = 0;
    std::string vendorId;
    std::string productId;
    std::string productRevision;
    std::string serialNumber;
};

struct StorageAdapterInfo {
    STORAGE_BUS_TYPE busType = BusTypeUnknown;
    DWORD maxTransferLength = 0;
    DWORD alignmentMask = 0;
};

struct DiskGeometryInfo {
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
    BYTE mbrType;
    BOOLEAN mbrBootIndicator;
    GUID gptType;
    GUID gptId;
    std::wstring gptName;
};

struct PartitionLayoutInfo {
    PARTITION_STYLE style = PARTITION_STYLE_RAW;
    std::vector<PartitionEntry> partitions;
    DWORD mbrSignature = 0;
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

struct WriteCacheInfo {
    DWORD writeCacheType = 0;
    DWORD writeCacheEnabled = 0;
    DWORD writeCacheChangeable = 0;
    DWORD writeThroughSupported = 0;
    BOOLEAN flushCacheSupported = FALSE;
    BOOLEAN userDefinedPowerProtection = FALSE;
    BOOLEAN nvCacheEnabled = FALSE;
};

struct AccessAlignmentInfo {
    DWORD bytesPerCacheLine = 0;
    DWORD bytesOffsetForCacheAlignment = 0;
    DWORD bytesPerLogicalSector = 0;
    DWORD bytesPerPhysicalSector = 0;
    DWORD bytesOffsetForSectorAlignment = 0;
};

struct SeekPenaltyInfo {
    BOOLEAN incursSeekPenalty = FALSE;
};

struct TrimInfo {
    BOOLEAN trimEnabled = FALSE;
};

struct DevicePowerInfo {
    BOOLEAN deviceAttentionSupported = FALSE;
    BOOLEAN asyncNotificationSupported = FALSE;
    BOOLEAN idlePowerManagementEnabled = FALSE;
    BOOLEAN d3ColdEnabled = FALSE;
    BOOLEAN d3ColdSupported = FALSE;
    BOOLEAN noVerifyDuringIdlePower = FALSE;
    DWORD idleTimeoutInMS = 0;
};

struct MediumProductTypeInfo {
    DWORD mediumProductType = 0;
};

struct IoCapabilityInfo {
    DWORD lunMaxIoCount = 0;
    DWORD adapterMaxIoCount = 0;
};

struct TemperatureInfo {
    SHORT criticalTemperature = 0;
    SHORT warningTemperature = 0;
    struct SensorInfo {
        WORD index = 0;
        SHORT temperature = 0;
        SHORT overThreshold = 0;
        SHORT underThreshold = 0;
    };
    std::vector<SensorInfo> sensors;
};

struct MediaTypeExInfo {
    DWORD deviceType = 0;
    struct MediaEntry {
        DWORD mediaType = 0;
        DWORD mediaCharacteristics = 0;
        LARGE_INTEGER cylinders = {};
        DWORD tracksPerCylinder = 0;
        DWORD sectorsPerTrack = 0;
        DWORD bytesPerSector = 0;
        DWORD numberMediaSides = 0;
    };
    std::vector<MediaEntry> entries;
};

struct SD_CID_Register {
    BYTE raw[16] = {};
    BYTE mid = 0;
    char oid[3] = {};
    char pnm[6] = {};
    BYTE prv_major = 0;
    BYTE prv_minor = 0;
    DWORD psn = 0;
    WORD mdt_year = 0;
    BYTE mdt_month = 0;
    BYTE crc = 0;
};

struct SD_CSD_Register {
    BYTE raw[16] = {};
    BYTE csdVersion = 0;
    BYTE taac = 0;
    BYTE nsac = 0;
    BYTE tranSpeed = 0;
    WORD ccc = 0;
    BYTE readBlLen = 0;
    BYTE readBlPartial = 0;
    BYTE writeBlkMisalign = 0;
    BYTE readBlkMisalign = 0;
    BYTE dsrImp = 0;
    WORD cSizeV1 = 0;
    BYTE cSizeMultV1 = 0;
    DWORD cSizeV2 = 0;
    BYTE eraseBlkEn = 0;
    BYTE sectorSize = 0;
    BYTE wpGrpSize = 0;
    BYTE wpGrpEnable = 0;
    BYTE r2wFactor = 0;
    BYTE writeBlLen = 0;
    BYTE writeBlPartial = 0;
    BYTE fileFormatGrp = 0;
    BYTE copy = 0;
    BYTE permWriteProtect = 0;
    BYTE tmpWriteProtect = 0;
    BYTE fileFormat = 0;
    BYTE crc = 0;
    ULONGLONG computedCapacityBytes = 0;
};

struct SD_SCR_Register {
    BYTE raw[8] = {};
    BYTE scrStructure = 0;
    BYTE sdSpec = 0;
    BYTE dataStatAfterErase = 0;
    BYTE sdSecurity = 0;
    BYTE sdBusWidths = 0;
    BYTE sdSpec3 = 0;
    BYTE exSecurity = 0;
    BYTE sdSpec4 = 0;
    BYTE sdSpecX = 0;
    BYTE cmdSupport = 0;
};

struct SD_OCR_Register {
    BYTE raw[4] = {};
    DWORD ocrValue = 0;
    BOOLEAN vdd27_28 = FALSE;
    BOOLEAN vdd28_29 = FALSE;
    BOOLEAN vdd29_30 = FALSE;
    BOOLEAN vdd30_31 = FALSE;
    BOOLEAN vdd31_32 = FALSE;
    BOOLEAN vdd32_33 = FALSE;
    BOOLEAN vdd33_34 = FALSE;
    BOOLEAN vdd34_35 = FALSE;
    BOOLEAN vdd35_36 = FALSE;
    BOOLEAN s18a = FALSE;
    BOOLEAN uhs2CardStatus = FALSE;
    BOOLEAN ccs = FALSE;
    BOOLEAN busy = FALSE;
};

struct SD_Status_Register {
    BYTE raw[64] = {};
    BYTE datBusWidth = 0;
    BYTE securedMode = 0;
    WORD sdCardType = 0;
    DWORD sizeOfProtectedArea = 0;
    BYTE speedClass = 0;
    BYTE performanceMove = 0;
    BYTE auSize = 0;
    WORD eraseSize = 0;
    BYTE eraseTimeout = 0;
    BYTE eraseOffset = 0;
    BYTE uhsSpeedGrade = 0;
    BYTE uhsAuSize = 0;
    BYTE videoSpeedClass = 0;
    BYTE appPerfClass = 0;
    BYTE performanceEnhance = 0;
};

struct SD_SwitchStatus {
    BYTE raw[64] = {};
    WORD maxCurrentConsumption = 0;
    WORD funGroup6Support = 0;
    WORD funGroup5Support = 0;
    WORD funGroup4Support = 0;
    WORD funGroup3Support = 0;
    WORD funGroup2Support = 0;
    WORD funGroup1Support = 0;
    BYTE funGroup6Selection = 0;
    BYTE funGroup5Selection = 0;
    BYTE funGroup4Selection = 0;
    BYTE funGroup3Selection = 0;
    BYTE funGroup2Selection = 0;
    BYTE funGroup1Selection = 0;
    BYTE dataStructureVersion = 0;
    WORD funGroup6BusyStatus = 0;
    WORD funGroup5BusyStatus = 0;
    WORD funGroup4BusyStatus = 0;
    WORD funGroup3BusyStatus = 0;
    WORD funGroup2BusyStatus = 0;
    WORD funGroup1BusyStatus = 0;
};

struct PhysicalDriveInfo {
    DWORD driveIndex = 0;
    DWORD deviceNumber = 0;
    StorageDeviceInfo device;
    StorageAdapterInfo adapter;
    DiskGeometryInfo geometry;
    PartitionLayoutInfo partitions;
    std::vector<VolumeOnDisk> volumes;
    std::wstring devicePath;
    std::wstring friendlyName;
    std::wstring hardwareIds;
    std::wstring locationInfo;
    std::wstring enumeratorName;
    DWORD removalPolicy = 0;
    bool isSDCandidate = false;

    // Storage property queries (optional — may not be supported by all drivers)
    bool hasWriteCache = false;
    bool hasAccessAlignment = false;
    bool hasSeekPenalty = false;
    bool hasTrim = false;
    bool hasPower = false;
    bool hasMediumProductType = false;
    bool hasIoCapability = false;
    bool hasDeviceTemperature = false;
    bool hasAdapterTemperature = false;
    bool hasMediaTypesEx = false;
    WriteCacheInfo writeCache;
    AccessAlignmentInfo accessAlignment;
    SeekPenaltyInfo seekPenalty;
    TrimInfo trim;
    DevicePowerInfo power;
    MediumProductTypeInfo mediumProductType;
    IoCapabilityInfo ioCapability;
    TemperatureInfo deviceTemperature;
    TemperatureInfo adapterTemperature;
    MediaTypeExInfo mediaTypesEx;

    // SD card register data
    bool hasSDRegisters = false;
    bool sdProtocolIsSD = false;
    bool sdProtocolIsMMC = false;
    GUID sdProtocolGUID = {};
    SD_CID_Register sdCID;
    SD_CSD_Register sdCSD;
    SD_SCR_Register sdSCR;
    SD_OCR_Register sdOCR;
    SD_Status_Register sdStatus;
    SD_SwitchStatus sdSwitch;
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

static const char* WriteCacheTypeName(DWORD type)
{
    switch (type) {
    case 0: return "Unknown";
    case 1: return "None";
    case 2: return "WriteBack";
    case 3: return "WriteThrough";
    default: return "Other";
    }
}

static const char* WriteCacheEnabledName(DWORD enabled)
{
    switch (enabled) {
    case 0: return "Unknown";
    case 1: return "Disabled";
    case 2: return "Enabled";
    default: return "Other";
    }
}

static const char* WriteCacheChangeName(DWORD change)
{
    switch (change) {
    case 0: return "Unknown";
    case 1: return "NotChangeable";
    case 2: return "Changeable";
    default: return "Other";
    }
}

static const char* WriteThroughName(DWORD wt)
{
    switch (wt) {
    case 0: return "Unknown";
    case 1: return "NotSupported";
    case 2: return "Supported";
    default: return "Other";
    }
}

static const char* MediumProductTypeName(DWORD type)
{
    switch (type) {
    case 0x00: return "Not indicated";
    case 0x01: return "CFast";
    case 0x02: return "CompactFlash";
    case 0x03: return "Memory Stick";
    case 0x04: return "MultiMediaCard (MMC)";
    case 0x05: return "SD Card";
    case 0x06: return "QXD";
    case 0x07: return "Universal Flash Storage (UFS)";
    default:   return "Unknown";
    }
}

static void FormatMediaCharacteristics(DWORD flags, char* buf, size_t bufLen)
{
    buf[0] = '\0';
    size_t pos = 0;
    auto append = [&](const char* s) {
        if (pos > 0 && pos + 3 < bufLen) { buf[pos++] = ' '; buf[pos++] = '|'; buf[pos++] = ' '; }
        size_t slen = strlen(s);
        if (pos + slen < bufLen) { memcpy(buf + pos, s, slen); pos += slen; }
        buf[pos] = '\0';
    };
    if (flags & 0x00000001) append("ERASEABLE");
    if (flags & 0x00000002) append("WRITE_ONCE");
    if (flags & 0x00000004) append("READ_ONLY");
    if (flags & 0x00000008) append("READ_WRITE");
    if (flags & 0x80000000) append("WRITE_PROTECTED");
    if (pos == 0) sprintf_s(buf, bufLen, "0x%08lX", flags);
}

// ============================================================
// Bit extraction for SD register parsing (big-endian byte arrays)
// ============================================================

// Extract bits from a big-endian byte array of totalBits size.
// startBit: MSB position of the field (e.g. 127 for MSB of 16-byte array)
// numBits: number of bits to extract (1-32)
static DWORD ExtractBitsBE(const BYTE* data, int totalBits, int startBit, int numBits)
{
    DWORD result = 0;
    for (int i = 0; i < numBits; i++)
    {
        int bit = startBit - i;
        int byteIdx = (totalBits - 1 - bit) / 8;
        int bitPos = bit % 8;
        if (data[byteIdx] & (1 << bitPos))
            result |= (1u << (numBits - 1 - i));
    }
    return result;
}

// ============================================================
// Safe string extraction from STORAGE_DEVICE_DESCRIPTOR
// ============================================================

static std::string SafeExtractString(const BYTE* buffer, DWORD bufferSize, DWORD offset)
{
    if (offset == 0 || offset >= bufferSize)
        return {};

    const char* start = reinterpret_cast<const char*>(buffer + offset);
    DWORD maxLen = bufferSize - offset;
    DWORD len = 0;
    while (len < maxLen && start[len] != '\0')
        ++len;

    std::string result(start, len);

    while (!result.empty() && (result.back() == ' ' || result.back() == '\t'))
        result.pop_back();

    return result;
}

// ============================================================
// Query functions — all fatal on failure
// ============================================================

static void QueryStorageDeviceDescriptor(HANDLE hDevice, DWORD driveIndex, StorageDeviceInfo& out)
{
    STORAGE_PROPERTY_QUERY query = {};
    query.PropertyId = StorageDeviceProperty;
    query.QueryType = PropertyStandardQuery;

    StoragePropertyHeaderBuffer _hdrBuf = {};
    auto& header = _hdrBuf.header;
    DWORD bytesReturned = 0;
    if (!DeviceIoControl(hDevice, IOCTL_STORAGE_QUERY_PROPERTY,
        &query, sizeof(query), &_hdrBuf, sizeof(_hdrBuf),
        &bytesReturned, nullptr))
    {
        char msg[256];
        sprintf_s(msg, "IOCTL_STORAGE_QUERY_PROPERTY (header) failed on PhysicalDrive%lu", driveIndex);
        FatalError(msg);
    }

    if (header.Size < sizeof(STORAGE_DEVICE_DESCRIPTOR))
    {
        char msg[256];
        sprintf_s(msg, "STORAGE_DEVICE_DESCRIPTOR header.Size (%lu) < expected (%zu) on PhysicalDrive%lu",
            header.Size, sizeof(STORAGE_DEVICE_DESCRIPTOR), driveIndex);
        FatalErrorMsg(msg);
    }

    const DWORD bufSize = header.Size;
    auto buffer = std::make_unique<BYTE[]>(bufSize);
    memset(buffer.get(), 0, bufSize);

    if (!DeviceIoControl(hDevice, IOCTL_STORAGE_QUERY_PROPERTY,
        &query, sizeof(query), buffer.get(), bufSize,
        &bytesReturned, nullptr))
    {
        char msg[256];
        sprintf_s(msg, "IOCTL_STORAGE_QUERY_PROPERTY (full descriptor) failed on PhysicalDrive%lu", driveIndex);
        FatalError(msg);
    }

    const auto* desc = reinterpret_cast<const STORAGE_DEVICE_DESCRIPTOR*>(buffer.get());
    out.busType = desc->BusType;
    out.removableMedia = desc->RemovableMedia;
    out.deviceType = desc->DeviceType;
    out.vendorId = SafeExtractString(buffer.get(), bufSize, desc->VendorIdOffset);
    out.productId = SafeExtractString(buffer.get(), bufSize, desc->ProductIdOffset);
    out.productRevision = SafeExtractString(buffer.get(), bufSize, desc->ProductRevisionOffset);
    out.serialNumber = SafeExtractString(buffer.get(), bufSize, desc->SerialNumberOffset);
}

static void QueryStorageAdapterDescriptor(HANDLE hDevice, DWORD driveIndex, StorageAdapterInfo& out)
{
    STORAGE_PROPERTY_QUERY query = {};
    query.PropertyId = StorageAdapterProperty;
    query.QueryType = PropertyStandardQuery;

    StoragePropertyHeaderBuffer _hdrBuf = {};
    auto& header = _hdrBuf.header;
    DWORD bytesReturned = 0;
    if (!DeviceIoControl(hDevice, IOCTL_STORAGE_QUERY_PROPERTY,
        &query, sizeof(query), &_hdrBuf, sizeof(_hdrBuf),
        &bytesReturned, nullptr))
    {
        char msg[256];
        sprintf_s(msg, "IOCTL_STORAGE_QUERY_PROPERTY (adapter header) failed on PhysicalDrive%lu", driveIndex);
        FatalError(msg);
    }

    if (header.Size < sizeof(STORAGE_ADAPTER_DESCRIPTOR))
    {
        char msg[256];
        sprintf_s(msg, "STORAGE_ADAPTER_DESCRIPTOR header.Size (%lu) < expected (%zu) on PhysicalDrive%lu",
            header.Size, sizeof(STORAGE_ADAPTER_DESCRIPTOR), driveIndex);
        FatalErrorMsg(msg);
    }

    const DWORD bufSize = header.Size;
    auto buffer = std::make_unique<BYTE[]>(bufSize);
    memset(buffer.get(), 0, bufSize);

    if (!DeviceIoControl(hDevice, IOCTL_STORAGE_QUERY_PROPERTY,
        &query, sizeof(query), buffer.get(), bufSize,
        &bytesReturned, nullptr))
    {
        char msg[256];
        sprintf_s(msg, "IOCTL_STORAGE_QUERY_PROPERTY (adapter full) failed on PhysicalDrive%lu", driveIndex);
        FatalError(msg);
    }

    const auto* desc = reinterpret_cast<const STORAGE_ADAPTER_DESCRIPTOR*>(buffer.get());
    out.busType = static_cast<STORAGE_BUS_TYPE>(desc->BusType);
    out.maxTransferLength = desc->MaximumTransferLength;
    out.alignmentMask = desc->AlignmentMask;
}

static void QueryDiskGeometry(HANDLE hDevice, DWORD driveIndex, DiskGeometryInfo& out)
{
    DISK_GEOMETRY_EX dgex = {};
    DWORD bytesReturned = 0;
    if (!DeviceIoControl(hDevice, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX,
        nullptr, 0, &dgex, sizeof(dgex),
        &bytesReturned, nullptr))
    {
        char msg[256];
        sprintf_s(msg, "IOCTL_DISK_GET_DRIVE_GEOMETRY_EX failed on PhysicalDrive%lu", driveIndex);
        FatalError(msg);
    }

    out.diskSizeBytes = dgex.DiskSize.QuadPart;
    out.cylinders = dgex.Geometry.Cylinders;
    out.tracksPerCylinder = dgex.Geometry.TracksPerCylinder;
    out.sectorsPerTrack = dgex.Geometry.SectorsPerTrack;
    out.bytesPerSector = dgex.Geometry.BytesPerSector;
    out.mediaType = dgex.Geometry.MediaType;
}

static DWORD QueryDeviceNumber(HANDLE hDevice, DWORD driveIndex)
{
    STORAGE_DEVICE_NUMBER sdn = {};
    DWORD bytesReturned = 0;
    if (!DeviceIoControl(hDevice, IOCTL_STORAGE_GET_DEVICE_NUMBER,
        nullptr, 0, &sdn, sizeof(sdn),
        &bytesReturned, nullptr))
    {
        char msg[256];
        sprintf_s(msg, "IOCTL_STORAGE_GET_DEVICE_NUMBER failed on PhysicalDrive%lu", driveIndex);
        FatalError(msg);
    }
    return sdn.DeviceNumber;
}

static void QueryPartitionLayout(HANDLE hDevice, DWORD driveIndex, PartitionLayoutInfo& out)
{
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
            out.style = static_cast<PARTITION_STYLE>(layout->PartitionStyle);

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
            return;
        }

        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER)
        {
            char msg[256];
            sprintf_s(msg, "IOCTL_DISK_GET_DRIVE_LAYOUT_EX failed on PhysicalDrive%lu", driveIndex);
            FatalError(msg);
        }

        bufSize *= 2;
    }

    char msg[256];
    sprintf_s(msg, "IOCTL_DISK_GET_DRIVE_LAYOUT_EX: buffer too small after 5 attempts on PhysicalDrive%lu", driveIndex);
    FatalErrorMsg(msg);
}

// ============================================================
// Additional storage property queries
// ============================================================

// Helper: two-pass storage property query that returns false if unsupported.
// On not-supported errors (1, 50, 87), returns false. On other errors, calls FatalError.
static bool QueryOptionalStorageProperty(HANDLE hDevice, DWORD driveIndex,
    STORAGE_PROPERTY_ID propId, const char* propName,
    std::unique_ptr<BYTE[]>& outBuffer, DWORD& outSize)
{
    STORAGE_PROPERTY_QUERY query = {};
    query.PropertyId = propId;
    query.QueryType = PropertyStandardQuery;

    StoragePropertyHeaderBuffer _hdrBuf = {};
    auto& header = _hdrBuf.header;
    DWORD bytesReturned = 0;
    if (!DeviceIoControl(hDevice, IOCTL_STORAGE_QUERY_PROPERTY,
        &query, sizeof(query), &_hdrBuf, sizeof(_hdrBuf),
        &bytesReturned, nullptr))
    {
        if (IsNotSupportedError(GetLastError()))
            return false;
        char msg[256];
        sprintf_s(msg, "IOCTL_STORAGE_QUERY_PROPERTY (%s header) failed on PhysicalDrive%lu", propName, driveIndex);
        FatalError(msg);
    }

    const DWORD bufSize = header.Size;
    if (bufSize == 0)
        return false;

    auto buffer = std::make_unique<BYTE[]>(bufSize);
    memset(buffer.get(), 0, bufSize);

    if (!DeviceIoControl(hDevice, IOCTL_STORAGE_QUERY_PROPERTY,
        &query, sizeof(query), buffer.get(), bufSize,
        &bytesReturned, nullptr))
    {
        if (IsNotSupportedError(GetLastError()))
            return false;
        char msg[256];
        sprintf_s(msg, "IOCTL_STORAGE_QUERY_PROPERTY (%s) failed on PhysicalDrive%lu", propName, driveIndex);
        FatalError(msg);
    }

    outBuffer = std::move(buffer);
    outSize = bufSize;
    return true;
}

static bool QueryWriteCacheProperty(HANDLE hDevice, DWORD driveIndex, WriteCacheInfo& out)
{
    std::unique_ptr<BYTE[]> buffer;
    DWORD bufSize = 0;
    if (!QueryOptionalStorageProperty(hDevice, driveIndex,
        StorageDeviceWriteCacheProperty, "WriteCacheProperty", buffer, bufSize))
        return false;

    const auto* prop = reinterpret_cast<const OurWriteCacheProperty*>(buffer.get());
    out.writeCacheType = prop->WriteCacheType;
    out.writeCacheEnabled = prop->WriteCacheEnabled;
    out.writeCacheChangeable = prop->WriteCacheChangeable;
    out.writeThroughSupported = prop->WriteThroughSupported;
    out.flushCacheSupported = prop->FlushCacheSupported;
    out.userDefinedPowerProtection = prop->UserDefinedPowerProtection;
    out.nvCacheEnabled = prop->NVCacheEnabled;
    return true;
}

static bool QueryAccessAlignmentProperty(HANDLE hDevice, DWORD driveIndex, AccessAlignmentInfo& out)
{
    std::unique_ptr<BYTE[]> buffer;
    DWORD bufSize = 0;
    if (!QueryOptionalStorageProperty(hDevice, driveIndex,
        StorageAccessAlignmentProperty, "AccessAlignmentProperty", buffer, bufSize))
        return false;

    const auto* desc = reinterpret_cast<const OurAccessAlignmentDescriptor*>(buffer.get());
    out.bytesPerCacheLine = desc->BytesPerCacheLine;
    out.bytesOffsetForCacheAlignment = desc->BytesOffsetForCacheAlignment;
    out.bytesPerLogicalSector = desc->BytesPerLogicalSector;
    out.bytesPerPhysicalSector = desc->BytesPerPhysicalSector;
    out.bytesOffsetForSectorAlignment = desc->BytesOffsetForSectorAlignment;
    return true;
}

static bool QuerySeekPenaltyProperty(HANDLE hDevice, DWORD driveIndex, SeekPenaltyInfo& out)
{
    std::unique_ptr<BYTE[]> buffer;
    DWORD bufSize = 0;
    if (!QueryOptionalStorageProperty(hDevice, driveIndex,
        StorageDeviceSeekPenaltyProperty, "SeekPenaltyProperty", buffer, bufSize))
        return false;

    const auto* desc = reinterpret_cast<const OurSeekPenaltyDescriptor*>(buffer.get());
    out.incursSeekPenalty = desc->IncursSeekPenalty;
    return true;
}

static bool QueryTrimProperty(HANDLE hDevice, DWORD driveIndex, TrimInfo& out)
{
    std::unique_ptr<BYTE[]> buffer;
    DWORD bufSize = 0;
    if (!QueryOptionalStorageProperty(hDevice, driveIndex,
        StorageDeviceTrimProperty, "TrimProperty", buffer, bufSize))
        return false;

    const auto* desc = reinterpret_cast<const OurTrimDescriptor*>(buffer.get());
    out.trimEnabled = desc->TrimEnabled;
    return true;
}

static bool QueryDevicePowerProperty(HANDLE hDevice, DWORD driveIndex, DevicePowerInfo& out)
{
    std::unique_ptr<BYTE[]> buffer;
    DWORD bufSize = 0;
    if (!QueryOptionalStorageProperty(hDevice, driveIndex,
        StorageDevicePowerProperty, "DevicePowerProperty", buffer, bufSize))
        return false;

    const auto* desc = reinterpret_cast<const OurPowerDescriptor*>(buffer.get());
    out.deviceAttentionSupported = desc->DeviceAttentionSupported;
    out.asyncNotificationSupported = desc->AsynchronousNotificationSupported;
    out.idlePowerManagementEnabled = desc->IdlePowerManagementEnabled;
    out.d3ColdEnabled = desc->D3ColdEnabled;
    out.d3ColdSupported = desc->D3ColdSupported;
    out.noVerifyDuringIdlePower = desc->NoVerifyDuringIdlePower;
    out.idleTimeoutInMS = desc->IdleTimeoutInMS;
    return true;
}

static bool QueryMediumProductType(HANDLE hDevice, DWORD driveIndex, MediumProductTypeInfo& out)
{
    std::unique_ptr<BYTE[]> buffer;
    DWORD bufSize = 0;
    if (!QueryOptionalStorageProperty(hDevice, driveIndex,
        StorageDeviceMediumProductType, "MediumProductType", buffer, bufSize))
        return false;

    const auto* desc = reinterpret_cast<const OurMediumProductTypeDescriptor*>(buffer.get());
    out.mediumProductType = desc->MediumProductType;
    return true;
}

static bool QueryIoCapabilityProperty(HANDLE hDevice, DWORD driveIndex, IoCapabilityInfo& out)
{
    std::unique_ptr<BYTE[]> buffer;
    DWORD bufSize = 0;
    if (!QueryOptionalStorageProperty(hDevice, driveIndex,
        StorageDeviceIoCapabilityProperty, "IoCapabilityProperty", buffer, bufSize))
        return false;

    const auto* desc = reinterpret_cast<const OurIoCapabilityDescriptor*>(buffer.get());
    out.lunMaxIoCount = desc->LunMaxIoCount;
    out.adapterMaxIoCount = desc->AdapterMaxIoCount;
    return true;
}

static bool QueryTemperatureProperty(HANDLE hDevice, DWORD driveIndex,
    STORAGE_PROPERTY_ID propId, const char* propName, TemperatureInfo& out)
{
    std::unique_ptr<BYTE[]> buffer;
    DWORD bufSize = 0;
    if (!QueryOptionalStorageProperty(hDevice, driveIndex,
        propId, propName, buffer, bufSize))
        return false;

    const auto* desc = reinterpret_cast<const OurTemperatureDataDescriptor*>(buffer.get());
    out.criticalTemperature = desc->CriticalTemperature;
    out.warningTemperature = desc->WarningTemperature;

    for (WORD i = 0; i < desc->InfoCount; ++i)
    {
        const BYTE* infoPtr = reinterpret_cast<const BYTE*>(&desc->TemperatureInfo[0])
            + i * sizeof(OurTemperatureInfo);
        if (infoPtr + sizeof(OurTemperatureInfo) > buffer.get() + bufSize)
            break;
        const auto* ti = reinterpret_cast<const OurTemperatureInfo*>(infoPtr);
        TemperatureInfo::SensorInfo si;
        si.index = ti->Index;
        si.temperature = ti->Temperature;
        si.overThreshold = ti->OverThreshold;
        si.underThreshold = ti->UnderThreshold;
        out.sensors.push_back(si);
    }
    return true;
}

static bool QueryDeviceTemperature(HANDLE hDevice, DWORD driveIndex, TemperatureInfo& out)
{
    return QueryTemperatureProperty(hDevice, driveIndex,
        StorageDeviceTemperatureProperty, "DeviceTemperature", out);
}

static bool QueryAdapterTemperature(HANDLE hDevice, DWORD driveIndex, TemperatureInfo& out)
{
    return QueryTemperatureProperty(hDevice, driveIndex,
        StorageAdapterTemperatureProperty, "AdapterTemperature", out);
}

static bool QueryMediaTypesEx(HANDLE hDevice, DWORD driveIndex, MediaTypeExInfo& out)
{
    DWORD bufSize = 4096;
    for (int attempt = 0; attempt < 5; ++attempt)
    {
        auto buffer = std::make_unique<BYTE[]>(bufSize);
        memset(buffer.get(), 0, bufSize);

        DWORD bytesReturned = 0;
        if (DeviceIoControl(hDevice, IOCTL_STORAGE_GET_MEDIA_TYPES_EX,
            nullptr, 0, buffer.get(), bufSize,
            &bytesReturned, nullptr))
        {
            const auto* gmt = reinterpret_cast<const GET_MEDIA_TYPES*>(buffer.get());
            out.deviceType = gmt->DeviceType;

            for (DWORD i = 0; i < gmt->MediaInfoCount; ++i)
            {
                const auto& mi = gmt->MediaInfo[i];
                MediaTypeExInfo::MediaEntry entry;
                entry.mediaType = static_cast<DWORD>(mi.DeviceSpecific.DiskInfo.MediaType);
                entry.mediaCharacteristics = mi.DeviceSpecific.DiskInfo.MediaCharacteristics;
                entry.cylinders = mi.DeviceSpecific.DiskInfo.Cylinders;
                entry.tracksPerCylinder = mi.DeviceSpecific.DiskInfo.TracksPerCylinder;
                entry.sectorsPerTrack = mi.DeviceSpecific.DiskInfo.SectorsPerTrack;
                entry.bytesPerSector = mi.DeviceSpecific.DiskInfo.BytesPerSector;
                entry.numberMediaSides = mi.DeviceSpecific.DiskInfo.NumberMediaSides;
                out.entries.push_back(entry);
            }
            return true;
        }

        DWORD err = GetLastError();
        if (err == ERROR_INSUFFICIENT_BUFFER)
        {
            bufSize *= 2;
            continue;
        }
        if (IsNotSupportedError(err))
            return false;

        char msg[256];
        sprintf_s(msg, "IOCTL_STORAGE_GET_MEDIA_TYPES_EX failed on PhysicalDrive%lu", driveIndex);
        FatalError(msg);
    }
    return false;
}

// ============================================================
// SD command helper functions
// ============================================================

static void SendSDCommand(
    HANDLE hVolume,
    BYTE cmdIndex,
    SD_COMMAND_CLASS cmdClass,
    SD_TRANSFER_DIRECTION transferDir,
    SD_TRANSFER_TYPE transferType,
    SD_RESPONSE_TYPE responseType,
    DWORD argument,
    DWORD dataSize,
    BYTE* outData,
    const char* context)
{
    const DWORD dataOffset = (DWORD)offsetof(SFFDISK_DEVICE_COMMAND_DATA, Data);
    const DWORD totalSize = dataOffset + sizeof(SDCMD_DESCRIPTOR) + dataSize;

    auto buffer = std::make_unique<BYTE[]>(totalSize);
    memset(buffer.get(), 0, totalSize);

    auto* cmdData = reinterpret_cast<SFFDISK_DEVICE_COMMAND_DATA*>(buffer.get());
    cmdData->HeaderSize = sizeof(SFFDISK_DEVICE_COMMAND_DATA);
    cmdData->Flags = 0;
    cmdData->Command = SFFDISK_DC_DEVICE_COMMAND;
    cmdData->ProtocolArgumentSize = sizeof(SDCMD_DESCRIPTOR);
    cmdData->DeviceDataBufferSize = dataSize;
    cmdData->Information = argument;

    auto* sdCmd = reinterpret_cast<SDCMD_DESCRIPTOR*>(buffer.get() + dataOffset);
    sdCmd->Cmd = cmdIndex;
    sdCmd->CmdClass = cmdClass;
    sdCmd->TransferDirection = transferDir;
    sdCmd->TransferType = transferType;
    sdCmd->ResponseType = responseType;

    DWORD bytesReturned = 0;
    if (!DeviceIoControl(hVolume, IOCTL_SFFDISK_DEVICE_COMMAND,
        buffer.get(), totalSize,
        buffer.get(), totalSize,
        &bytesReturned, nullptr))
    {
        char msg[256];
        sprintf_s(msg, "IOCTL_SFFDISK_DEVICE_COMMAND (%s) failed", context);
        FatalError(msg);
    }

    if (dataSize > 0 && outData)
    {
        const BYTE* responseData = buffer.get() + dataOffset + sizeof(SDCMD_DESCRIPTOR);
        memcpy(outData, responseData, dataSize);
    }
}

// Returns true if the driver supports SFFDISK IOCTLs, false otherwise.
// When false, errorCode is set to the Win32 error for diagnostic display.
static bool QuerySD_Protocol(HANDLE hVolume, GUID& outGUID, DWORD& errorCode)
{
    SFFDISK_QUERY_DEVICE_PROTOCOL_DATA protData = {};
    protData.Size = sizeof(protData);

    DWORD bytesReturned = 0;
    if (!DeviceIoControl(hVolume, IOCTL_SFFDISK_QUERY_DEVICE_PROTOCOL,
        &protData, sizeof(protData),
        &protData, sizeof(protData),
        &bytesReturned, nullptr))
    {
        errorCode = GetLastError();
        return false;
    }

    outGUID = protData.ProtocolGUID;
    errorCode = 0;
    return true;
}

static void QuerySD_CID(HANDLE hVolume, BYTE outRaw[16])
{
    SendSDCommand(hVolume, 10, SDCC_STANDARD, SDTD_READ,
        SDTT_CMD_ONLY, SDRT_2, 0, 16, outRaw, "CMD10 CID");
}

static void QuerySD_CSD(HANDLE hVolume, BYTE outRaw[16])
{
    SendSDCommand(hVolume, 9, SDCC_STANDARD, SDTD_READ,
        SDTT_CMD_ONLY, SDRT_2, 0, 16, outRaw, "CMD9 CSD");
}

static void QuerySD_SCR(HANDLE hVolume, BYTE outRaw[8])
{
    SendSDCommand(hVolume, 51, SDCC_APP_CMD, SDTD_READ,
        SDTT_SINGLE_BLOCK, SDRT_1, 0, 8, outRaw, "ACMD51 SCR");
}

static void QuerySD_OCR(HANDLE hVolume, BYTE outRaw[4])
{
    SendSDCommand(hVolume, 58, SDCC_STANDARD, SDTD_READ,
        SDTT_CMD_ONLY, SDRT_3, 0, 4, outRaw, "CMD58 OCR");
}

static void QuerySD_Status(HANDLE hVolume, BYTE outRaw[64])
{
    SendSDCommand(hVolume, 13, SDCC_APP_CMD, SDTD_READ,
        SDTT_SINGLE_BLOCK, SDRT_1, 0, 64, outRaw, "ACMD13 SD Status");
}

static void QuerySD_SwitchFunction(HANDLE hVolume, BYTE outRaw[64])
{
    SendSDCommand(hVolume, 6, SDCC_STANDARD, SDTD_READ,
        SDTT_SINGLE_BLOCK, SDRT_1, 0x00FFFFFF, 64, outRaw, "CMD6 Switch");
}

// ============================================================
// SD register parsing functions
// ============================================================

static void ParseCID(const BYTE* raw, SD_CID_Register& cid)
{
    memcpy(cid.raw, raw, 16);
    cid.mid = raw[0];
    cid.oid[0] = (char)raw[1];
    cid.oid[1] = (char)raw[2];
    cid.oid[2] = '\0';
    memcpy(cid.pnm, &raw[3], 5);
    cid.pnm[5] = '\0';
    cid.prv_major = raw[8] >> 4;
    cid.prv_minor = raw[8] & 0x0F;
    cid.psn = ((DWORD)raw[9] << 24) | ((DWORD)raw[10] << 16)
            | ((DWORD)raw[11] << 8) | raw[12];
    cid.mdt_year = 2000 + (WORD)(((raw[13] & 0x0F) << 4) | (raw[14] >> 4));
    cid.mdt_month = raw[14] & 0x0F;
    cid.crc = raw[15] >> 1;
}

static void ParseCSD(const BYTE* raw, SD_CSD_Register& csd)
{
    memcpy(csd.raw, raw, 16);
    csd.csdVersion = (BYTE)ExtractBitsBE(raw, 128, 127, 2);
    csd.taac = (BYTE)ExtractBitsBE(raw, 128, 119, 8);
    csd.nsac = (BYTE)ExtractBitsBE(raw, 128, 111, 8);
    csd.tranSpeed = (BYTE)ExtractBitsBE(raw, 128, 103, 8);
    csd.ccc = (WORD)ExtractBitsBE(raw, 128, 95, 12);
    csd.readBlLen = (BYTE)ExtractBitsBE(raw, 128, 83, 4);
    csd.readBlPartial = (BYTE)ExtractBitsBE(raw, 128, 79, 1);
    csd.writeBlkMisalign = (BYTE)ExtractBitsBE(raw, 128, 78, 1);
    csd.readBlkMisalign = (BYTE)ExtractBitsBE(raw, 128, 77, 1);
    csd.dsrImp = (BYTE)ExtractBitsBE(raw, 128, 76, 1);

    if (csd.csdVersion == 0)
    {
        // CSD v1.0 (SDSC)
        csd.cSizeV1 = (WORD)ExtractBitsBE(raw, 128, 73, 12);
        csd.cSizeMultV1 = (BYTE)ExtractBitsBE(raw, 128, 49, 3);
        ULONGLONG mult = 1ULL << (csd.cSizeMultV1 + 2);
        ULONGLONG blockLen = 1ULL << csd.readBlLen;
        csd.computedCapacityBytes = (csd.cSizeV1 + 1) * mult * blockLen;
    }
    else if (csd.csdVersion == 1)
    {
        // CSD v2.0 (SDHC/SDXC)
        csd.cSizeV2 = ExtractBitsBE(raw, 128, 69, 22);
        csd.computedCapacityBytes = ((ULONGLONG)csd.cSizeV2 + 1) * 512ULL * 1024ULL;
    }

    csd.eraseBlkEn = (BYTE)ExtractBitsBE(raw, 128, 46, 1);
    csd.sectorSize = (BYTE)ExtractBitsBE(raw, 128, 45, 7);
    csd.wpGrpSize = (BYTE)ExtractBitsBE(raw, 128, 38, 7);
    csd.wpGrpEnable = (BYTE)ExtractBitsBE(raw, 128, 31, 1);
    csd.r2wFactor = (BYTE)ExtractBitsBE(raw, 128, 28, 3);
    csd.writeBlLen = (BYTE)ExtractBitsBE(raw, 128, 25, 4);
    csd.writeBlPartial = (BYTE)ExtractBitsBE(raw, 128, 21, 1);
    csd.fileFormatGrp = (BYTE)ExtractBitsBE(raw, 128, 15, 1);
    csd.copy = (BYTE)ExtractBitsBE(raw, 128, 14, 1);
    csd.permWriteProtect = (BYTE)ExtractBitsBE(raw, 128, 13, 1);
    csd.tmpWriteProtect = (BYTE)ExtractBitsBE(raw, 128, 12, 1);
    csd.fileFormat = (BYTE)ExtractBitsBE(raw, 128, 11, 2);
    csd.crc = (BYTE)ExtractBitsBE(raw, 128, 7, 7);
}

static void ParseSCR(const BYTE* raw, SD_SCR_Register& scr)
{
    memcpy(scr.raw, raw, 8);
    // SCR is 64 bits, big-endian
    scr.scrStructure = (BYTE)ExtractBitsBE(raw, 64, 63, 4);
    scr.sdSpec = (BYTE)ExtractBitsBE(raw, 64, 59, 4);
    scr.dataStatAfterErase = (BYTE)ExtractBitsBE(raw, 64, 55, 1);
    scr.sdSecurity = (BYTE)ExtractBitsBE(raw, 64, 54, 3);
    scr.sdBusWidths = (BYTE)ExtractBitsBE(raw, 64, 51, 4);
    scr.sdSpec3 = (BYTE)ExtractBitsBE(raw, 64, 47, 1);
    scr.exSecurity = (BYTE)ExtractBitsBE(raw, 64, 46, 4);
    scr.sdSpec4 = (BYTE)ExtractBitsBE(raw, 64, 42, 1);
    scr.sdSpecX = (BYTE)ExtractBitsBE(raw, 64, 41, 4);
    scr.cmdSupport = (BYTE)ExtractBitsBE(raw, 64, 33, 4);
}

static void ParseOCR(const BYTE* raw, SD_OCR_Register& ocr)
{
    memcpy(ocr.raw, raw, 4);
    ocr.ocrValue = ((DWORD)raw[0] << 24) | ((DWORD)raw[1] << 16)
                 | ((DWORD)raw[2] << 8) | raw[3];
    ocr.vdd27_28 = (ocr.ocrValue >> 15) & 1;
    ocr.vdd28_29 = (ocr.ocrValue >> 16) & 1;
    ocr.vdd29_30 = (ocr.ocrValue >> 17) & 1;
    ocr.vdd30_31 = (ocr.ocrValue >> 18) & 1;
    ocr.vdd31_32 = (ocr.ocrValue >> 19) & 1;
    ocr.vdd32_33 = (ocr.ocrValue >> 20) & 1;
    ocr.vdd33_34 = (ocr.ocrValue >> 21) & 1;
    ocr.vdd34_35 = (ocr.ocrValue >> 22) & 1;
    ocr.vdd35_36 = (ocr.ocrValue >> 23) & 1;
    ocr.s18a = (ocr.ocrValue >> 24) & 1;
    ocr.uhs2CardStatus = (ocr.ocrValue >> 29) & 1;
    ocr.ccs = (ocr.ocrValue >> 30) & 1;
    ocr.busy = (ocr.ocrValue >> 31) & 1;
}

static void ParseSDStatus(const BYTE* raw, SD_Status_Register& st)
{
    memcpy(st.raw, raw, 64);
    st.datBusWidth = (BYTE)ExtractBitsBE(raw, 512, 511, 2);
    st.securedMode = (BYTE)ExtractBitsBE(raw, 512, 509, 1);
    st.sdCardType = (WORD)ExtractBitsBE(raw, 512, 495, 16);
    st.sizeOfProtectedArea = ExtractBitsBE(raw, 512, 479, 32);
    st.speedClass = (BYTE)ExtractBitsBE(raw, 512, 447, 8);
    st.performanceMove = (BYTE)ExtractBitsBE(raw, 512, 439, 8);
    st.auSize = (BYTE)ExtractBitsBE(raw, 512, 431, 4);
    st.eraseSize = (WORD)ExtractBitsBE(raw, 512, 423, 16);
    st.eraseTimeout = (BYTE)ExtractBitsBE(raw, 512, 407, 6);
    st.eraseOffset = (BYTE)ExtractBitsBE(raw, 512, 401, 2);
    st.uhsSpeedGrade = (BYTE)ExtractBitsBE(raw, 512, 399, 4);
    st.uhsAuSize = (BYTE)ExtractBitsBE(raw, 512, 395, 4);
    st.videoSpeedClass = (BYTE)ExtractBitsBE(raw, 512, 383, 8);
    st.appPerfClass = (BYTE)ExtractBitsBE(raw, 512, 367, 8);
    st.performanceEnhance = (BYTE)ExtractBitsBE(raw, 512, 359, 4);
}

static void ParseSwitchStatus(const BYTE* raw, SD_SwitchStatus& sw)
{
    memcpy(sw.raw, raw, 64);
    sw.maxCurrentConsumption = (WORD)ExtractBitsBE(raw, 512, 511, 16);
    sw.funGroup6Support = (WORD)ExtractBitsBE(raw, 512, 495, 16);
    sw.funGroup5Support = (WORD)ExtractBitsBE(raw, 512, 479, 16);
    sw.funGroup4Support = (WORD)ExtractBitsBE(raw, 512, 463, 16);
    sw.funGroup3Support = (WORD)ExtractBitsBE(raw, 512, 447, 16);
    sw.funGroup2Support = (WORD)ExtractBitsBE(raw, 512, 431, 16);
    sw.funGroup1Support = (WORD)ExtractBitsBE(raw, 512, 415, 16);
    sw.funGroup6Selection = (BYTE)ExtractBitsBE(raw, 512, 399, 4);
    sw.funGroup5Selection = (BYTE)ExtractBitsBE(raw, 512, 395, 4);
    sw.funGroup4Selection = (BYTE)ExtractBitsBE(raw, 512, 391, 4);
    sw.funGroup3Selection = (BYTE)ExtractBitsBE(raw, 512, 387, 4);
    sw.funGroup2Selection = (BYTE)ExtractBitsBE(raw, 512, 383, 4);
    sw.funGroup1Selection = (BYTE)ExtractBitsBE(raw, 512, 379, 4);
    sw.dataStructureVersion = (BYTE)ExtractBitsBE(raw, 512, 375, 8);
    sw.funGroup6BusyStatus = (WORD)ExtractBitsBE(raw, 512, 367, 16);
    sw.funGroup5BusyStatus = (WORD)ExtractBitsBE(raw, 512, 351, 16);
    sw.funGroup4BusyStatus = (WORD)ExtractBitsBE(raw, 512, 335, 16);
    sw.funGroup3BusyStatus = (WORD)ExtractBitsBE(raw, 512, 319, 16);
    sw.funGroup2BusyStatus = (WORD)ExtractBitsBE(raw, 512, 303, 16);
    sw.funGroup1BusyStatus = (WORD)ExtractBitsBE(raw, 512, 287, 16);
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
        FatalError("FindFirstVolumeW failed");

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

        // Volumes that don't map to a single physical disk (e.g. spanned,
        // virtual, or system volumes) will fail here — skip them.
        if (!ok && GetLastError() != ERROR_MORE_DATA)
            continue;

        if (extents.NumberOfDiskExtents == 0)
            continue;

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
        // This can fail for RAW/unformatted/corrupt volumes — that's expected.
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

        // Get disk space (only possible if mounted)
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

    if (dataType == REG_SZ)
        return reinterpret_cast<const WCHAR*>(buffer.get());

    if (dataType == REG_MULTI_SZ)
    {
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
        FatalError("SetupDiGetClassDevsW(GUID_DEVINTERFACE_DISK) failed");

    SP_DEVICE_INTERFACE_DATA spdid = {};
    spdid.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);

    for (DWORD idx = 0;
        SetupDiEnumDeviceInterfaces(hDevInfo.get(), nullptr,
            &MY_GUID_DEVINTERFACE_DISK, idx, &spdid);
        ++idx)
    {
        DWORD requiredSize = 0;
        SetupDiGetDeviceInterfaceDetailW(hDevInfo.get(), &spdid,
            nullptr, 0, &requiredSize, nullptr);

        if (requiredSize == 0)
        {
            char msg[256];
            sprintf_s(msg, "SetupDiGetDeviceInterfaceDetailW returned requiredSize=0 at index %lu", idx);
            FatalError(msg);
        }

        auto detailBuf = std::make_unique<BYTE[]>(requiredSize);
        auto* pDetail = reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(detailBuf.get());
        pDetail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);

        SP_DEVINFO_DATA devInfoData = {};
        devInfoData.cbSize = sizeof(SP_DEVINFO_DATA);

        if (!SetupDiGetDeviceInterfaceDetailW(hDevInfo.get(), &spdid,
            pDetail, requiredSize, &requiredSize, &devInfoData))
        {
            char msg[256];
            sprintf_s(msg, "SetupDiGetDeviceInterfaceDetailW (second call) failed at index %lu", idx);
            FatalError(msg);
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

        HandleGuard hDisk(CreateFileW(pDetail->DevicePath, 0,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_EXISTING, 0, nullptr));

        if (hDisk.valid())
        {
            STORAGE_DEVICE_NUMBER sdn = {};
            DWORD br = 0;
            if (DeviceIoControl(hDisk.get(), IOCTL_STORAGE_GET_DEVICE_NUMBER,
                nullptr, 0, &sdn, sizeof(sdn), &br, nullptr))
            {
                info.deviceNumber = sdn.DeviceNumber;
            }
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
        [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    std::transform(lowerNeedle.begin(), lowerNeedle.end(), lowerNeedle.begin(),
        [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    return lowerHay.find(lowerNeedle) != std::wstring::npos;
}

// Checks whether device strings or SetupDi properties suggest an SD card reader.
// Applied to any removable media regardless of bus type, since PCIe card readers
// (e.g. Realtek RTS5208) report BusTypeScsi, not BusTypeSd.
static bool LooksLikeSDCardReader(const PhysicalDriveInfo& info)
{
    // Product ID / Vendor ID from STORAGE_DEVICE_DESCRIPTOR
    if (ContainsCaseInsensitive(info.device.productId, "card reader") ||
        ContainsCaseInsensitive(info.device.productId, "sd/mmc") ||
        ContainsCaseInsensitive(info.device.productId, "sd card") ||
        ContainsCaseInsensitive(info.device.productId, "microsd") ||
        ContainsCaseInsensitive(info.device.productId, "cardreader") ||
        ContainsCaseInsensitive(info.device.productId, "multi-card") ||
        ContainsCaseInsensitive(info.device.vendorId, "card reader"))
    {
        return true;
    }

    // Friendly name from SetupDi (e.g. "SDXC Card", "SD Card Reader")
    if (ContainsCaseInsensitiveW(info.friendlyName, L"SDXC") ||
        ContainsCaseInsensitiveW(info.friendlyName, L"SDHC") ||
        ContainsCaseInsensitiveW(info.friendlyName, L"SD Card") ||
        ContainsCaseInsensitiveW(info.friendlyName, L"MMC Card") ||
        ContainsCaseInsensitiveW(info.friendlyName, L"microSD"))
    {
        return true;
    }

    // Hardware IDs from SetupDi
    if (ContainsCaseInsensitiveW(info.hardwareIds, L"SD\\") ||
        ContainsCaseInsensitiveW(info.hardwareIds, L"SDA\\") ||
        ContainsCaseInsensitiveW(info.hardwareIds, L"SDMMC\\"))
    {
        return true;
    }

    return false;
}

static const char* ClassifyDrive(const PhysicalDriveInfo& info)
{
    // Definitive: native SD/MMC bus
    if (info.device.busType == BusTypeSd)
        return "SD Card (native SD bus)";
    if (info.device.busType == BusTypeMmc)
        return "MMC Card (native MMC bus)";

    // For any removable media, check if it looks like a card reader.
    // PCIe card readers (Realtek, etc.) report BusTypeScsi; USB readers
    // report BusTypeUsb — the heuristics apply to both.
    if (info.device.removableMedia)
    {
        if (LooksLikeSDCardReader(info))
        {
            return "SD Card (card reader detected)";
        }

        if (info.device.busType == BusTypeUsb)
            return "USB Removable Media (could be SD in USB reader)";

        return "Removable Media";
    }

    if (info.device.busType == BusTypeUsb)
        return "USB Fixed Disk";

    return "Fixed Disk";
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

    // Adapter descriptor
    printf("\n  --- Storage Adapter Descriptor ---\n");
    printf("  Adapter Bus Type:   %s (0x%02X)\n",
        BusTypeName(info.adapter.busType), static_cast<int>(info.adapter.busType));
    printf("  Max Transfer:       %lu bytes\n", info.adapter.maxTransferLength);
    printf("  Alignment Mask:     0x%08lX\n", info.adapter.alignmentMask);

    // Geometry
    char sizeBuf[128];
    FormatBytes(info.geometry.diskSizeBytes, sizeBuf, sizeof(sizeBuf));

    printf("\n  --- Disk Geometry ---\n");
    printf("  Disk Size:          %s\n", sizeBuf);
    printf("  Media Type:         %s\n", MediaTypeName(info.geometry.mediaType));
    printf("  Cylinders:          %lld\n", info.geometry.cylinders.QuadPart);
    printf("  Tracks/Cylinder:    %lu\n", info.geometry.tracksPerCylinder);
    printf("  Sectors/Track:      %lu\n", info.geometry.sectorsPerTrack);
    printf("  Bytes/Sector:       %lu\n", info.geometry.bytesPerSector);

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
            char partSizeBuf[128];
            char offsetBuf[128];
            FormatBytes(part.length, partSizeBuf, sizeof(partSizeBuf));
            FormatBytes(part.startingOffset, offsetBuf, sizeof(offsetBuf));

            printf("\n  Partition #%lu:\n", part.partitionNumber);
            printf("    Offset:           %s\n", offsetBuf);
            printf("    Size:             %s\n", partSizeBuf);

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
        printf("\n  (No mounted volumes on this disk)\n");
    }

    // Write Cache
    if (info.hasWriteCache)
    {
        printf("\n  --- Write Cache ---\n");
        printf("  Cache Type:          %s (%lu)\n",
            WriteCacheTypeName(info.writeCache.writeCacheType), info.writeCache.writeCacheType);
        printf("  Cache Enabled:       %s (%lu)\n",
            WriteCacheEnabledName(info.writeCache.writeCacheEnabled), info.writeCache.writeCacheEnabled);
        printf("  Cache Changeable:    %s (%lu)\n",
            WriteCacheChangeName(info.writeCache.writeCacheChangeable), info.writeCache.writeCacheChangeable);
        printf("  Write-Through:       %s (%lu)\n",
            WriteThroughName(info.writeCache.writeThroughSupported), info.writeCache.writeThroughSupported);
        printf("  Flush Supported:     %s\n", info.writeCache.flushCacheSupported ? "Yes" : "No");
        printf("  User Power Protect:  %s\n", info.writeCache.userDefinedPowerProtection ? "Yes" : "No");
        printf("  NV Cache:            %s\n", info.writeCache.nvCacheEnabled ? "Yes" : "No");
    }

    // Access Alignment
    if (info.hasAccessAlignment)
    {
        printf("\n  --- Access Alignment ---\n");
        printf("  Bytes/Logical Sector:    %lu\n", info.accessAlignment.bytesPerLogicalSector);
        printf("  Bytes/Physical Sector:   %lu\n", info.accessAlignment.bytesPerPhysicalSector);
        printf("  Sector Alignment Offset: %lu\n", info.accessAlignment.bytesOffsetForSectorAlignment);
        printf("  Cache Line Size:         %lu\n", info.accessAlignment.bytesPerCacheLine);
        printf("  Cache Alignment Offset:  %lu\n", info.accessAlignment.bytesOffsetForCacheAlignment);
    }

    // Seek Penalty
    if (info.hasSeekPenalty)
    {
        printf("\n  --- Seek Penalty ---\n");
        printf("  Incurs Seek Penalty: %s\n", info.seekPenalty.incursSeekPenalty ? "Yes" : "No");
    }

    // TRIM
    if (info.hasTrim)
    {
        printf("\n  --- TRIM Support ---\n");
        printf("  TRIM Enabled:        %s\n", info.trim.trimEnabled ? "Yes" : "No");
    }

    // Device Power
    if (info.hasPower)
    {
        printf("\n  --- Device Power ---\n");
        printf("  Attention Supported:     %s\n", info.power.deviceAttentionSupported ? "Yes" : "No");
        printf("  Async Notification:      %s\n", info.power.asyncNotificationSupported ? "Yes" : "No");
        printf("  Idle Power Mgmt:         %s\n", info.power.idlePowerManagementEnabled ? "Yes" : "No");
        printf("  D3Cold Enabled:          %s\n", info.power.d3ColdEnabled ? "Yes" : "No");
        printf("  D3Cold Supported:        %s\n", info.power.d3ColdSupported ? "Yes" : "No");
        printf("  No Verify During Idle:   %s\n", info.power.noVerifyDuringIdlePower ? "Yes" : "No");
        printf("  Idle Timeout:            %lu ms\n", info.power.idleTimeoutInMS);
    }

    // Medium Product Type
    if (info.hasMediumProductType)
    {
        printf("\n  --- Medium Product Type ---\n");
        printf("  Product Type:        %s (0x%02lX)\n",
            MediumProductTypeName(info.mediumProductType.mediumProductType),
            info.mediumProductType.mediumProductType);
    }

    // I/O Capability
    if (info.hasIoCapability)
    {
        printf("\n  --- I/O Capability ---\n");
        printf("  LUN Max I/O Count:   %lu\n", info.ioCapability.lunMaxIoCount);
        printf("  Adapter Max I/O:     %lu\n", info.ioCapability.adapterMaxIoCount);
    }

    // Device Temperature
    if (info.hasDeviceTemperature)
    {
        printf("\n  --- Device Temperature ---\n");
        printf("  Critical Temp:       %d C\n", info.deviceTemperature.criticalTemperature);
        printf("  Warning Temp:        %d C\n", info.deviceTemperature.warningTemperature);
        for (const auto& s : info.deviceTemperature.sensors)
            printf("  Sensor %u:            %d C (over: %d C, under: %d C)\n",
                s.index, s.temperature, s.overThreshold, s.underThreshold);
    }

    // Adapter Temperature
    if (info.hasAdapterTemperature)
    {
        printf("\n  --- Adapter Temperature ---\n");
        printf("  Critical Temp:       %d C\n", info.adapterTemperature.criticalTemperature);
        printf("  Warning Temp:        %d C\n", info.adapterTemperature.warningTemperature);
        for (const auto& s : info.adapterTemperature.sensors)
            printf("  Sensor %u:            %d C (over: %d C, under: %d C)\n",
                s.index, s.temperature, s.overThreshold, s.underThreshold);
    }

    // Media Types (Extended)
    if (info.hasMediaTypesEx)
    {
        printf("\n  --- Media Types (Extended) ---\n");
        printf("  Device Type:         0x%08lX\n", info.mediaTypesEx.deviceType);
        if (info.mediaTypesEx.entries.empty())
        {
            printf("  (No media entries)\n");
        }
        else
        {
            int mIdx = 1;
            for (const auto& me : info.mediaTypesEx.entries)
            {
                char charBuf[256];
                FormatMediaCharacteristics(me.mediaCharacteristics, charBuf, sizeof(charBuf));
                printf("  Media #%d:\n", mIdx++);
                printf("    Media Type:        0x%08lX\n", me.mediaType);
                printf("    Characteristics:   %s\n", charBuf);
                printf("    Cylinders:         %lld\n", me.cylinders.QuadPart);
                printf("    Tracks/Cylinder:   %lu\n", me.tracksPerCylinder);
                printf("    Sectors/Track:     %lu\n", me.sectorsPerTrack);
                printf("    Bytes/Sector:      %lu\n", me.bytesPerSector);
                printf("    Sides:             %lu\n", me.numberMediaSides);
            }
        }
    }

    // SD Card Registers (only if queried)
    if (info.hasSDRegisters)
    {
        // Protocol
        printf("\n  --- SD Card Protocol ---\n");
        {
            char guidBuf[64];
            FormatGUID(info.sdProtocolGUID, guidBuf, sizeof(guidBuf));
            printf("  Protocol GUID:       %s\n", guidBuf);
            if (info.sdProtocolIsSD)
                printf("  Protocol:            SD\n");
            else if (info.sdProtocolIsMMC)
                printf("  Protocol:            MMC\n");
            else
                printf("  Protocol:            Unknown\n");
        }

        // CID
        printf("\n  --- SD CID Register (Card Identification) ---\n");
        printf("  Raw:                 ");
        for (int i = 0; i < 16; i++) printf("%02X ", info.sdCID.raw[i]);
        printf("\n");
        printf("  Manufacturer ID:     0x%02X\n", info.sdCID.mid);
        printf("  OEM ID:              \"%s\"\n", info.sdCID.oid);
        printf("  Product Name:        \"%s\"\n", info.sdCID.pnm);
        printf("  Product Revision:    %u.%u\n", info.sdCID.prv_major, info.sdCID.prv_minor);
        printf("  Serial Number:       0x%08lX\n", info.sdCID.psn);
        printf("  Manufacturing Date:  %u/%02u\n", info.sdCID.mdt_year, info.sdCID.mdt_month);
        printf("  CRC7:                0x%02X\n", info.sdCID.crc);

        // CSD
        printf("\n  --- SD CSD Register (Card Specific Data) ---\n");
        printf("  Raw:                 ");
        for (int i = 0; i < 16; i++) printf("%02X ", info.sdCSD.raw[i]);
        printf("\n");
        printf("  CSD Version:         %s\n",
            info.sdCSD.csdVersion == 0 ? "1.0 (SDSC)" :
            info.sdCSD.csdVersion == 1 ? "2.0 (SDHC/SDXC)" : "Unknown");
        printf("  TAAC:                0x%02X\n", info.sdCSD.taac);
        printf("  NSAC:                0x%02X\n", info.sdCSD.nsac);
        printf("  Transfer Speed:      0x%02X\n", info.sdCSD.tranSpeed);
        printf("  Command Classes:     0x%03X\n", info.sdCSD.ccc);
        printf("  Read Block Length:   %u (%u bytes)\n",
            info.sdCSD.readBlLen, 1u << info.sdCSD.readBlLen);
        if (info.sdCSD.csdVersion == 0)
        {
            printf("  C_SIZE (v1):         %u\n", info.sdCSD.cSizeV1);
            printf("  C_SIZE_MULT (v1):    %u\n", info.sdCSD.cSizeMultV1);
        }
        else
        {
            printf("  C_SIZE (v2):         %lu\n", info.sdCSD.cSizeV2);
        }
        {
            char capBuf[128];
            FormatBytes(static_cast<LONGLONG>(info.sdCSD.computedCapacityBytes), capBuf, sizeof(capBuf));
            printf("  Computed Capacity:   %s\n", capBuf);
        }
        printf("  Erase Block Enable:  %s\n", info.sdCSD.eraseBlkEn ? "Yes" : "No");
        printf("  Erase Sector Size:   %u\n", info.sdCSD.sectorSize);
        printf("  Write Protect Grp:   %u\n", info.sdCSD.wpGrpSize);
        printf("  WP Group Enable:     %s\n", info.sdCSD.wpGrpEnable ? "Yes" : "No");
        printf("  R2W Factor:          %u\n", info.sdCSD.r2wFactor);
        printf("  Write Block Length:  %u (%u bytes)\n",
            info.sdCSD.writeBlLen, 1u << info.sdCSD.writeBlLen);
        printf("  Copy Flag:           %u\n", info.sdCSD.copy);
        printf("  Perm Write Protect:  %s\n", info.sdCSD.permWriteProtect ? "Yes" : "No");
        printf("  Temp Write Protect:  %s\n", info.sdCSD.tmpWriteProtect ? "Yes" : "No");

        // SCR
        printf("\n  --- SD SCR Register (SD Configuration) ---\n");
        printf("  Raw:                 ");
        for (int i = 0; i < 8; i++) printf("%02X ", info.sdSCR.raw[i]);
        printf("\n");
        printf("  SCR Structure:       %u\n", info.sdSCR.scrStructure);
        {
            const char* specVer = "Unknown";
            if (info.sdSCR.sdSpec == 0) specVer = "1.0/1.01";
            else if (info.sdSCR.sdSpec == 1) specVer = "1.10";
            else if (info.sdSCR.sdSpec == 2 && !info.sdSCR.sdSpec3) specVer = "2.00";
            else if (info.sdSCR.sdSpec == 2 && info.sdSCR.sdSpec3 && !info.sdSCR.sdSpec4) specVer = "3.0x";
            else if (info.sdSCR.sdSpec == 2 && info.sdSCR.sdSpec3 && info.sdSCR.sdSpec4) specVer = "4.xx";
            if (info.sdSCR.sdSpecX > 0)
            {
                if (info.sdSCR.sdSpecX == 1) specVer = "5.xx";
                else if (info.sdSCR.sdSpecX == 2) specVer = "6.xx";
                else if (info.sdSCR.sdSpecX == 3) specVer = "7.xx";
                else if (info.sdSCR.sdSpecX == 4) specVer = "8.xx";
                else if (info.sdSCR.sdSpecX == 5) specVer = "9.xx";
            }
            printf("  SD Spec Version:     %s\n", specVer);
        }
        printf("  Data After Erase:    %u\n", info.sdSCR.dataStatAfterErase);
        printf("  Security:            %u\n", info.sdSCR.sdSecurity);
        printf("  Bus Widths:          ");
        if (info.sdSCR.sdBusWidths & 0x01) printf("1-bit ");
        if (info.sdSCR.sdBusWidths & 0x04) printf("4-bit ");
        printf("\n");
        printf("  SD Spec 3:           %s\n", info.sdSCR.sdSpec3 ? "Yes" : "No");
        printf("  SD Spec 4:           %s\n", info.sdSCR.sdSpec4 ? "Yes" : "No");
        printf("  CMD Support:         CMD20=%u CMD23=%u CMD48/49=%u CMD58/59=%u\n",
            info.sdSCR.cmdSupport & 1, (info.sdSCR.cmdSupport >> 1) & 1,
            (info.sdSCR.cmdSupport >> 2) & 1, (info.sdSCR.cmdSupport >> 3) & 1);

        // OCR
        printf("\n  --- SD OCR Register (Operation Conditions) ---\n");
        printf("  Raw:                 ");
        for (int i = 0; i < 4; i++) printf("%02X ", info.sdOCR.raw[i]);
        printf("\n");
        printf("  OCR Value:           0x%08lX\n", info.sdOCR.ocrValue);
        printf("  Voltage Window:      ");
        if (info.sdOCR.vdd27_28) printf("2.7-2.8V ");
        if (info.sdOCR.vdd28_29) printf("2.8-2.9V ");
        if (info.sdOCR.vdd29_30) printf("2.9-3.0V ");
        if (info.sdOCR.vdd30_31) printf("3.0-3.1V ");
        if (info.sdOCR.vdd31_32) printf("3.1-3.2V ");
        if (info.sdOCR.vdd32_33) printf("3.2-3.3V ");
        if (info.sdOCR.vdd33_34) printf("3.3-3.4V ");
        if (info.sdOCR.vdd34_35) printf("3.4-3.5V ");
        if (info.sdOCR.vdd35_36) printf("3.5-3.6V ");
        printf("\n");
        printf("  CCS (Capacity):      %s\n", info.sdOCR.ccs ? "SDHC/SDXC" : "SDSC");
        printf("  1.8V Switching:      %s\n", info.sdOCR.s18a ? "Accepted" : "No");
        printf("  UHS-II:              %s\n", info.sdOCR.uhs2CardStatus ? "Yes" : "No");
        printf("  Power-Up Status:     %s\n", info.sdOCR.busy ? "Ready" : "Busy");

        // SD Status
        printf("\n  --- SD Status (Extended) ---\n");
        printf("  Raw (64 bytes):      ");
        for (int i = 0; i < 16; i++) printf("%02X ", info.sdStatus.raw[i]);
        printf("...\n");
        printf("  Bus Width:           %s\n",
            info.sdStatus.datBusWidth == 0 ? "1-bit" :
            info.sdStatus.datBusWidth == 2 ? "4-bit" : "Unknown");
        printf("  Secured Mode:        %s\n", info.sdStatus.securedMode ? "Yes" : "No");
        printf("  Card Type:           0x%04X\n", info.sdStatus.sdCardType);
        printf("  Protected Area:      %lu bytes\n", info.sdStatus.sizeOfProtectedArea);
        printf("  Speed Class:         %u\n", info.sdStatus.speedClass);
        printf("  Performance Move:    %u MB/s\n", info.sdStatus.performanceMove);
        printf("  AU Size:             %u\n", info.sdStatus.auSize);
        printf("  Erase Size:          %u AU\n", info.sdStatus.eraseSize);
        printf("  Erase Timeout:       %u s\n", info.sdStatus.eraseTimeout);
        printf("  Erase Offset:        %u\n", info.sdStatus.eraseOffset);
        printf("  UHS Speed Grade:     %u\n", info.sdStatus.uhsSpeedGrade);
        printf("  UHS AU Size:         %u\n", info.sdStatus.uhsAuSize);
        printf("  Video Speed Class:   %u\n", info.sdStatus.videoSpeedClass);
        printf("  App Perf Class:      %u\n", info.sdStatus.appPerfClass);

        // Switch Function Status
        printf("\n  --- SD Switch Function Status ---\n");
        printf("  Raw (64 bytes):      ");
        for (int i = 0; i < 16; i++) printf("%02X ", info.sdSwitch.raw[i]);
        printf("...\n");
        printf("  Max Current:         %u mA\n", info.sdSwitch.maxCurrentConsumption);
        printf("  Access Mode Support: 0x%04X (", info.sdSwitch.funGroup1Support);
        if (info.sdSwitch.funGroup1Support & 0x01) printf("SDR12 ");
        if (info.sdSwitch.funGroup1Support & 0x02) printf("SDR25 ");
        if (info.sdSwitch.funGroup1Support & 0x04) printf("SDR50 ");
        if (info.sdSwitch.funGroup1Support & 0x08) printf("SDR104 ");
        if (info.sdSwitch.funGroup1Support & 0x10) printf("DDR50 ");
        printf(")\n");
        printf("  Current Access Mode: %u\n", info.sdSwitch.funGroup1Selection);
        printf("  Driver Strength:     0x%04X (current: %u)\n",
            info.sdSwitch.funGroup3Support, info.sdSwitch.funGroup3Selection);
        printf("  Current Limit:       0x%04X (current: %u)\n",
            info.sdSwitch.funGroup4Support, info.sdSwitch.funGroup4Selection);
        printf("  Command System:      0x%04X (current: %u)\n",
            info.sdSwitch.funGroup2Support, info.sdSwitch.funGroup2Selection);
        printf("  Data Struct Version: %u\n", info.sdSwitch.dataStructureVersion);
    }
}

// ============================================================
// Main
// ============================================================

static void RequireAdministrator()
{
    HANDLE hToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken))
        FatalError("OpenProcessToken failed — cannot check elevation status");

    TOKEN_ELEVATION elevation = {};
    DWORD cbSize = sizeof(TOKEN_ELEVATION);
    BOOL success = GetTokenInformation(hToken, TokenElevation,
        &elevation, sizeof(elevation), &cbSize);
    CloseHandle(hToken);

    if (!success)
        FatalError("GetTokenInformation(TokenElevation) failed");

    if (!elevation.TokenIsElevated)
        FatalErrorMsg("This program must be run as Administrator.\n"
            "  Right-click the executable and select \"Run as administrator\",\n"
            "  or launch from an elevated command prompt.");
}

int wmain()
{
    printf("SD Card Data Extraction Tool for Windows\n");
    printf("==========================================\n\n");

    RequireAdministrator();
    printf("Running as Administrator.\n\n");

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

        info.deviceNumber = QueryDeviceNumber(hDrive.get(), i);
        QueryStorageDeviceDescriptor(hDrive.get(), i, info.device);
        QueryStorageAdapterDescriptor(hDrive.get(), i, info.adapter);
        QueryDiskGeometry(hDrive.get(), i, info.geometry);
        QueryPartitionLayout(hDrive.get(), i, info.partitions);

        info.hasWriteCache = QueryWriteCacheProperty(hDrive.get(), i, info.writeCache);
        info.hasAccessAlignment = QueryAccessAlignmentProperty(hDrive.get(), i, info.accessAlignment);
        info.hasSeekPenalty = QuerySeekPenaltyProperty(hDrive.get(), i, info.seekPenalty);
        info.hasTrim = QueryTrimProperty(hDrive.get(), i, info.trim);
        info.hasPower = QueryDevicePowerProperty(hDrive.get(), i, info.power);
        info.hasMediumProductType = QueryMediumProductType(hDrive.get(), i, info.mediumProductType);
        info.hasIoCapability = QueryIoCapabilityProperty(hDrive.get(), i, info.ioCapability);
        info.hasDeviceTemperature = QueryDeviceTemperature(hDrive.get(), i, info.deviceTemperature);
        info.hasAdapterTemperature = QueryAdapterTemperature(hDrive.get(), i, info.adapterTemperature);
        info.hasMediaTypesEx = QueryMediaTypesEx(hDrive.get(), i, info.mediaTypesEx);

        // Match SetupDi info by device number
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

        info.volumes = FindVolumesOnDisk(info.deviceNumber);

        info.isSDCandidate =
            info.device.busType == BusTypeSd ||
            info.device.busType == BusTypeMmc ||
            (info.device.removableMedia && LooksLikeSDCardReader(info)) ||
            (info.device.busType == BusTypeUsb && info.device.removableMedia);

        drives.push_back(std::move(info));
    }

    printf("Found %zu physical drive(s).\n", drives.size());

    if (drives.empty())
        FatalErrorMsg("No physical drives found — this is unexpected on any Windows system.");

    // Step 2.5: Query SD registers for SD candidates via SFFDISK IOCTLs
    for (auto& drive : drives)
    {
        if (!drive.isSDCandidate || drive.volumes.empty())
            continue;

        printf("Querying SD registers for PhysicalDrive%lu...\n", drive.driveIndex);

        // Open volume handle for SFFDISK IOCTLs
        std::wstring volPath = drive.volumes[0].volumeGuid;
        if (!volPath.empty() && volPath.back() == L'\\')
            volPath.pop_back();

        HandleGuard hVol(CreateFileW(volPath.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_EXISTING, 0, nullptr));
        if (!hVol.valid())
            FatalError("Failed to open volume for SFFDISK commands");

        // Probe whether the driver supports SFFDISK IOCTLs at all.
        // These IOCTLs are only implemented by the Microsoft SD bus driver
        // stack (sdbus.sys + sffdisk.sys + sffp_sd.sys), which is loaded when
        // the host controller is an SDA-standard-compliant SD host (BusTypeSd).
        //
        // PCIe card readers (Realtek RtsPer.sys, Alcor, etc.) use their own
        // monolithic driver that presents the SD card as a SCSI device
        // (BusTypeScsi). These drivers do not implement the SFFDISK interface,
        // so the IOCTLs fail with ERROR_GEN_FAILURE (31) or similar.
        //
        // USB card readers (usbstor.sys) translate SD commands to SCSI/USB
        // mass storage protocol, completely abstracting away the SD layer.
        //
        // When SFFDISK is unavailable, SD card registers (CID, CSD, SCR, etc.)
        // can be read on Linux via sysfs: /sys/block/mmcblk0/device/cid etc.
        DWORD sffdiskError = 0;
        if (!QuerySD_Protocol(hVol.get(), drive.sdProtocolGUID, sffdiskError))
        {
            printf("\n");
            printf("  NOTE: SD card register queries are unavailable for PhysicalDrive%lu.\n", drive.driveIndex);
            printf("  IOCTL_SFFDISK_QUERY_DEVICE_PROTOCOL failed with Win32 error %lu (0x%08lX).\n",
                sffdiskError, sffdiskError);
            printf("\n");
            printf("  The SFFDISK interface requires the Microsoft SD bus driver stack\n");
            printf("  (sdbus.sys + sffdisk.sys), which is only loaded when the host\n");
            printf("  controller presents as an SDA-standard-compliant SD host (BusTypeSd).\n");
            printf("\n");
            printf("  This card reader reports BusType = %s (%d), which means it uses\n",
                BusTypeName(drive.device.busType), static_cast<int>(drive.device.busType));
            if (drive.device.busType == BusTypeScsi)
            {
                printf("  a proprietary PCIe driver (e.g. Realtek RtsPer.sys) that presents\n");
                printf("  the SD card as a SCSI device, bypassing the Microsoft SD stack.\n");
            }
            else if (drive.device.busType == BusTypeUsb)
            {
                printf("  the USB mass storage driver (usbstor.sys) which translates SD\n");
                printf("  commands to SCSI, completely abstracting away the SD protocol layer.\n");
            }
            else
            {
                printf("  a driver that does not expose the SD protocol layer via SFFDISK.\n");
            }
            printf("\n");
            printf("  SD card registers (CID, CSD, SCR, OCR, etc.) can instead be read\n");
            printf("  on Linux via sysfs, for example:\n");
            printf("    /sys/block/mmcblk0/device/cid\n");
            printf("    /sys/block/mmcblk0/device/csd\n");
            printf("    /sys/block/mmcblk0/device/scr\n");
            printf("\n");
            printf("  Skipping SD register queries. Raw disk imaging will still proceed.\n");
            printf("\n");
            continue;
        }

        drive.sdProtocolIsSD = (memcmp(&drive.sdProtocolGUID, &GUID_SFF_PROTOCOL_SD, sizeof(GUID)) == 0);
        drive.sdProtocolIsMMC = (memcmp(&drive.sdProtocolGUID, &GUID_SFF_PROTOCOL_MMC, sizeof(GUID)) == 0);

        // Query SD registers
        BYTE cidRaw[16] = {};
        QuerySD_CID(hVol.get(), cidRaw);
        ParseCID(cidRaw, drive.sdCID);

        BYTE csdRaw[16] = {};
        QuerySD_CSD(hVol.get(), csdRaw);
        ParseCSD(csdRaw, drive.sdCSD);

        BYTE scrRaw[8] = {};
        QuerySD_SCR(hVol.get(), scrRaw);
        ParseSCR(scrRaw, drive.sdSCR);

        BYTE ocrRaw[4] = {};
        QuerySD_OCR(hVol.get(), ocrRaw);
        ParseOCR(ocrRaw, drive.sdOCR);

        BYTE statusRaw[64] = {};
        QuerySD_Status(hVol.get(), statusRaw);
        ParseSDStatus(statusRaw, drive.sdStatus);

        BYTE switchRaw[64] = {};
        QuerySD_SwitchFunction(hVol.get(), switchRaw);
        ParseSwitchStatus(switchRaw, drive.sdSwitch);

        drive.hasSDRegisters = true;
        printf("SD registers queried successfully for PhysicalDrive%lu.\n", drive.driveIndex);
    }

    // Step 3: List removable drive letters via GetDriveType
    printf("\nRemovable drive letters (GetDriveType): ");
    {
        DWORD logicalDrives = GetLogicalDrives();
        if (logicalDrives == 0)
            FatalError("GetLogicalDrives returned 0");

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

    // Step 6: Raw disk imaging for each SD card candidate
    for (const auto& sdDrive : drives)
    {
        if (!sdDrive.isSDCandidate)
            continue;

        printf("\n================================================================\n");
        printf("  Raw Disk Imaging: PhysicalDrive%lu\n", sdDrive.driveIndex);
        printf("================================================================\n");

        // Lock and dismount all volumes on this drive
        std::vector<HandleGuard> lockedVolumes;
        for (const auto& vol : sdDrive.volumes)
        {
            std::wstring volPath = vol.volumeGuid;
            if (!volPath.empty() && volPath.back() == L'\\')
                volPath.pop_back();

            HandleGuard hVol(CreateFileW(volPath.c_str(),
                GENERIC_READ | GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE,
                nullptr, OPEN_EXISTING, 0, nullptr));
            if (!hVol.valid())
            {
                char msg[512];
                sprintf_s(msg, "Failed to open volume %ls for locking", volPath.c_str());
                FatalError(msg);
            }

            DWORD br = 0;
            if (!DeviceIoControl(hVol.get(), FSCTL_LOCK_VOLUME,
                nullptr, 0, nullptr, 0, &br, nullptr))
            {
                char msg[512];
                sprintf_s(msg, "FSCTL_LOCK_VOLUME failed on %ls", volPath.c_str());
                FatalError(msg);
            }

            if (!DeviceIoControl(hVol.get(), FSCTL_DISMOUNT_VOLUME,
                nullptr, 0, nullptr, 0, &br, nullptr))
            {
                char msg[512];
                sprintf_s(msg, "FSCTL_DISMOUNT_VOLUME failed on %ls", volPath.c_str());
                FatalError(msg);
            }

            lockedVolumes.push_back(std::move(hVol));
        }

        printf("  Locked and dismounted %zu volume(s).\n", lockedVolumes.size());

        // Open physical drive for raw reading
        WCHAR drivePath[64];
        swprintf_s(drivePath, L"\\\\.\\PhysicalDrive%lu", sdDrive.driveIndex);

        HandleGuard hRawDrive(CreateFileW(drivePath, GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_EXISTING,
            FILE_FLAG_NO_BUFFERING | FILE_FLAG_SEQUENTIAL_SCAN,
            nullptr));
        if (!hRawDrive.valid())
            FatalError("Failed to open physical drive for raw reading");

        // Create output file (no NO_BUFFERING to avoid alignment issues on last write)
        char outputPath[256];
        sprintf_s(outputPath, "sd_card_PhysicalDrive%lu_raw.img", sdDrive.driveIndex);

        HandleGuard hOutput(CreateFileA(outputPath, GENERIC_WRITE, 0,
            nullptr, CREATE_ALWAYS,
            FILE_FLAG_SEQUENTIAL_SCAN,
            nullptr));
        if (!hOutput.valid())
            FatalError("Failed to create output image file");

        // Allocate aligned buffer (VirtualAlloc returns page-aligned memory)
        const DWORD chunkSize = 4 * 1024 * 1024; // 4 MB
        LPVOID readBuf = VirtualAlloc(nullptr, chunkSize,
            MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!readBuf)
            FatalError("VirtualAlloc failed for read buffer");

        const LONGLONG totalBytes = sdDrive.geometry.diskSizeBytes;
        LONGLONG bytesRemaining = totalBytes;
        LONGLONG totalBytesRead = 0;

        char totalBuf[128];
        FormatBytes(totalBytes, totalBuf, sizeof(totalBuf));
        printf("  Output file:  %s\n", outputPath);
        printf("  Total size:   %s\n", totalBuf);
        printf("\n  Reading raw disk image...\n");

        LARGE_INTEGER perfFreq, startTime, now;
        QueryPerformanceFrequency(&perfFreq);
        QueryPerformanceCounter(&startTime);

        while (bytesRemaining > 0)
        {
            DWORD toRead = (bytesRemaining < (LONGLONG)chunkSize)
                ? (DWORD)bytesRemaining : chunkSize;

            // Round up to sector boundary for NO_BUFFERING
            DWORD sectorSize = sdDrive.geometry.bytesPerSector;
            if (sectorSize == 0) sectorSize = 512;
            toRead = ((toRead + sectorSize - 1) / sectorSize) * sectorSize;

            DWORD bytesRead = 0;
            if (!ReadFile(hRawDrive.get(), readBuf, toRead, &bytesRead, nullptr))
            {
                char msg[256];
                sprintf_s(msg, "ReadFile failed at offset %lld (read %lld of %lld bytes)",
                    totalBytesRead, totalBytesRead, totalBytes);
                FatalError(msg);
            }
            if (bytesRead == 0)
                break;

            DWORD bytesWritten = 0;
            if (!WriteFile(hOutput.get(), readBuf, bytesRead, &bytesWritten, nullptr))
                FatalError("WriteFile to image failed");
            if (bytesWritten != bytesRead)
                FatalErrorMsg("WriteFile wrote fewer bytes than expected");

            totalBytesRead += bytesRead;
            bytesRemaining -= bytesRead;

            // Progress every 256 MB
            if ((totalBytesRead % (256LL * 1024 * 1024)) < chunkSize)
            {
                double pct = 100.0 * totalBytesRead / totalBytes;
                QueryPerformanceCounter(&now);
                double elapsed = (double)(now.QuadPart - startTime.QuadPart) / perfFreq.QuadPart;
                double speed = (elapsed > 0) ? totalBytesRead / elapsed / (1024.0 * 1024.0) : 0.0;
                printf("  Progress: %.1f%% (%lld / %lld bytes, %.1f MB/s)\r",
                    pct, totalBytesRead, totalBytes, speed);
                fflush(stdout);
            }
        }

        VirtualFree(readBuf, 0, MEM_RELEASE);

        QueryPerformanceCounter(&now);
        double elapsed = (double)(now.QuadPart - startTime.QuadPart) / perfFreq.QuadPart;
        double speed = (elapsed > 0) ? totalBytesRead / elapsed / (1024.0 * 1024.0) : 0.0;

        printf("\n  Completed: %lld bytes read in %.1f seconds (%.1f MB/s)\n",
            totalBytesRead, elapsed, speed);

        // lockedVolumes goes out of scope here, releasing all locks via RAII
    }

    printf("\nDone.\n");
    return 0;
}
