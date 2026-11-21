# UKI Testing Suite

This directory contains test programs for UKI (Unified Kernel Image) analysis and verification.

## Test Programs

### 1. test_size - UKI Size Analysis

Computes the actual UKI size from PE/COFF headers, identifying extra padding.

```bash
./test_size <uki-file>
```

**Features:**
- Analyzes PE structure (DOS header, NT headers, sections)
- Detects certificate table (Authenticode signatures)
- Computes minimum file size (headers + sections + certificate)
- Identifies extra padding beyond UKI data
- Works with both signed and unsigned UKIs

**Example:**
```bash
$ ./test_size ukiboot_signed.img
Expected UKI size: 66324224 bytes (63.25 MB)
Actual file size:  134217728 bytes (128.00 MB)
Extra padding:     67893504 bytes (64.75 MB)
```

### 2. test_verify - Signature Verification

Verifies UKI Authenticode signatures using X.509 certificates.

```bash
./test_verify [options] <uki-file> <cert.pem>
```

**Options:**
- `-i` - Ignore padding (verify only actual UKI data)
- `-h` - Show help

**Features:**
- PKCS#7/Authenticode signature verification
- X.509 certificate chain validation
- Optional padding removal for padded UKI files
- Detailed verbose output (digest algorithm, hash verification)

**Example (Standard Mode):**
```bash
$ ./test_verify ukiboot_signed.img rootCA.crt
✓ SIGNATURE VERIFICATION PASSED
```

**Example (Ignore Padding):**
```bash
$ ./test_verify -i ukiboot_signed.img rootCA.crt
Computed UKI size: 66324224 bytes (63.25 MB)
Extra padding: 67893504 bytes (64.75 MB, 50.6% of file)
✓ SIGNATURE VERIFICATION PASSED
```

### 3. test_slot - ukiboot Slot Selection

Locates the UKI of a `ukiboot_a` / `ukiboot_b` slot in a ukiboot disk image and
lists the sections `ukifs` would expose, using the same code as the driver.

```bash
./test_slot [options] <disk-image> [a|b|auto]
```

**Options:**
- `--bootctl <path>` - Standalone `ukibootctl` partition to use
- `--cert <cert.pem>` - Verify the signature of the UKI in the slot
- `-d <level>` - Debug level: 0=quiet, 1=errors, 2=info, 3=debug
- `-h` - Show help

**Features:**
- GPT images (partitions found by name) and MBR images (logical partitions
  with ids 0x46, 0x47, 0x48)
- Automatic slot selection from the `ukibootctl` content, with ukiboot's
  priority / tries_remaining / successful_boot rules
- Reports slot offset and size, UKI size and padding, and the PE sections
  with their offset in the image
- Optional Authenticode verification of the UKI where it sits in the slot

**Example:**
```bash
$ ./test_slot ukiboot.img auto
Selected slot:  b
Active slot:    b
Slot offset:    2097152 bytes
Slot size:      1048576 bytes
UKI size:       5144 bytes (1043432 bytes of padding)

Section                Offset         Size   Image offset
--------------------------------------------------------------
.linux                   2560           17        2099712
.initrd                  3072           17        2100224
```

### 4. test_zboot - EFI zboot Kernel Unwrapping

Reports whether the kernel section of a UKI is an EFI zboot image and, when it
is, unwraps the kernel from it with the same code `ukifs` uses to serve
`vmlinux`.

```bash
./test_zboot [options] <uki-file|ukiboot-image>
```

**Options:**
- `-s <a|b|auto>`, `-A`, `-B` - Read the UKI from a ukiboot slot
- `--bootctl <path>` - Standalone `ukibootctl` partition to use
- `-o <path>` - Write the unwrapped kernel there (`-` for stdout)
- `-d <level>` - Debug level: 0=quiet, 1=errors, 2=info, 3=debug
- `-h` - Show help

