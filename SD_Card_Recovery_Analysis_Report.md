# SD Card Forensic Analysis and Data Recovery Report

**Date:** February 12, 2026
**Subject:** HIKSEMI 64GB Micro SDXC Memory Card — Recovery of GoPro Video Footage (Turtle Swimming Video)
**Card Model:** HIKSEMI Micro SDXC V30 I, Part Number YAH06S0112SZ, Model HS2310064G C, Manufactured in China
**Image File:** `sd_card_PhysicalDrive1_raw.img` (62,921,900,032 bytes / 58.60 GB)

---

## 1. Background and History of the SD Card

This HIKSEMI 64GB Micro SDXC memory card was originally used inside a **GoPro action camera**, where it stored video footage including a recording of a turtle swimming. The card subsequently changed hands through at least two other devices:

1. **GoPro Action Camera** (original use) — Recorded MP4 video files, likely using an exFAT or FAT32 filesystem in the standard GoPro directory structure (`DCIM/100GOPRO/`, `DCIM/101GOPRO/`, etc.)
2. **Android Smartphone** — Someone inserted the SD card into an Android phone. The phone reformatted the card, creating a fresh exFAT filesystem with the volume label `"android"` and the standard Android directory hierarchy (Alarms, Android, Audiobooks, DCIM, Documents, Download, MISC, Movies, Music, Notifications, Pictures, Podcasts, Recordings, Ringtones)
3. **Car Infotainment System** — The card was also inserted into a car, though the car likely did not reformat or modify the card (car stereos typically read but do not reformat storage media)

The **Android phone** is the device that destroyed access to the original GoPro data. It reformatted the card and — critically — issued TRIM/DISCARD/ERASE commands that instructed the SD card's internal flash controller to logically erase all data blocks.

---

## 2. Raw Image Acquisition

The raw disk image was captured on a **Windows 11** desktop computer using a custom-built Win32 API tool (`recover_data_from_sd_card.exe`). The tool was developed specifically for this recovery effort and is documented in `main.cpp` in this repository.

### 2.1 Acquisition Hardware

- **Host Computer:** Windows 11 desktop with AMD-RAID storage array
- **Card Reader:** Realtek RTS5208 PCIe Card Reader (integrated into the motherboard/laptop)
  - **Bus Type:** SCSI (0x01) — The Realtek PCIe reader presents SD cards as SCSI devices rather than native SD devices
  - **Driver:** Realtek proprietary driver (`RtsPer.sys`), not the Microsoft SD bus driver stack (`sdbus.sys` + `sffdisk.sys`)
  - **Max Transfer Size:** 1,048,576 bytes (1 MB)
  - **Alignment Mask:** 0x00000007

### 2.2 Acquisition Method

The raw image was captured using the following Win32 API sequence:

1. **Volume Locking:** `FSCTL_LOCK_VOLUME` and `FSCTL_DISMOUNT_VOLUME` IOCTLs to obtain exclusive access and dismount the filesystem
2. **Direct Disk Reading:** `CreateFileW` with `FILE_FLAG_NO_BUFFERING | FILE_FLAG_SEQUENTIAL_SCAN` on `\\.\PhysicalDrive1`
3. **Page-Aligned I/O:** `VirtualAlloc` for page-aligned read buffers (required by `FILE_FLAG_NO_BUFFERING`)
4. **Sector-Aligned Reads:** 4 MB read chunks, rounded up to the 512-byte sector boundary
5. **Performance:** 62,921,900,032 bytes read in 685.7 seconds (87.5 MB/s average throughput)

### 2.3 SD Card Register Queries — Failed

The tool attempted to query the SD card's internal registers (CID, CSD, SCR, OCR) via `IOCTL_SFFDISK_QUERY_DEVICE_PROTOCOL` and `IOCTL_SFFDISK_DEVICE_COMMAND`. These IOCTLs are part of the Microsoft SFFDISK interface, which requires the Microsoft SD bus driver stack (`sdbus.sys` + `sffdisk.sys`).

The queries **failed with Win32 error 31** (`ERROR_GEN_FAILURE` / `0x0000001F` / "A device attached to the system is not functioning"). This is because the Realtek RTS5208 PCIe card reader uses its own proprietary driver (`RtsPer.sys`) that presents the SD card as a SCSI device (BusType = SCSI), completely bypassing the Microsoft SD stack. The SFFDISK IOCTLs are only functional when the host controller is an SDA-standard-compliant SD host that loads the Microsoft SD bus driver.

As a consequence, we were unable to read the card's **CID register** (which contains the manufacturer ID, OEM ID, product name, serial number, and manufacturing date) through software. The card's manufacturer identity was determined from the physical label instead.

---

## 3. Card Identification from Physical Label

Since the CID register was inaccessible, the card was identified from its printed label:

| Field | Value |
|---|---|
| **Brand** | HIKSEMI (subsidiary of Hikvision, established 2017) |
| **Type** | Micro SDXC (SD Extended Capacity, >32 GB) |
| **Capacity** | 64 GB (reported as 58.60 GB / 62,921,900,032 bytes by the controller) |
| **Speed Class** | V30 (Video Speed Class 30, minimum 30 MB/s sustained sequential write) |
| **UHS Class** | UHS-I (Ultra High Speed Phase I) |
| **Part Number** | YAH06S0112SZ |
| **Model** | HS2310064G C |
| **Country of Manufacture** | China |

### 3.1 Probable Internal Components

Based on research into HIKSEMI's product line and the card's specifications:

- **Flash Controller:** Most likely a **Silicon Motion SM2703** or similar UHS-I SD 3.0 controller. Silicon Motion is the dominant supplier of SD card controllers (58% market share) and the SM2703 is their cost-effective UHS-I offering designed for TLC NAND. HIKSEMI uses Silicon Motion controllers in their SSD products (SM2259 in the E100 SSD).
- **NAND Flash:** Likely **YMTC (Yangtze Memory Technologies Co.) TLC NAND**, as HIKSEMI's parent company Hikvision has a known supply relationship with YMTC (their HikSemi CC700 SSD uses 232-layer YMTC TLC NAND).

These identifications are educated inferences, not confirmed. The exact internal components can only be verified by physically opening the card.

---

## 4. Partition Table Analysis

### 4.1 Partition Table Type: MBR (Master Boot Record)

The SD card uses an **MBR (Master Boot Record)** partition table, not GPT (GUID Partition Table). This was confirmed by:

1. **MBR Signature:** Bytes `55 AA` present at offset `0x1FE–0x1FF` of sector 0
2. **No GPT Protective MBR:** No partition entry with type `0xEE` (GPT Protective)
3. **No GPT Header:** Sector 1 (offset 0x200) is all zeros — no `"EFI PART"` signature
4. **No Backup GPT:** Last sector of the disk is all zeros
5. **MBR Boot Code:** First 446 bytes of sector 0 are all zeros (no boot code, typical for removable media)
6. **MBR Disk Signature:** `0x00000000` (null, common for SD cards formatted by Android)

### 4.2 Partition Layout

The MBR contains a single partition entry:

| Field | Value |
|---|---|
| **Partition Number** | 1 |
| **Partition Type** | `0x07` (HPFS / NTFS / exFAT) |
| **Boot Indicator** | Inactive (`0x00`) |
| **Start Sector (LBA)** | 2048 (offset 1,048,576 bytes = 1.00 MB) |
| **End Sector (LBA)** | 122,894,302 |
| **Total Sectors** | 122,892,255 |
| **Partition Size** | 62,920,834,560 bytes (58.60 GB) |
| **CHS Start** | 0/32/33 |
| **CHS End** | 7649/208/14 |

The 2048-sector (1 MB) offset before the partition start is standard alignment for modern formatting tools.

### 4.3 No Hidden or Additional Partitions

TestDisk partition scanning confirmed only one partition exists. No evidence of:
- Previous partition tables
- Deleted partition entries
- Overlapping or hidden partitions
- GPT protective MBR remnants
- Extended partition chains

---

## 5. Filesystem Analysis

### 5.1 Filesystem Type: exFAT

The partition contains an **exFAT (Extended File Allocation Table)** filesystem. This was confirmed by the `"EXFAT   "` signature at offset `0x03` of the Volume Boot Record (VBR) at sector 2048.

