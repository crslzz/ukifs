# UKI Size Analysis Test Program

## Overview

`test_size` is a standalone Linux utility that analyzes UKI (Unified Kernel Image) files to compute their expected size based on PE/COFF structure, identifying any extra padding or data appended after the actual UKI content.

## Purpose

This program helps you:
- **Detect extra padding** - Identify wasted space in UKI files
- **Validate UKI structure** - Ensure sections don't overlap or extend beyond the file
- **Optimize storage** - Determine the minimum file size needed
- **Analyze layout** - Understand how sections are arranged in the PE file

## Building

Build the test program natively for Linux:

```bash
make test-size
```

This creates a `test_size` executable (~25KB, x86-64 Linux binary) with no external dependencies beyond libc.

## Usage

```bash
./test_size <uki-file>
```

### Arguments

- `<uki-file>` - Path to UKI file (PE executable, typically `.efi` or `.img`)

### Examples

```bash
# Analyze a UKI image
./test_size /boot/archlinux.efi

# Analyze test image
./test_size ../ukiboot_a.img

# Run automated demo
./test_size_demo.sh
```

## Sample Output

```
========================================
UKI Size Analysis
========================================
File: ukiboot_a.img
Actual file size: 134217728 bytes (128.00 MB)
========================================

PE Structure:
  DOS header:      0 - 64 bytes
  NT headers at:   64 bytes
  Optional header: 240 bytes
  Sections table:  440 bytes (offset 328)
  Number of sections: 11
  Certificate table: offset 66322432, size 1792 bytes

Section Layout:
Name                Offset       Size          End
----                ------       ----          ---
  text               1536      57856        59392
  rodata            59392      16384        75776
  data              75776        512        76288
  sbat              76288        512        76800
  sdmagic           76800        512        77312
  reloc             77312        512        77824
  osrel             77824        512        78336
  cmdline           78336        512        78848
  uname             78848        512        79360
  linux             79360   53215232     53294592
  initrd         53294592   11335680     64630272

Size Computation:
  Headers end at:        768 bytes
  Last section ends at:  66322432 bytes (initrd)
  Cert table ends at:    66324224 bytes

========================================
Results:
========================================
Expected UKI size: 66324224 bytes (63.25 MB)
Actual file size:  134217728 bytes (128.00 MB)

Extra padding:     67893504 bytes (64.75 MB)
Padding ratio:     50.6% of file size

✓ The file contains 69587456 bytes of extra data after the UKI
========================================
```

## Interpretation

### Expected UKI Size

The **expected UKI size** is computed as the maximum of:
1. End of PE headers (DOS + NT + Optional + Section table)
2. End of last section (highest `PointerToRawData + SizeOfRawData`)
3. End of certificate table (if present, for signed UKIs)

This represents the minimum file size needed to contain all UKI data, including Authenticode signatures.

### Actual File Size

The **actual file size** is the physical size of the file on disk.

### Extra Padding

If `Actual > Expected`, the file contains extra data after the UKI:

```
Extra Padding = Actual File Size - Expected UKI Size
```

This padding can result from:
- **Block alignment** - Storage systems align files to block boundaries (512B, 4KB, etc.)
- **Pre-allocation** - File systems reserving space for future growth
- **Manual padding** - Intentional padding for fixed-size partitions
- **Build artifacts** - Tooling that rounds up to power-of-2 sizes

### Warning Conditions

If `Actual < Expected`, the file is **truncated or corrupted**:
- One or more sections extend beyond the file boundary
- The file was improperly copied or transferred
- Storage corruption occurred

## Common Scenarios

### Scenario 1: No Extra Padding

```
Expected UKI size: 45678901 bytes (43.56 MB)
Actual file size:  45678901 bytes (43.56 MB)

✓ File size matches expected UKI size (no extra padding)
```

**Interpretation:** Optimal - no wasted space.

### Scenario 2: Block Alignment Padding

```
Expected UKI size: 45678901 bytes (43.56 MB)
Actual file size:  45678592 bytes (43.56 MB)

Extra padding:     691 bytes (0.00 MB)
Padding ratio:     0.0% of file size
```

**Interpretation:** Normal - likely aligned to 512-byte blocks (45678901 → 45678592 = 89217 × 512).

### Scenario 3: Significant Padding (Sample Above)

```
Expected UKI size: 64630272 bytes (61.64 MB)
Actual file size:  134217728 bytes (128.00 MB)

Extra padding:     69587456 bytes (66.36 MB)
Padding ratio:     51.8% of file size
```

**Interpretation:** File is padded to a power-of-2 size (128 MB). Common for:
- Fixed-size disk partitions
- Memory-mapped boot images
- Container or archive formats

**Optimization:** Could truncate to 64630272 bytes to save 66.36 MB (51.8% reduction).

### Scenario 4: File Truncated (Error)

```
Expected UKI size: 45678901 bytes (43.56 MB)
Actual file size:  45000000 bytes (42.91 MB)

✗ WARNING: File is smaller than expected!
  Missing: 678901 bytes
  The file may be truncated or corrupted.
```

**Interpretation:** Error condition - file is incomplete.

## Section Layout

The program displays sections sorted by file offset, showing:
- **Name** - Section name with leading '.' stripped (e.g., `.linux` → `linux`)
- **Offset** - Starting position in file (`PointerToRawData`)
- **Size** - Section size in bytes (`SizeOfRawData`)
- **End** - Ending position (`Offset + Size`)

Typical UKI sections:
- **text** - Executable code
- **rodata** - Read-only data
- **data** - Initialized data
- **reloc** - Relocation table
- **sbat** - Secure Boot Advanced Targeting
- **osrel** - OS release information
- **cmdline** - Kernel command line
- **linux** - Linux kernel image
- **initrd** - Initial RAM disk