**Exit status:** 0 when a kernel was unwrapped, 1 on error, 2 when there is
nothing to unwrap (a bare `Image`, or a container this build cannot handle).

**Example:**
```bash
$ ./test_zboot --slot auto autosd10-ebbr-minimal-image-efi.img
UKI:            autosd10-ebbr-minimal-image-efi.img
UKI offset:     105906176 bytes
Kernel section: linux, offset 105985536, 13914624 bytes

EFI zboot:      yes
Compression:    gzip
Payload:        offset 106034464, 13852062 bytes
vmlinux:        55246848 bytes
Unwrapped:      an arm64 Image

$ ./test_zboot --slot auto autosd10-ebbr-signed.img
EFI zboot:      no, nothing to unwrap
```

## Building

Build all test programs:

```bash
UKIBOOT=/path/to/ukiboot QNX_SDP=/path/to/qnx/sdp make tests
```

Build individually:

```bash
UKIBOOT=/path/to/ukiboot QNX_SDP=/path/to/qnx/sdp make test-size    # UKI size analysis
UKIBOOT=/path/to/ukiboot QNX_SDP=/path/to/qnx/sdp make test-verify  # Signature verification
UKIBOOT=/path/to/ukiboot QNX_SDP=/path/to/qnx/sdp make test-slot    # ukiboot slot selection
UKIBOOT=/path/to/ukiboot QNX_SDP=/path/to/qnx/sdp make test-zboot   # EFI zboot kernel
```

## Running Tests

### Quick Test
```bash
# Size analysis
./test_size ukiboot_signed.img

# Signature verification (both modes)
./test_verify ukiboot_signed.img rootCA.crt
./test_verify -i ukiboot_signed.img rootCA.crt

# ukiboot slot selection
./test_slot ukiboot.img auto
./test_slot ukiboot.img a

# EFI zboot kernel
./test_zboot --slot auto ukiboot.img
./test_zboot -o vmlinux uki.efi
```

### Demo Scripts
```bash
./test_size_demo.sh         # Demonstrates size analysis
./test_verify_demo.sh       # Demonstrates signature verification
./test_slot_demo.sh         # Builds GPT and MBR ukiboot images, resolves their slots,
                            # and round-trips the UKIs through ukiextract
./test_zboot_demo.sh        # Builds UKIs around an EFI zboot kernel and checks that
                            # what is unwrapped is the kernel that was wrapped
./test_zboot_demo.sh <dir>  # Same, keeping the UKI and the checksum of its vmlinux
                            # for test_target.sh to use on a QNX target
sudo ./test_blkdev.sh       # Same, with the ukiboot image in a partition of a
                            # loop device, read through the block device
./test_comprehensive.sh     # Runs all tests
```

### Comprehensive Test Suite
```bash
./test_comprehensive.sh
```

**Tests performed:**
1. Size analysis on unsigned UKI
2. Size analysis on signed UKI
3. Signature verification (standard mode)
4. Signature verification (ignore padding mode)
5. Size computation accuracy validation

**Expected output:**
```
All Tests Passed!

Summary:
  - test_size correctly computes UKI size including certificate table
  - test_verify standard mode works with padded files
  - test_verify -i mode correctly handles padding
  - Both modes successfully verify signed UKI with rootCA.crt
```

## Use Cases

### 1. Detecting Padding in UKI Files

```bash
./test_size production.efi
```

Identifies if UKI has been padded to a fixed size (common for partition alignment or pre-allocation).

### 2. Verifying Signed UKIs

```bash
./test_verify signed.efi signing-cert.pem
```

Validates Authenticode signature and certificate chain.

### 3. Verifying Padded Signed UKIs

When a UKI is signed, then padded to a fixed size:

```bash
# Standard mode: verifies full file (may work if padding is after signature)
./test_verify padded.efi cert.pem

# Ignore padding mode: verifies only UKI data (recommended)
./test_verify -i padded.efi cert.pem
```