### 5.2 exFAT Volume Boot Record Parameters

| Field | Value |
|---|---|
| **Jump Boot Code** | `EB 76 90` |
| **Filesystem Name** | `EXFAT   ` (8 bytes, padded with spaces) |
| **Partition Offset** | 2,048 sectors |
| **Volume Length** | 122,892,255 sectors (matches MBR partition size exactly) |
| **FAT Offset** | 2,048 sectors (relative to partition start, absolute sector 4,096) |
| **FAT Length** | 3,840 sectors |
| **Cluster Heap Offset** | 6,144 sectors (relative to partition start, absolute sector 8,192) |
| **Cluster Count** | 480,023 clusters |
| **First Cluster of Root Directory** | Cluster 4 |
| **Volume Serial Number** | `0x69E7BAF7` (matches `69E7-BAF7` reported by Windows) |
| **Filesystem Revision** | 1.00 |
| **Volume Flags** | `0x0000` |
| **Bytes Per Sector Shift** | 9 (2^9 = 512 bytes per sector) |
| **Sectors Per Cluster Shift** | 8 (2^8 = 256 sectors per cluster) |
| **Cluster Size** | 131,072 bytes (128 KB) — 512 bytes/sector x 256 sectors/cluster |
| **Number of FATs** | 1 |
| **Drive Select** | `0x80` |
| **Backup VBR Location** | Sector 2,060 (offset 12 from partition start) — confirmed present and valid |
| **Boot Signature** | `55 AA` at offset `0x1FE` of VBR |

### 5.3 Volume Label and Metadata

| Field | Value |
|---|---|
| **Volume Label** | `"android"` |
| **File System** | exFAT |
| **Total Filesystem Size** | 58.60 GB |
| **Used Space** | 886 MB (1.5%) |
| **Free Space** | 58 GB (98.5%) |

The volume label `"android"` confirms that an Android smartphone performed the most recent format operation. Android uses this label when formatting SD cards as "Portable Storage" (as opposed to "Adoptable Storage," which would use ext4 or f2fs with encryption).

### 5.4 Directory Structure

The filesystem contains only empty Android-standard directories. No user files exist:

```
/
+-- Alarms/
+-- Android/
|   +-- data/
|   |   +-- .nomedia
|   |   +-- com.amazon.mp3/files/Music/
|   |   +-- com.google.android.apps.nbu.files/files/
|   |   +-- com.google.android.apps.docs.editors.docs/files/
|   |   +-- com.google.android.apps.docs.editors.sheets/files/
|   |   +-- com.google.android.apps.docs.editors.slides/files/
|   |   +-- com.whatsapp/cache/
|   |   +-- com.google.android.youtube/cache/files/
|   |   +-- com.glance.lockscreenM/files/
|   |   +-- com.google.android.apps.maps/files/
|   |   +-- com.android.chrome/files/Download/
|   |   +-- com.spotify.music/cache/files/
|   +-- media/
|       +-- com.whatsapp/
+-- Audiobooks/
+-- DCIM/
+-- Documents/
+-- Download/
+-- MISC/
|   +-- thumb.data
+-- Movies/
|   +-- .thumbnails/.database_uuid, .nomedia
+-- Music/
|   +-- .thumbnails/.database_uuid, .nomedia
+-- Notifications/
+-- Pictures/
|   +-- .thumbnails/.database_uuid, .nomedia
+-- Podcasts/
+-- Recordings/
+-- Ringtones/
```

The presence of app data directories (WhatsApp, YouTube, Spotify, Google Chrome, Google Maps, Amazon Music, Google Docs/Sheets/Slides, Glance lock screen) indicates the Android phone had these apps installed and they created their default external storage directories on the SD card, but no actual user data was stored.

The `MISC/thumb.data` file and `.thumbnails/` directories with `.database_uuid` files are Android MediaStore artifacts. The `MISC/` directory with a modification timestamp of `January 2, 2017` is notable — this may be a remnant from a previous use of the card or an artifact of the Android formatting process.

---

## 6. Full-Disk Signature Scan

### 6.1 Methodology

A custom Rust program (`/tmp/sd_scan/`) was compiled and executed to scan the entire 58.60 GB raw image for known file signatures. The scanner was built for performance (compiled with `--release` optimizations) and completed the full scan in **238.8 seconds at 251 MB/s**.

The scanner searched for the following file type signatures across every byte of the image:

| Category | Signatures Searched |
|---|---|
| **Video** | MP4/MOV/3GP (`ftyp` box), HEIF/HEIC (`ftyp` + `heic`/`mif1`), MKV/WebM (EBML header `1A 45 DF A3`), AVI (`RIFF` + `AVI`), MPEG-TS (0x47 sync every 188 bytes), MPEG-PS (`00 00 01 BA`) |
| **Image** | JPEG (`FF D8 FF`), PNG (`89 50 4E 47`), GIF (`GIF87a`/`GIF89a`), WebP (`RIFF` + `WEBP`), BMP |
| **Audio** | MP3 ID3 (`ID3`), MP3 frame sync (`FF FB`/`FF F3`), OGG (`OggS`), FLAC (`fLaC`) |
| **Document** | PDF (`%PDF`), SQLite (`SQLite format 3`) |
| **Archive** | ZIP/APK/DOCX (`PK\x03\x04`), RAR (`Rar!`), GZIP (`1F 8B`), BZ2 (`BZh`) |
| **Filesystem** | exFAT VBR (`EXFAT`) |

### 6.2 Results

```
File signature scan results:
============================================================
  GZIP                : 2 hit(s)  [0x420F1D, 0x420F2D]

No hits: MP4/MOV, HEIF/HEIC, MKV/WebM, AVI, MPEG-TS, MPEG-PS,
         JPEG, PNG, GIF, WebP, BMP, MP3, MP3(ID3), OGG, FLAC,
         PDF, SQLite, ZIP/APK/DOCX, RAR, BZ2
```

**Zero video file signatures were found anywhere on the entire 58.60 GB disk image.**

No image files. No audio files. No documents. No archives. No databases. The only hits were 2 tiny GZIP fragments within the exFAT metadata area (filesystem internals, not user data), and the 2 expected exFAT VBR signatures (primary and backup boot sectors).

---

## 7. Erasure State Analysis

### 7.1 Byte-Level Statistics

The Rust scanner counted every byte across the entire image:

| Byte Value | Percentage | Amount | Meaning |
|---|---|---|---|
| `0xFF` | **99.9842%** | 58.59 GB | NAND flash erased state |
| `0x00` | 0.0155% | 0.01 GB | Zeroed areas (MBR boot code, exFAT reserved fields) |
| Other | 0.0003% | **0.16 MB** | Actual data (exFAT metadata only) |

### 7.2 What 0xFF Means

The value `0xFF` is the **native erased state of NAND flash memory**. When a NAND flash cell is erased (via an erase block operation), all bits are set to 1, producing the byte value `0xFF`. This is fundamentally different from being "zeroed" (`0x00`), which would require a deliberate write operation.

The fact that 99.98% of the card reads as `0xFF` means the SD card's internal flash controller has **logically erased virtually all data blocks**. When the Android phone reformatted the card, it issued erase/discard commands that caused the controller to:

1. Create a new exFAT filesystem (writing ~0.16 MB of metadata)
2. Mark all remaining blocks as "free" in its Flash Translation Layer (FTL)
3. Return `0xFF` for all reads to those blocks (Deterministic Read After Trim / DRAT behavior)

### 7.3 Spatial Distribution of Non-Erased Data

The scanner identified only two small regions in the data area (beyond the exFAT metadata) containing non-`0xFF` bytes:

| Region | Offset | Non-0xFF Bytes | Description |
|---|---|---|---|
| Region 1 | 0.812 GB | 513 bytes | Minimal — likely exFAT allocation bitmap or directory entry remnant |
| Region 2 | 58.562 GB | 16,896 bytes | Near end of disk — likely exFAT FAT table or backup metadata |

Both regions correspond to filesystem metadata, not user data.

### 7.4 Entropy Analysis

Entropy measurements were taken at multiple offsets across the image to characterize the data:

| Offset | Entropy (bits) | Classification | Dominant Byte |
|---|---|---|---|
| 0.00 GB (partition metadata) | 0.2861 | Low — filesystem metadata | `0x00` (63,387/65,536) |
| 0.00 GB (FAT area) | 2.3034 | Low — structured metadata | `0x00` (51,346/65,536) |
| 1.00 GB | 0.0000 | Zero — perfectly uniform | `0xFF` (65,536/65,536) |
| 5.00 GB | 0.0000 | Zero — perfectly uniform | `0xFF` (65,536/65,536) |
| 10.00 GB | 0.0000 | Zero — perfectly uniform | `0xFF` (65,536/65,536) |
| 20.00 GB | 0.0000 | Zero — perfectly uniform | `0xFF` (65,536/65,536) |
| 30.00 GB | 0.0000 | Zero — perfectly uniform | `0xFF` (65,536/65,536) |
| 40.00 GB | 0.0000 | Zero — perfectly uniform | `0xFF` (65,536/65,536) |
| 50.00 GB | 0.0000 | Zero — perfectly uniform | `0xFF` (65,536/65,536) |

An entropy of exactly `0.0000` means every single byte in the sampled 64 KB region is identical (`0xFF`). There is zero information content. This rules out encryption (which would produce entropy near 8.0) and confirms a genuine flash erase.

### 7.5 Hex Dump Confirmation

Direct hex dumps at 1 GB, 10 GB, and 30 GB offsets confirmed the entire data area is uniformly `0xFF`:

```
40000000: ffff ffff ffff ffff ffff ffff ffff ffff  ................
40000010: ffff ffff ffff ffff ffff ffff ffff ffff  ................
...
(pattern continues for the entire data area)
```

---

## 8. Why Standard Data Recovery Software Cannot Work

### 8.1 How Conventional File Recovery Works

Standard data recovery software (PhotoRec, Recuva, R-Studio, TestDisk, Disk Drill, etc.) works by scanning raw disk sectors for known file signatures (file headers like the `ftyp` box in MP4 files or the `FF D8 FF` JPEG header). When a file is "deleted" normally, only the filesystem metadata (directory entry, FAT/MFT record) is removed — the file's actual data blocks remain on disk until overwritten by new data.

### 8.2 Why It Fails Here

In this case, recovery software has nothing to find. The file data blocks themselves have been returned as `0xFF` by the controller. Our comprehensive scan of all 62.9 billion bytes found zero video file signatures. There are no MP4 headers, no H.264/H.265 video frame data, no `moov` atoms, no `mdat` atoms — nothing. The conventional recovery approach is completely inapplicable.

### 8.3 PhotoRec Confirmation

PhotoRec was run against the raw image in command-line mode searching for video file types (MOV, MP4, AVI, MKV, 3GP, WMV, FLV). It found **zero recoverable files**, confirming the signature scan results.

---

## 9. The Controller vs. The NAND: Why Data May Still Exist

### 9.1 Architecture of an SD Card

An SD card is not a simple storage chip. It contains at minimum two components:

1. **NAND Flash Memory Die** — The actual storage silicon containing billions of flash cells organized into pages (typically 4–16 KB) and blocks (typically 256 KB – 4 MB)
2. **Flash Controller Microprocessor** — A dedicated ARM or 8051-based CPU running proprietary firmware that manages all host communication, wear leveling, error correction (ECC/BCH), bad block management, and the Flash Translation Layer (FTL)

The host computer (or phone, or camera) never communicates directly with the NAND flash. Every read and write goes through the controller, which translates logical block addresses (LBAs) to physical NAND page addresses using its internal FTL mapping table.

### 9.2 What TRIM/DISCARD Actually Does

When the Android phone issued TRIM/DISCARD/ERASE commands during formatting:

1. **The controller updated its FTL** — It removed the logical-to-physical mapping entries for all data blocks, marking them as "free" in its internal tables
2. **The controller began returning `0xFF`** — For any subsequent read to those logical addresses, the controller returns `0xFF` (the DRAT/RZAT behavior) without actually reading the NAND
3. **The NAND pages may NOT have been physically erased yet** — NAND flash can only be erased in large blocks (typically 256 KB – 4 MB). Physical erasure (called "block erase") is an expensive operation that the controller defers and batches through a background process called "garbage collection"

### 9.3 The Critical Distinction

| Layer | State After TRIM |
|---|---|
| **Logical (what the host sees)** | All `0xFF` — data appears completely gone |
| **Physical (actual NAND silicon)** | Old data pages may still be intact, just unmapped by the FTL |

This is the fundamental reason why chip-off recovery can work: the controller is reporting "empty" for blocks that may still physically contain data on the NAND die.

### 9.4 Factors Favoring Recovery

Several factors suggest the GoPro video data may still physically exist on the NAND:

1. **No new data was written** — The Android phone only wrote ~0.16 MB of filesystem metadata. The remaining 58+ GB of NAND was not overwritten with new data
2. **Wear leveling preserves old copies** — The controller distributes writes across the NAND to extend lifespan. Old data pages from the GoPro usage are scattered across the physical NAND and are not necessarily targeted for immediate erasure
3. **Garbage collection is lazy** — Most consumer SD card controllers do not aggressively erase freed blocks. They erase blocks on-demand when new write space is needed. Since very little was written after the format, few blocks would have been garbage-collected
4. **The card has been idle** — Garbage collection typically runs during idle periods. If the card was removed from the phone promptly and has been sitting unused, fewer blocks would have been erased

### 9.5 Factors Working Against Recovery

1. **Time elapsed** — Some controllers perform background garbage collection even without host write activity
2. **Unknown controller behavior** — The HIKSEMI card's Silicon Motion controller may have an aggressive garbage collection policy
3. **NAND block erase granularity** — If the controller did erase some blocks, entire 256 KB+ regions are lost at once
4. **FTL reconstruction difficulty** — Even if NAND pages are intact, reconstructing the logical file from scattered physical pages requires reverse-engineering the FTL algorithm

---

## 10. Approaches Investigated for Bypassing the Controller

### 10.1 Controller Firmware Exploit (CMD63 Knock Sequence)