The `-i` option is crucial when:
- UKI was signed before padding was added
- File was padded to 128 MB, 256 MB, or other power-of-2 sizes
- Signature verification fails in standard mode due to padding

### 4. Checking Which Slot Would Be Mounted

```bash
# Active slot of a ukiboot disk image, as ukiboot would select it
./test_slot /dev/sda auto

# Force a slot, for instance to inspect the inactive one
./test_slot /dev/sda b
```

`test_slot_demo.sh` builds GPT and MBR ukiboot images from scratch (sfdisk +
python3) and runs all the selection cases, including an exhausted slot, a
corrupted `ukibootctl` content, a standalone `ukibootctl` partition and a
signed UKI stored in a slot. If `ukiextract` has been built (`make utils`), it
also extracts every slot back and checks that the result is byte-identical to
the UKI that was written to it.

### 5. Extracting a UKI on Linux

```bash
make utils

# Active slot, as ukiboot would select it
./ukiextract disk.img

# A specific slot, verified before extraction
./ukiextract --slot-b --cert rootCA.crt -o slot_b.efi disk.img
```

`ukiextract` shares `ukifs_slot.c`, `ukifs_size.c` and ukiboot's PE parser with
the driver, so it is also a convenient way to check on Linux what `ukifs` will
mount on the target. See the README for the full option list.

### 6. Block Device Access

```bash
make test-slot utils
sudo ./test_blkdev.sh
```

`test_blkdev.sh` builds a disk whose first partition contains a complete
ukiboot image, binds it to a loop device with `losetup --partscan`, and checks
that the tools work on `/dev/loopNp1`, that is on a ukiboot image that is not
a regular file:

- the size of the media is found by seeking, since block devices report no
  size in their stat data,
- the partition table of the image, its slots and its `ukibootctl` content are
  read with `pread()` at the right offsets,
- the extracted UKIs are byte-identical to the ones written to the slots,
- the outer disk, which has no ukiboot partitions, is rejected with a clear
  error.

Root is needed for `losetup` only.

### 7. On a QNX Target

The resource manager itself can only be exercised on QNX. `test_target.sh`
runs on the target and does that:

```bash
# on the host
scp ukifs.aarch64 test_target.sh root@target:/data/
# on the target (a binary cannot be executed from a DOS filesystem)
cp /data/ukifs.aarch64 /tmp/ukifs && chmod +x /tmp/ukifs
DEV=/dev/sd0t131 sh /data/test_target.sh
```

It mounts the active slot of the image, lists and reads every section,
does partial and seeking reads, checks that the filesystem is read-only,
and mounts the same slot again with `--no-mmap` to confirm that the data
served from the mapping and the data read with `pread()` are identical.

It then covers the shutdown paths: `umount` is refused while a file is
open and accepted once it is closed, `umount -f` is accepted either way,
`slay` does the same through SIGTERM, and in each case the mountpoint is
gone, the process has exited, and the shutdown watchdog did not fire.

It also covers `-b`: that the command returns only once the filesystem is
serving, that the process stays in the background, that a redirected log
keeps its contents, and that a mount which cannot succeed comes back as a
non-zero status rather than a timeout.

It copies the UKI out of the slot with `dd` and mounts it as a
plain file, with no slot option, and checks that the file and the slot
serve byte-identical sections; that is repeated with `--no-mmap` and
`--eager-map`. Passing `SIGNED_UKI` and `CERT` adds a signed UKI file
mounted with its certificate, which the AutoSD image cannot cover as its
UKI is unsigned:

```bash
DEV=/dev/sd0t131 SIGNED_UKI=/data/signed.efi CERT=/data/cert.pem \
    sh /data/test_target.sh
```

### 8. Integration Testing

```bash
# Verify UKI size matches expected value
COMPUTED=$(./test_size uki.efi | grep "Expected UKI" | awk '{print $4}')
if [ "$COMPUTED" != "66324224" ]; then
    echo "ERROR: Size mismatch"
    exit 1
fi

# Verify signature
if ! ./test_verify -i uki.efi cert.pem; then
    echo "ERROR: Signature invalid"
    exit 1
fi

# Extract the UKI of the active slot, aborting if its signature is invalid
./ukiextract --slot auto --cert cert.pem -o active.efi disk.img || exit 1
```

## Technical Details

### Certificate Table Location

In PE files, the certificate table (Authenticode signature) is:
- Stored in the Security Data Directory (Optional Header, index 4)
- Located at a file offset (not RVA like other data directories)
- Appended after all sections
- Offset: PE32+ = 144 bytes, PE32 = 128 bytes from optional header start

### Size Computation Algorithm

```
expected_size = max(
    headers_end,           // End of section table
    last_section_end,      // Highest section offset + size
    cert_table_end         // Certificate offset + size (if present)
)

extra_padding = actual_file_size - expected_size
```

### Signature Verification with Padding

The `-i` option of `test_verify` works by:
1. Computing actual UKI size using `uki_compute_size()`
2. Creating temporary file with only UKI data (no padding)
3. Verifying signature against temporary file
4. Cleaning up temporary file

The driver itself does not need this: the Authenticode digest computed by
ukiboot's PE parser stops at the certificate table, so padding (including the
unused part of a ukiboot slot partition) is never hashed. See
[SIGNATURE_VERIFICATION.md](SIGNATURE_VERIFICATION.md).

### ukiboot Slot Layout

| Partition    | GPT name     | MBR id | Content                        |
|--------------|--------------|--------|--------------------------------|
| Slot A       | `ukiboot_a`  | `0x46` | Raw UKI (PE/COFF image)        |
| Slot B       | `ukiboot_b`  | `0x47` | Raw UKI (PE/COFF image)        |
| Boot control | `ukibootctl` | `0x48` | `UkiBootCtrl` (24 bytes)       |

`UkiBootCtrl` holds a magic (`1420550408`), two slot entries
(`priority`, `tries_remaining`, `successful_boot`, reserved), 8 reserved bytes
and a CRC32. A slot is valid when `successful_boot` is set or
`tries_remaining` is greater than zero; the valid slot with the highest
priority is the active one. An invalid content falls back to the ukiboot
defaults (both slots valid, slot A with the highest priority).

This is necessary because Authenticode signatures are computed over:
- File content excluding checksum field
- File content excluding certificate table pointers
- File content up to the certificate table

If padding is added after signing, it's not included in the signature hash.

## Files

- `test_size.c` - Size analysis program
- `test_verify.c` - Signature verification program
- `test_slot.c` - ukiboot slot selection program
- `test_zboot.c` - EFI zboot kernel unwrapping program
- `test_images.sh` - Helpers building test UKIs, kernels and ukiboot images
- `test_zboot_demo.sh` - EFI zboot unwrapping demo and checks
- `uki_size.c` / `uki_size.h` - Common size computation library
- `config_linux.h` - Linux build configuration
- `test_size_demo.sh` - Size analysis demo
- `test_verify_demo.sh` - Signature verification demo
- `test_comprehensive.sh` - Complete test suite
- `TEST_SIZE_README.md` - Detailed size analysis documentation
- `TEST_VERIFY_README.md` - Detailed verification documentation
- `TESTING.md` - This file

## Dependencies

- GCC or Clang
- OpenSSL development libraries (`libssl-dev` or `openssl-devel`)
- zlib development libraries (`zlib-devel` or `zlib1g-dev`), for test_zboot
- Linux x86_64 system

Install on Fedora/RHEL:
```bash
sudo dnf install openssl-devel zlib-devel gcc
```

Install on Debian/Ubuntu:
```bash
sudo apt install libssl-dev zlib1g-dev gcc
```

## See Also

- **ukifs** - QNX UKI filesystem driver (uses same PE parsing logic)
- **PE/COFF Specification** - Microsoft Portable Executable format
- **Authenticode Specification** - Microsoft code signing format