**Research source:** [Bunnie Huang's 30C3 presentation "On Hacking MicroSD Cards"](https://www.bunniestudios.com/blog/2013/on-hacking-microsd-cards/) (2013)

Bunnie Huang and xobs demonstrated that some SD card controllers can be put into a firmware loading mode using vendor-reserved SD commands. On **Appotech AX211/AX215** controllers, sending the SD command `CMD63` followed by the bytes `'A','P','P','O'` causes the controller to accept 512 bytes of arbitrary code and execute it on its internal 8051 microprocessor.

Custom firmware running on the controller would have unrestricted access to the raw NAND, including unmapped pages. In theory, one could upload code that:
- Ignores the FTL mapping
- Reads every physical NAND page sequentially
- Streams all NAND content (including "erased" pages) back to the host through the normal SD data interface

**Verdict: Not applicable to this card.** The CMD63 + `'APPO'` knock sequence is specific to Appotech AX211/AX215 controllers. The HIKSEMI card almost certainly uses a **Silicon Motion** controller, which has a different (and publicly unknown) firmware loading mechanism. Other known controller manufacturers (Phison, SMI, Alcor Micro, Samsung, SanDisk) each have their own proprietary mechanisms, and no general-purpose "universal controller exploit" tool exists publicly.

### 10.2 Vendor-Specific CMD56 (General Command)

**Research source:** [Linux kernel mmc-utils CMD56 support patches](https://lore.kernel.org/lkml/20220417110934.621035-1-huobean@gmail.com/T/)

The SD specification defines CMD56 (GEN_CMD) as a vendor-specific general-purpose command. Some manufacturers (notably SanDisk for their Industrial microSD cards) implement CMD56 to expose internal health status registers, NAND wear counters, and diagnostic information.

However, CMD56 is designed for **health monitoring, not raw NAND access**. It returns manufacturer-defined diagnostic data (bad block counts, erase cycle counts, etc.) but does not provide a mechanism to read arbitrary NAND pages. Additionally, CMD56 support is vendor-specific and many consumer SD cards (including newer SanDisk and Samsung models) do not respond to CMD56 at all.

**Verdict: Unlikely to help.** Even if the HIKSEMI card responds to CMD56, the returned data would be health diagnostics, not recoverable file data. Additionally, the card would need to be accessed through a Linux system with a native MMC/SD driver (not the Realtek PCIe SCSI passthrough) to send raw SD commands.

### 10.3 SFFDISK IOCTLs (Windows SD Bus Driver Stack)

The Windows SFFDISK interface (`IOCTL_SFFDISK_DEVICE_COMMAND`) can send arbitrary SD commands (including CMD56) to the card, but only when the card reader uses the **Microsoft SD bus driver stack** (`sdbus.sys` + `sffdisk.sys` + `sffp_sd.sys`). This stack is loaded only for SDA-standard-compliant SD host controllers that report `BusType = BusTypeSd`.

The Realtek RTS5208 PCIe card reader reports `BusType = SCSI (0x01)` and uses its proprietary `RtsPer.sys` driver, which does not implement the SFFDISK interface. All SFFDISK IOCTL calls failed with Win32 error 31 (`ERROR_GEN_FAILURE`).

**Verdict: Not possible with this card reader hardware.** A different card reader that uses the Microsoft SD bus driver stack would be required. Alternatively, on Linux, a card reader that exposes the card as `/dev/mmcblk0` (using the `sdhci` or `rtsx_pci` MMC driver) would allow raw SD command access via `mmc-utils`.

### 10.4 Linux MMC Subsystem

On Linux, SD cards connected through native MMC host controllers are accessible via the MMC subsystem, which exposes:
- `/sys/block/mmcblk0/device/cid` — Card Identification register
- `/sys/block/mmcblk0/device/csd` — Card Specific Data register
- `/sys/block/mmcblk0/device/scr` — SD Configuration register
- Raw SD command access via `mmc-utils` or `ioctl()` on `/dev/mmcblk0`

This would allow reading the card's CID (to confirm the controller manufacturer) and potentially sending vendor-specific commands. However, the Linux system used for analysis does not have the SD card physically connected (the card is with the Windows 11 laptop), and the Realtek PCIe reader may not present the card as an MMC device on Linux either (it depends on whether the `rtsx_pci_sdmmc` kernel module is loaded).

**Verdict: Potentially useful for diagnostics but would not recover data.** Even with full raw SD command access, the controller's FTL still governs what data is returned for standard read commands. Vendor-specific commands (if they exist for the Silicon Motion controller) are not publicly documented.

---

## 11. The Only Viable Recovery Path: NAND Chip-Off

### 11.1 What Chip-Off Recovery Is

Chip-off recovery involves physically removing the NAND flash memory die from the SD card's PCB (printed circuit board) and reading it directly with specialized hardware, completely bypassing the flash controller. The raw NAND dump is then processed with software that reverse-engineers the FTL to reconstruct logical files from physical page data.

### 11.2 The Process

1. **Physical Decapsulation** — The SD card's plastic housing is removed to expose the PCB
2. **NAND Chip Desoldering** — The NAND flash die is carefully desoldered from the PCB using hot air rework or infrared heating (microSD cards are often "monolithic," meaning the controller and NAND are bonded together, requiring specialized techniques)
3. **NAND Pin Mapping** — The NAND chip's pinout is identified (data lines, address lines, chip enable, write enable, read enable, etc.)
4. **Raw NAND Dump** — A NAND reader (such as the ACE Lab PC-3000 Flash with SpiderBoard adapter) reads every physical page of the NAND, including:
   - Pages that are logically mapped (current data)
   - Pages that are logically unmapped (old data, including TRIMmed GoPro video)
   - Spare area / out-of-band (OOB) data containing ECC codes and FTL metadata
   - Reserved blocks containing the controller's firmware and FTL tables
5. **Data Unwhitening** — The controller applies XOR-based "data whitening" (entropy distribution) to prevent adjacent NAND cells from having correlated values. This XOR mask must be identified and reversed
6. **ECC Correction** — BCH or LDPC error-correcting codes are applied to fix bit errors in the raw NAND data
7. **FTL Reconstruction** — The logical-to-physical mapping is reconstructed from the FTL metadata stored in the NAND's reserved area and OOB data. For TRIMmed data, old mapping entries may still be recoverable from previous FTL snapshots or wear-leveling copies
8. **File Carving** — MP4 video files are reconstructed from the recovered logical image using standard file carving techniques

### 11.3 Professional Tools and Services

The industry-standard professional tool for this work is the **ACE Lab PC-3000 Flash** system:

- **Hardware:** PC-3000 Flash reader board with SpiderBoard adapter for microSD
- **Software:** Visual NAND Reconstructor — maintains a database of FTL algorithms for hundreds of controller types
- **Cost of equipment:** ~$5,500 (purchased by professional labs)
- **Cost of service:** $300 – $1,500 per recovery, depending on the lab and complexity
- **Many labs offer "no data, no fee" policies** — you only pay if they successfully recover your files

### 11.4 Recommended Professional Labs — Israel

The following Israeli data recovery labs are listed in order of overall confidence for this specific case (monolithic microSD chip-off recovery), based on verified equipment, independent reputation, certifications, and transparent pricing. Rankings account for both technical capability and verifiable trustworthiness.

#### 1. UDATA — Tel Aviv (Best Verified Chip-Off Capability)

- **Website:** [udata.co.il](https://xn--9dbbc4eb.co.il/)
- **Location:** Ha'Me'apilim Street 33, Tel Aviv 6802833
- **Phone:** 072-249-4570
- **Email:** sos@udata.co.il
- **Hours:** Sun–Thu 9:00–18:00, Fri 9:00–12:00

UDATA confirms use of the **PC-3000 Flash** and **Rusolut Visual NAND Reconstructor (VNR)** — the industry-standard tool for reconstructing data from raw NAND dumps, handling XOR unwhitening, ECC correction, and FTL reconstruction. This confirms they can perform the full chip-off pipeline (physical NAND read + data reconstruction), not just the physical read. ISO-standard clean room with security cameras and restricted access, 3 expert technicians, infrared soldering station for SMD work. NDA available for sensitive data.

**Published pricing** (all prices **exclude 17% VAT** — their site explicitly states "כל המחירים במחירון שלנו - לא כוללים מע"מ"):

| Service | Price (NIS, excl. VAT) |
|---|---|
| **Standard diagnostic** | 100 |
| **Rush diagnostic** (4 hours) | 250 |
| **Previously-opened device inspection** | 300 |
| Logical damage (corruption, deletion) | 550–1,800 |
| Post-format recovery | 350–1,500 |
| Physical damage | 800–2,200 |
| **Chip-off (up to 64 GB)** | **1,800–2,500** |
| **Chip-off (64–250 GB)** | **2,500–4,500** |
| SSD chip-off extraction | 3,500–9,500 |
| **No-data-no-fee** | Yes |
| **Claimed success rate** | 97% |

**Estimated total cost for this specific card (HIKSEMI 64 GB microSD):**

| Component | NIS |
|---|---|
| Diagnostic fee | 100 |
| Chip-off recovery ("up to 64 GB" tier) | 1,800–2,500 |
| **Subtotal before VAT** | **1,900–2,600** |
| VAT (17%) | 323–442 |
| **Total including VAT** | **2,223–3,042 NIS (~$609–$833 USD)** |

**Pricing ambiguity to clarify by phone:** The card is exactly 64 GB. UDATA's tiers are "up to 64 GB" (1,800–2,500 NIS) and "64–250 GB" (2,500–4,500 NIS). If they interpret "up to" as strictly less than 64, the card would fall in the higher tier, pushing the worst case to **5,265 NIS including VAT (~$1,442 USD)**. This is the most important question to ask when calling.

**Additional cost considerations:**
- It is unclear whether the 100 NIS diagnostic fee is credited toward the recovery cost or added on top — ask when calling
- Recovered data is delivered on a replacement drive/USB (not the original card) — unclear if the cost of this media is included
- Drop-off at the Tel Aviv lab is free; nationwide pickup service is available but cost is not published
- The "previously-opened device" fee (300 NIS) does not apply since this card has not been physically opened

**Questions to ask when calling 072-249-4570:**
1. Does a 64 GB card fall in the "up to 64 GB" or "64–250 GB" chip-off tier?
2. Is the 100 NIS diagnostic fee credited toward the recovery cost if I proceed?
3. Have you done monolithic microSD chip-off recovery before (not standard TSOP chip-off)?
4. Is recovery media (USB drive for the recovered files) included in the price?
5. What is the estimated timeline for chip-off recovery?
6. If recovery fails, do I still owe the diagnostic fee?

**Why #1:** Confirmed PC-3000 Flash + Rusolut VNR on-site (full chip-off pipeline), explicit chip-off pricing by capacity, documented clean room, multi-person team. The transparent pricing and dedicated flash recovery focus inspire the most confidence for this specific job.

**For comparison with alternatives:**

| Lab | Chip-off price for 64 GB (excl. VAT) | Total incl. 17% VAT |
|---|---|---|
| **UDATA (Tel Aviv)** | 1,800–2,500 NIS | 2,106–2,925 NIS (~$577–$801) |
| **ZE-LAB (Haifa)** | 1,500–4,500 NIS | 1,755–5,265 NIS (~$481–$1,442) |
| **Flash Recovery (Herzliya)** | 800–1,800 NIS (physical tier; chip-off may be higher) | 936–2,106 NIS (~$256–$577) |
| **$300 Data Recovery (US)** | ~$300 USD flat (~1,095 NIS) | N/A (no Israeli VAT) + intl. shipping |

#### 2. Tik Tac Technologies — Ramat Gan (Most Established Lab in Israel)

- **Website:** [tictac.co.il](https://www.tictac.co.il/) / [en.tictac.co.il](https://en.tictac.co.il/)
- **Location:** 10 Hataas Street, Ground Floor, Ramat Gan 52512 (Diamond Exchange complex)
- **Phone:** 03-613-1555
- **Emergency mobile:** 052-287-7477
- **Email:** sos@tictac.co.il
- **Hours:** Sun–Thu 8:00–18:00

Israel's largest data recovery lab (est. 1995), handling an estimated 50%+ of all Israeli data recovery cases. **ISO 9001:2015 certified. Class 100 clean room.** Verified supplier to the Israeli Ministry of Defense, Prime Minister's Office, Israel Police, universities (including Tel Aviv University), and hospitals. Published case categories and recovery statistics. Extensive independently verifiable track record over 25+ years.

Chip-off is not explicitly listed on their public website, but their scale, Class 100 clean room, and description of "unique technologies for recovering damaged memory cards considered advanced and leading in the world" make it very likely they have the equipment. **Call 03-613-1555 to confirm chip-off capability for monolithic microSD before visiting.**

| | |
|---|---|
| **Evaluation fee** | 500 NIS for physically opened media |
| **Recovery pricing** | Quote-based (not publicly listed) |
| **No-data-no-fee** | Yes |
| **Claimed success rate** | 97% |

**Why #2:** By far the most credible and established lab in Israel. The only concern is that chip-off is not explicitly listed on their site — they may handle it in-house or may outsource it. A single phone call will clarify. If they confirm chip-off capability, they are arguably the safest choice overall due to their institutional track record and certifications.

#### 3. Flash Recovery (Techno Data) — Herzliya (Confirmed Equipment, Unverified Track Record)

- **Website:** [flash-recovery.co.il](https://flash-recovery.co.il/recovering-data/sd-microsd.html)
- **Location:** Sokolov Street 10, Herzliya
- **Phone:** 052-529-2863 (cellular)
- **Email:** Flashrecovery15@gmail.com / info@flash-recovery.co.il
- **WhatsApp:** Available
- **Contact:** Yuri

Flash Recovery is the only Israeli lab that **explicitly lists on their website** the **ACE Lab PC-3000 Flash** system with the **Spider Board Adapter**, **Monolith Module**, BGA-48, BGA-152, and TSOP-56 adapters, and describes performing work under a microscope for solder points under 1 mm. The technical descriptions on their SD card recovery page demonstrate genuine knowledge of NAND flash recovery methodology.

| | |
|---|---|
| **Diagnostic** | Free |
| **Logical damage** (corruption, deletion, formatting) | 300–800 NIS (~$80–$220 USD) |
| **Physical damage** (undetected cards, broken controllers) | 800–1,800 NIS (~$220–$500 USD) |
| **No-data-no-fee** | Yes — "Customers pay only for recovered data" |

Note: Chip-off for a monolithic microSD may be quoted above the standard physical damage tier. Call to confirm pricing for this specific case.

**Why #3 despite listing the right equipment:** Independent due diligence revealed several concerns that prevent a higher ranking:

- **Near-zero independent reviews** — only 2 reviews found (on Dapei Zahav, 5.0 stars). No Google Business reviews found. No social media presence (no Facebook, Instagram, or LinkedIn).
- **Not listed in any industry directory** — absent from Fastbase (77 Israeli data recovery companies), Ensun (39 companies), B144 (Bezeq directory), Dolphin Data Lab, and Data Recovery Salon.
- **No clean room documented** — despite advertising HDD head replacement (which requires a clean room), no clean room is mentioned anywhere on their site or in external listings.
- **Small operation** — appears to be a one-person shop (cellular number only, Gmail as secondary email). Also provides VHS digitization and general computer repair services, suggesting a generalist rather than a specialist.
- **Website padded with generic SEO content** — dozens of unrelated consumer electronics articles (TV rankings, phone reviews) that appear template-generated.
- **Unverifiable institutional claims** — claims to be an "authorized supplier to Tel Aviv University" but this could not be independently confirmed.
- **No published case studies** — zero documented recovery examples.

**Recommendation:** The free diagnostic makes this low-risk to try. When calling, ask directly: *"Is the PC-3000 Flash on-site right now? How many monolithic microSD chip-off recoveries have you completed?"* The specificity of the answer will indicate whether the equipment listing reflects current, active capability or aspirational marketing.

#### 4. ZE-LAB — Haifa

- **Website:** [zelab.co.il](https://zelab.co.il/data-recovery/flash)
- **Location:** Nativ HaLulav 12, Haifa 2634227
- **Phone:** 073-787-7730 / 058-727-2737
- **Email:** [email protected]
- **Hours:** Sun–Thu 09:00–21:00, Fri 09:00–13:00 (24/6 emergency availability)

ZE-LAB explicitly lists **"Direct NAND chip reading (Chip-off)"** as a service on their website. Specific PC-3000 Flash equipment is not confirmed publicly but chip-off capability is directly stated.

| | |
|---|---|
| **Diagnostic** | Free (up to 3 business days) |
| **Logical errors / formatting** | 750–1,500 NIS |
| **Chip-off NAND reading** | **1,500–4,500 NIS (~$415–$1,240 USD)** |
| **Complex repairs requiring soldering** | 2,500+ NIS |
| **No-data-no-fee** | Not confirmed on website — ask directly |

#### 5. Recoverli — Petah Tikva

- **Website:** [recoverli.co.il](https://recoverli.co.il/)
- **Location:** Aryeh Shnecor 3, Petah Tikva
- **Phone:** 077-205-0000 / 050-740-0003
- **Email:** [email protected]
- **Hours:** 24/7

Mentions chip-off on their website: *"In particularly complex cases, performs Chip-Off recovery — direct extraction of the memory chip and professional reading of it."* ISO 14644-1 Class 1005 clean room. 166+ Google reviews — the strongest independent review presence of any lab on this list. Specific equipment not confirmed publicly.

| | |
|---|---|
| **Diagnostic** | Free (deducted from recovery cost) |
| **Recovery pricing** | Quote-based |
| **No-data-no-fee** | Yes |
| **Claimed success rate** | 97% |
| **Turnaround** | 24–72 hours (standard cases) |

#### 6. WIMD (Where Is My Data) — Tel Aviv area

- **Website:** [wimd.co.il](https://www.wimd.co.il/)
- **Phone:** 03-678-6568

Mentions chip-off and JTAG/ISP techniques. Authorized supplier to the Israeli Ministry of Defense. Established early 2000s. Claims 99% success rate. Specific equipment and pricing not publicly listed — call for details.

| | |
|---|---|
| **No-data-no-fee** | Yes |
| **Claimed success rate** | 99% |

#### Suggested approach

1. **Call UDATA first** (072-249-4570) — confirm they can do chip-off on a HIKSEMI monolithic microSD, get a firm quote within their published 1,800–2,500 NIS range for 64 GB.
2. **Call Tik Tac second** (03-613-1555) — ask if they do monolithic microSD chip-off in-house. If yes, get a quote for comparison.
3. **If both decline or quote too high**, try Flash Recovery's free diagnostic (052-529-2863) — ask the questions above about on-site equipment and monolithic recovery experience.
4. Take the card to whichever lab gives the most specific, confident answer about monolithic microSD chip-off experience. Vague answers like "we'll try" are a red flag for this type of recovery.

### 11.4.1 Recommended Professional Labs — United States (International)

For reference, the following US-based labs are well-documented for microSD chip-off and may be contacted for a second opinion or if Israeli labs cannot recover the data:

| Lab | URL | Notes |
|---|---|---|
| **Rossmann Group** | [rossmanngroup.com/microsd-data-recovery](https://rossmanngroup.com/microsd-data-recovery) | Uses ACE Lab SpiderBoard + PC-3000 Flash for microSD, documented video teardowns of the process |
| **$300 Data Recovery** | [300dollardatarecovery.com/chip-off-data-recovery](https://www.300dollardatarecovery.com/chip-off-data-recovery/) | Flat $300 chip-off rate, no data no fee |
| **PITS Data Recovery** | [pitsdatarecovery.com](https://www.pitsdatarecovery.com/blog/micro-sd-chip-off-recovery/) | ISO Certified Class 10 Cleanroom, free evaluation |
| **Gillware** | [gillware.com](https://www.gillware.com/flash-drive-data-recovery/flash-memory-amnesia-resurrecting-data-through-direct-read-of-nand-memory/) | Published detailed technical documentation on NAND recovery from TRIMmed/formatted flash devices |

### 11.5 DIY Chip-Off (Advanced)

[Joshua Wise's ndfrecovery project](https://joshuawise.com/projects/ndfrecovery) demonstrates that a hobbyist with electronics experience can perform chip-off recovery. His project:

- **Recovered 100% of photos and videos** from a broken SD card
- **Hardware cost:** ~$100–200 (Digilent Nexys-2 FPGA board, Schmartboard breakout, wire)
- **Time:** Approximately 6 months (but he was reverse-engineering everything from scratch; with his published code and the PC-3000 Flash's controller database as reference, it could be significantly faster)
- **Open-source code:** [github.com/jwise/ndfslave](https://github.com/jwise/ndfslave) — includes FPGA Verilog code, NAND dumping tools, FTL reconstruction software, ECC correction (BCH), data whitening reversal, and a custom FAT filesystem driver with recovery workarounds
- **NAND dump throughput:** ~1.2 MB/s (24 GB in ~6.5 hours)

This approach requires: soldering skill (particularly for microSD monolithic packages), an FPGA development board, patience for FTL reverse-engineering, and comfort with low-level embedded systems work.

---

## 12. What to Tell the Recovery Lab

When contacting a professional recovery lab, provide the following information:

> **Card:** HIKSEMI 64GB Micro SDXC V30, Part Number YAH06S0112SZ, Model HS2310064G C
>
> **History:** The card was used in a GoPro action camera to record video (including a specific turtle swimming video). The card was subsequently inserted into an Android smartphone, which reformatted it as exFAT with the volume label "android." The card was also inserted into a car infotainment system.
>
> **Current state:** A complete raw disk image was captured. The entire data area (99.98% of the 58.60 GB card) reads as `0xFF` through the standard SD interface. No file signatures of any kind (MP4, JPEG, or otherwise) are present in the raw image. The card contains only empty Android directory structures (~0.16 MB of exFAT metadata).
>
> **What happened:** The Android phone issued TRIM/DISCARD/ERASE commands during formatting, causing the controller to logically unmap all data blocks and return `0xFF` for all reads. However, the NAND flash pages were likely not physically erased (no new data was written to the card beyond the small exFAT metadata). The GoPro MP4 video data should still exist on unmapped physical NAND pages.
>
> **Request:** NAND chip-off / PC-3000 Flash recovery to read all physical NAND pages, reconstruct the FTL mapping, and recover the original GoPro MP4 video files.

---

## 13. Preservation Instructions

To maximize the chances of successful recovery:

1. **Do NOT use the SD card in any device** — Do not insert it into any phone, camera, computer, or card reader. Every mount, even read-only, may trigger the controller's background garbage collection, which could physically erase NAND pages containing the old GoPro data
2. **Do NOT reformat the card** — Any format operation may write new metadata to NAND pages that previously contained video data
3. **Do NOT attempt software recovery tools on the live card** — Tools like PhotoRec or Recuva cannot help here, and mounting the card risks triggering garbage collection
4. **Store the card safely** — Keep it in a static-free container at room temperature. NAND flash retains data for years without power, but extreme temperatures can accelerate charge leakage from flash cells
5. **The raw image file (`sd_card_PhysicalDrive1_raw.img`) can be deleted if storage space is needed** — It contains only `0xFF` and has no recovery value. The physical card itself is the artifact that matters

---

## 14. Summary of Conclusions

| Question | Answer |
|---|---|
| **Partition table type?** | MBR (Master Boot Record), single partition |
| **Filesystem?** | exFAT, 128 KB clusters, volume label "android" |
| **What device formatted the card?** | An Android smartphone (confirmed by volume label and directory structure) |
| **Are there existing video files?** | No. Zero video files exist on the filesystem |
| **Are there recoverable file signatures?** | No. Zero file signatures of any kind found across all 58.60 GB |
| **What is the erasure state?** | 99.9842% of the card is `0xFF` (NAND flash erased state) |
| **Is the data encrypted?** | No. Entropy analysis confirms `0xFF` fill, not encrypted data |
| **Can software recover the data?** | **No.** No software tool can recover data that the controller returns as `0xFF` |
| **Does the data still physically exist?** | **Probably yes.** The NAND pages were likely logically unmapped but not physically erased |
| **Can the controller be tricked?** | **No.** No publicly known firmware exploit exists for the likely Silicon Motion controller in this card |
| **What is the only viable recovery method?** | **Professional NAND chip-off recovery** using ACE Lab PC-3000 Flash or equivalent direct-NAND reading hardware |
| **Estimated cost?** | $300 – $1,500, most labs offer no data/no fee |
| **Likelihood of success?** | Moderate to good — the card was only reformatted (not filled with new data), so NAND pages likely retain the original GoPro data |

---

## 15. DIY FPGA-Based NAND Recovery (Arty Z7 Approach)

As an alternative to professional chip-off services, the NAND flash die can be read directly using a consumer FPGA development board. The principle is the same as the professional ACE Lab PC-3000 Flash: bypass the SD card's controller entirely and read every physical NAND page, including pages the controller reports as `0xFF` due to TRIM.

A complete FPGA design for this approach is provided in the `fpga_nand_recovery/` directory of this repository, targeting the **Digilent Arty Z7** (Xilinx Zynq XC7Z020 SoC).

### 15.1 Required Hardware

| Item | Purpose | Estimated Cost |
|---|---|---|
| **Digilent Arty Z7-20** | FPGA + ARM SoC development board | ~$260 |
| **Hot air rework station** | Decapsulating the microSD card and desoldering the NAND die | ~$50–150 |
| **Pmod breakout board or protoboard** | Wiring NAND die pads to the Arty Z7's Pmod connectors | ~$10–20 |
| **Fine-gauge wire (30–36 AWG)** | Connecting NAND bond pads to breakout board | ~$10 |
| **Magnifying lamp or stereo microscope** | Inspecting and soldering to NAND die pads | ~$30–100 (lamp) |
| **Multimeter** | Continuity testing and pin identification | ~$20 |
| **ESD mat and wrist strap** | Preventing electrostatic damage to exposed NAND die | ~$15 |
| **Total** | | **~$400–575** |

Software (free): Xilinx Vivado ML Edition (free for Zynq-7000), Python 3 with pyserial.

### 15.2 Step-by-Step Process

#### Step 1: Physical Decapsulation of the MicroSD Card

The plastic housing of the HIKSEMI microSD card must be removed to expose the internal PCB or monolithic die. MicroSD cards are constructed in one of two ways:

- **Multi-chip module (MCM):** A small PCB carries a separate NAND flash chip and a separate controller chip. The NAND chip can be desoldered and connected to the FPGA independently. This is the easier scenario.
- **Monolithic package:** The controller and NAND are bonded together on a single substrate (common in modern consumer microSD). The NAND die's bond pads are directly accessible on the substrate but require wire-bonding or probe-tip contact rather than standard soldering.

To decapsulate: carefully heat the card's plastic housing with a hot air gun at low temperature (~100–150 °C) and peel it apart with a razor blade. Do NOT heat to NAND erase temperatures (>200 °C). Alternatively, use acetone or fuming nitric acid to dissolve the plastic (advanced technique, requires proper ventilation).

**Risk:** This step is destructive and irreversible. If the card is monolithic and the bond pads are too fine to wire, this approach may not be viable and the card may be rendered unrecoverable even by professional labs. Consider sending the card to a professional lab first for evaluation before attempting this.

#### Step 2: Identify the NAND Flash Die

Once the internals are exposed, identify the NAND die by its markings. Look for:

- Die markings (manufacturer logo, part number, date code)
- The number and arrangement of bond pads
- Whether it is a separate chip (TSOP-48, BGA, LGA) or bonded directly to the substrate

If the NAND is a standard TSOP-48 package (common in older or larger SD cards), its pinout follows the ONFI/JEDEC standard and can be looked up directly. If it is a bare die or BGA, the pinout must be determined experimentally using the FPGA's READ ID command.

#### Step 3: Wire the NAND to the Arty Z7 Pmod Connectors

The NAND flash interface requires 15 signals, which fit on two Pmod connectors (16 pins):

| Pmod Pin | Signal | Direction | Function |
|---|---|---|---|
| JA[0]–JA[7] | I/O[7:0] | Bidirectional | 8-bit NAND data bus |
| JB[0] | CLE | Output | Command Latch Enable |
| JB[1] | ALE | Output | Address Latch Enable |
| JB[2] | CE# | Output | Chip Enable (active low) |
| JB[3] | WE# | Output | Write Enable (active low) |
| JB[4] | RE# | Output | Read Enable (active low) |
| JB[5] | WP# | Output | Write Protect (active low, directly tie high) |
| JB[6] | R/B# | Input | Ready/Busy (active low, needs pull-up) |

Power the NAND die at 3.3V from the Pmod VCC pins (the Arty Z7 Pmods provide 3.3V). The NAND's VSS pins connect to Pmod GND.

Use short wires (under 10 cm) to minimize signal integrity issues. The FPGA design uses conservative ONFI Mode 0 timing (30 ns pulse widths) specifically to tolerate sloppy wiring.

#### Step 4: Build and Program the FPGA Design

The `fpga_nand_recovery/` directory contains a complete Vivado project:

```
fpga_nand_recovery/
├── hdl/
│   ├── nand_pkg.vhd             — ONFI commands, timing constants, types
│   ├── nand_flash_ctrl.vhd      — NAND bus protocol FSM (two-level state machine)
│   ├── axi_nand_ctrl.vhd        — AXI4-Lite slave with 18 KB page buffer
│   └── nand_dumper_top.vhd      — Top-level with IOBUF instantiation
├── constraints/
│   └── arty_z7_pmod_nand.xdc    — Pin mapping for Pmod JA/JB
├── sim/
│   ├── tb_nand_flash_ctrl.vhd   — Testbench with behavioral NAND model
│   └── run_sim.tcl              — Vivado simulation script
├── sw/
│   ├── nand_dump.c              — Zynq ARM bare-metal dump program
│   └── host_receiver.py         — Host-side Python UART capture script
└── tcl/
    └── create_project.tcl       — Vivado project creation script
```

Build steps:

1. Install Xilinx Vivado ML Edition (free, supports Zynq-7000)
2. Install the [Digilent board files](https://digilent.com/reference/programmable-logic/guides/installing-vivado-and-sdk) for the Arty Z7 preset
3. Run the project creation script: `vivado -mode batch -source tcl/create_project.tcl`
4. Open the project in Vivado, generate the bitstream
5. Export the hardware (with bitstream) to Vitis
6. In Vitis, create a bare-metal application and add `sw/nand_dump.c`
7. Program the Arty Z7 via USB-JTAG

#### Step 5: Identify the NAND Geometry

With the FPGA programmed and the NAND wired up, open a serial terminal to the Arty Z7's USB-UART (921600 baud, 8N1). The ARM firmware provides an interactive command shell:

```
> R                          (Reset the NAND — always do this first)
OK:RESET
> I                          (Read ID — identifies the NAND chip)
ID:9493DE98 00000076
  Maker=0x98 Device=0xDE     (0x98 = Toshiba/Kioxia, 0xDE = 64Gb TLC)
  PageSize=00002000 BlockPages=00000100 Spare/512=00000010
```

The READ ID response reveals the NAND manufacturer, device code, page size, block size, and spare area layout. These parameters are critical — they determine the addressing scheme and how much data to read per page.

Common NAND manufacturers and their maker codes:

| Maker Code | Manufacturer |
|---|---|
| `0x98` | Toshiba / Kioxia |
| `0xAD` | SK Hynix |
| `0x2C` | Micron |
| `0xC8` | YMTC (probable for HIKSEMI) |
| `0x01` | AMD/Spansion |
| `0xEC` | Samsung |
| `0x45` | SanDisk |

If the NAND does not respond to READ ID (no data returned, or all `0xFF`), the wiring is incorrect or the die is damaged. Systematically try different pin assignments — the data bus pins are often in a non-obvious order on bare dies.

#### Step 6: Dump Every Physical Page

Once the NAND responds to READ ID, set the read count to the full page size (data + spare area) and dump all pages:

```
> C                          (Set read count)
Send 2 count bytes: [page_size + spare as uint16 LE]
> D                          (Start full dump — host_receiver.py must be running)
```

On the host computer, run the receiver script:

```bash
python3 sw/host_receiver.py --port /dev/ttyUSB1 --output nand_raw_dump.bin
```

The dump reads every physical NAND page sequentially — block 0 page 0 through the last block — and streams the raw bytes (including spare/OOB area) over UART. At 921600 baud (~90 KB/s effective throughput), a 64 GB NAND takes approximately **8–10 days** for a complete dump. This can be reduced to ~16 hours by increasing the baud rate to the FTDI chip's maximum of ~3 Mbaud (requires modifying the baud rate in `nand_dump.c` and the host script).

#### Step 7: Post-Processing the Raw NAND Dump

The raw dump file is NOT a usable disk image. It requires several processing steps to reconstruct the original files:

1. **Data Unwhitening (XOR Mask Removal):** The SD card controller applies an XOR-based "whitening" pattern to the data before writing it to NAND (this distributes bit values evenly to improve NAND cell reliability). The whitening pattern must be identified and reversed. It is typically a repeating XOR mask that depends on the page address. Trial-and-error with known file headers (e.g., the `ftyp` atom of MP4 files) can reveal the mask.

2. **ECC Error Correction:** Raw NAND pages contain bit errors. The spare area holds BCH or LDPC error-correcting codes. The ECC algorithm and parameters (e.g., BCH-8 over 512-byte sectors) must be determined from the controller's firmware or by analysis. Joshua Wise's [ndfslave](https://github.com/jwise/ndfslave) project includes a BCH decoder.

3. **FTL Reconstruction:** The Flash Translation Layer maps logical block addresses (LBAs) to physical NAND page addresses. The FTL metadata is stored in reserved blocks on the NAND (typically the first and last few blocks). For TRIMmed data, the FTL's "old" mapping entries — which record where the GoPro video pages were physically written — may still exist in previous FTL snapshots or journal entries. Reconstructing this mapping is the most complex step and requires reverse-engineering the Silicon Motion controller's FTL algorithm.

4. **File Carving:** Once the logical page order is reconstructed, standard file carving tools (PhotoRec, Scalpel, or custom scripts) can scan for MP4/MOV file signatures (`ftyp` atoms, `moov` atoms, `mdat` atoms) and reconstruct the GoPro video files.

### 15.3 FPGA Design Architecture

The FPGA design implements the ONFI (Open NAND Flash Interface) asynchronous Mode 0 protocol:

```
┌─ Zynq PS (ARM Cortex-A9) ──────────────────────────┐
│  nand_dump.c                                         │
│  - Command shell over UART (921600 baud)             │
│  - Reads page buffer from FPGA via AXI registers     │
│  - Streams raw data to host PC                       │
└───────────────────┬──────────────────────────────────┘
                    │ AXI4-Lite (GP0 @ 0x40000000)
┌─ Zynq PL (FPGA) ─┼──────────────────────────────────┐
│  ┌────────────────┴────────────────────────────────┐ │
│  │  axi_nand_ctrl (AXI4-Lite slave)                │ │
│  │  Register map:                                   │ │
│  │    0x0000 CTRL     — start op, select op type    │ │
│  │    0x0004 STATUS   — busy/done/ready flags       │ │
│  │    0x0008 ADDR_COL — column address              │ │
│  │    0x000C ADDR_ROW — row address (page+block)    │ │
│  │    0x0010 RD_COUNT — bytes to read               │ │
│  │    0x0014 ID_LO    — NAND ID bytes 0–3           │ │
│  │    0x0018 ID_HI    — NAND ID byte 4              │ │
│  │    0x4000 PAGE_BUF — 18 KB page buffer (BRAM)    │ │
│  └────────────────┬────────────────────────────────┘ │
│  ┌────────────────┴────────────────────────────────┐ │
│  │  nand_flash_ctrl (ONFI protocol engine)         │ │
│  │  Two-level FSM:                                  │ │
│  │    Sequencer: CE# → CMD1 → ADDR → CMD2 →        │ │
│  │               wait R/B# → READ bytes → CE# off   │ │
│  │    Bus engine: data setup → WE#/RE# pulse →      │ │
│  │                hold → done (30 ns cycles)         │ │
│  └────────────────┬────────────────────────────────┘ │
│                   │ IOBUF (bidirectional data bus)    │
│         Pmod JA: I/O[7:0]   Pmod JB: control sigs   │
└───────────────────┼──────────────────────────────────┘
                    │ wires
           ┌────────┴────────┐
           │  NAND flash die  │
           │  (from SD card)  │
           └─────────────────┘
```

The controller uses conservative timing (30 ns WE#/RE# pulse widths, 20 ns setup/hold) that exceeds all ONFI Mode 0 minimums, ensuring reliable communication even with imperfect wiring. At 100 MHz PL clock, each read byte takes approximately 80 ns, yielding ~12 MB/s peak NAND read throughput — far faster than the UART bottleneck.

### 15.4 Comparison: DIY FPGA vs. Professional Lab

| Factor | DIY FPGA (Arty Z7) | Professional Lab (PC-3000 Flash) |
|---|---|---|
| **Hardware cost** | ~$400–575 (reusable for other projects) | $0 (included in service fee) |
| **Service cost** | $0 | $300–$1,500 |
| **Time to dump** | 16 hours – 10 days (depends on baud rate) | ~1–4 hours |
| **Post-processing** | Manual: reverse-engineer FTL, XOR mask, ECC | Automated: Visual NAND Reconstructor has controller database |
| **Success probability** | Low-moderate (depends on your ability to reconstruct the FTL) | Moderate-high (professional tools and experience) |
| **Risk of destroying data** | High (decapsulation is irreversible; wiring errors could electrically damage NAND) | Low (professionals have done this thousands of times) |
| **Learning value** | Extremely high | None |
| **No-data-no-fee option** | N/A (you bear all risk) | Most labs offer this |

### 15.5 Recommended Approach

**If the turtle video has sentimental or practical value:** Send the card to a professional lab first. The $300 flat-rate services with no-data-no-fee policies (see Section 11.4) are the lowest-risk option.

**If you want the learning experience:** The FPGA approach is a genuine embedded systems / digital forensics project. Joshua Wise's [ndfrecovery](https://joshuawise.com/projects/ndfrecovery) project proves it is achievable by a skilled hobbyist — he recovered 100% of photos and videos from a broken SD card using a similar FPGA setup. His project took approximately 6 months, though much of that was reverse-engineering that is now documented. With his published code and the FPGA design in this repository as a starting point, the timeline could be significantly shorter.

**The worst approach:** Attempting the FPGA recovery first, damaging the NAND die, and then finding that professional labs cannot recover it either. If you want to try the FPGA route, practice on a sacrificial SD card first.

---

## 16. References and Sources

### Research and Technical Analysis
- Bunnie Huang — ["On Hacking MicroSD Cards"](https://www.bunniestudios.com/blog/2013/on-hacking-microsd-cards/) (30C3, 2013)
- Bunnie Huang — ["The Exploration and Exploitation of an SD Memory Card"](https://www.bunniefoo.com/bunnie/sdcard-30c3-pub.pdf) (30C3 Presentation PDF)
- Joshua Wise — ["Reverse Engineering a NAND Flash Device Management Algorithm"](https://joshuawise.com/projects/ndfrecovery) (ndfrecovery project)
- Joshua Wise — [ndfslave source code on GitHub](https://github.com/jwise/ndfslave)
- Gillware — ["Flash Memory Data Recovery: Get Data through Direct Read of NAND Memory"](https://www.gillware.com/flash-drive-data-recovery/flash-memory-amnesia-resurrecting-data-through-direct-read-of-nand-memory/)
- Datarecovery.com — ["What Is an SSD's Flash Translation Layer (FTL) and Why Is It Crucial for Recovery?"](https://datarecovery.com/rd/what-is-an-ssds-flash-translation-layer-ftl-and-why-is-it-crucial-for-recovery/)
- 300 Dollar Data Recovery — ["What is TRIM? Can my data still be recovered?"](https://www.300dollardatarecovery.com/what-is-trim/)
- SanDisk — ["Learn About TRIM, UAS Support for USB Flash, Memory Cards, and SSD"](https://support-en.sandisk.com/app/answers/detailweb/a_id/25185/~/learn-about-trim-support-for-usb-flash,-memory-cards,-and-ssd-on-windows-and)
- Linux Kernel Mailing List — ["mmc-utils: Add General command CMD56 read support"](https://lore.kernel.org/lkml/20220417110934.621035-1-huobean@gmail.com/T/)
- Android Open Source Project — ["Adoptable Storage"](https://source.android.com/docs/core/storage/adoptable)

### Professional Recovery Services
- [ACE Lab PC-3000 Flash](https://www.teeltech.com/mobile-device-forensic-tools/pc-3000-suite/pc-3000-flash/)
- [Rossmann Group — MicroSD Data Recovery](https://rossmanngroup.com/microsd-data-recovery)
- [$300 Data Recovery — Chip-Off Recovery](https://www.300dollardatarecovery.com/chip-off-data-recovery/)
- [PITS Data Recovery — Micro SD Chip-Off Recovery](https://www.pitsdatarecovery.com/blog/micro-sd-chip-off-recovery/)
- [PetaPixel — "This is What Advanced Memory Card Data Recovery Looks Like"](https://petapixel.com/2022/07/28/this-is-what-advanced-memory-card-data-recovery-looks-like/)

### Hardware and Controller Information
- [Silicon Motion — Flash Card Controller Products](https://www.siliconmotion.com/products/Flash-Card/detail)
- [Silicon Motion SM2703 UHS-I Controller Announcement](https://ir.siliconmotion.com/news-releases/news-release-details/silicon-motion-introduces-sm2703-cost-effective-sd-30-uhs-i/)
- [Hackaday — "Hacking SD Card & Flash Memory Controllers"](https://hackaday.com/2013/12/29/hacking-sd-card-flash-memory-controllers/)

### Tools Used in This Analysis
- Custom Win32 SD card extraction tool (`main.cpp` in this repository) — Uses DeviceIoControl with 13 distinct IOCTL codes, SetupDi device enumeration, SFFDISK SD command interface, raw disk imaging with unbuffered direct I/O
- Custom Rust full-disk signature scanner (`/tmp/sd_scan/`) — Scanned 58.60 GB at 251 MB/s, searched for 19+ file type signatures
- TestDisk 7.1 (Christophe Grenier) — Partition table analysis
- fdisk (util-linux) — MBR/partition verification
- xxd — Hex dump analysis of MBR, VBR, and data regions
- Python 3 — Entropy analysis and byte distribution statistics
