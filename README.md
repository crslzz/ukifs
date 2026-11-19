# UKI Virtual Filesystem for QNX

## Overview

`ukifs` is a virtual filesystem for QNX that allows presenting the different sections of a Unified Kernel Image (UKI) as individual files. The UKI can be a plain file or one of the two A/B slots of a [ukiboot](https://gitlab.com/CentOS/automotive/src/ukiboot) disk image.

The repository also provides `ukiextract`, a standalone Linux utility that extracts the UKI of a ukiboot slot using the same code.

## Purpose

This program mounts a UKI as a read-only filesystem. This allows passing individual OS artifacts (typically, the Linux kernel, initial ramdisk, and kernel command-line) directly to QNX's qvm virtual machine manager. Files integrity is ensured via the verification of the UKI's signature.

## Building

This program depends on the `ukiboot` project at https://gitlab.com/CentOS/automotive/src/ukiboot.

Build the program using the QNX Software Development Platform (QNX SDP) using:

```bash
UKIBOOT=/path/to/ukiboot QNX_SDP=/path/to/qnx/sdp make
```

This creates `ukifs` (x86_64) and `ukifs.aarch64` executables for QNX.

The companion Linux utility `ukiextract` (see [Extracting a UKI on
Linux](#extracting-a-uki-on-linux-ukiextract)) is built for the host with:

```bash
UKIBOOT=/path/to/ukiboot QNX_SDP=/path/to/qnx/sdp make utils
```

## Usage

```bash
./ukifs [options] <uki-file|ukiboot-image> <mount-point>
```

The first argument is either a plain UKI file, or a ukiboot disk image when a
slot is selected with `--slot` / `-A` / `-B` (see [ukiboot A/B
slots](#ukiboot-ab-slots) below).

### Options
-  `-v`, `--verbose`        - Verbose mode (info messages)
-  `-p <perm>`, `--perm`    - File permission (default: 0444)
-  `-d <level>`, `--debug`  - Debug level: 0=quiet, 1=errors, 2=info, 3=debug (default: 0)
-  `-c <cert.pem>`, `--cert` - X.509 PEM certificate for signature verification
-  `-s <a|b|auto>`, `--slot` - Read the UKI from a slot of a ukiboot disk image
-  `-A`, `--slot-a`         - Same as `--slot a`
-  `-B`, `--slot-b`         - Same as `--slot b`
-  `--bootctl <path>`       - Standalone `ukibootctl` partition to use for `--slot auto`
-  `--no-mmap`              - Never map the UKI, always read it with `pread()`
-  `-h`, `--help`           - Show this help

### Examples:
```bash
./ukifs /boot/ukiboot.efi /mnt/uki
./ukifs -v /boot/ukiboot.efi /mnt/uki
./ukifs -d 3 /boot/ukiboot.efi /mnt/uki
./ukifs -c /etc/uki-cert.pem /boot/ukiboot.efi /mnt/uki
./ukifs --slot auto /dev/hd0 /mnt/uki
./ukifs --slot auto /dev/hd1t131 /mnt/uki   # image stored in a partition
./ukifs -c /etc/uki-cert.pem --slot-b /tmp/ukiboot.img /mnt/uki


./ukifs -v /boot/ukiboot.efi /mnt/uki
[INFO] Opening UKI file: /boot/ukiboot.efi
[INFO] File size: 134217728 bytes
[INFO] Mapped UKI at 0x10100000000 (134217728 bytes)
[INFO] UKI size: 66324224 bytes (134217728 bytes available)
[INFO] Ignoring 67893504 bytes of padding after the UKI (50.6%)
[INFO] No certificate provided, signature verification skipped
[INFO] Parsing UKI sections...
[INFO] Parsed 11 sections from UKI file
[INFO] Attaching resource manager at /mnt/uki

ls -l /mnt/uki
total 126228
-r--r--r--   1 root      root            325 May 13  2026 cmdline
-r--r--r--   1 root      root            152 May 13  2026 data
-r--r--r--   1 root      root       11335439 May 13  2026 initrd
-r--r--r--   1 root      root       53215232 May 13  2026 linux
-r--r--r--   1 root      root            430 May 13  2026 osrel
-r--r--r--   1 root      root            140 May 13  2026 reloc
-r--r--r--   1 root      root          16384 May 13  2026 rodata
-r--r--r--   1 root      root            321 May 13  2026 sbat
-r--r--r--   1 root      root             55 May 13  2026 sdmagic
-r--r--r--   1 root      root          57368 May 13  2026 text
-r--r--r--   1 root      root             25 May 13  2026 uname

cat /mnt/uki/uname
6.12.0-210.el10iv.aarch64
```

## ukiboot A/B slots

A [ukiboot](https://gitlab.com/CentOS/automotive/src/ukiboot) disk image holds
two UKIs in the `ukiboot_a` and `ukiboot_b` partitions, plus a `ukibootctl`
partition tracking which slot is valid, active, and how many boot attempts
remain. `ukifs` can mount the UKI of one of these slots directly from the disk
image or from the raw disk device, without extracting it first.

Both partitioning schemes defined by ukiboot are supported:

- **GPT**: partitions are found by name (`ukiboot_a`, `ukiboot_b`,
  `ukibootctl`), using ukiboot's GPT parser.
- **MBR**: the slots are logical partitions of an extended partition,
  identified by their MBR partition id (`0x46`, `0x47` and `0x48`).

### Selecting a slot

```bash
# Mount slot A, whatever the ukibootctl content says
./ukifs --slot a /dev/hd0 /mnt/uki     # or: ./ukifs -A /dev/hd0 /mnt/uki

# Mount slot B
./ukifs --slot b /dev/hd0 /mnt/uki     # or: ./ukifs -B /dev/hd0 /mnt/uki

# Mount the active slot, as ukiboot would select it
./ukifs --slot auto /dev/hd0 /mnt/uki

# Same, with the ukibootctl partition exposed as a separate device
./ukifs --bootctl /dev/hd0t72 /dev/hd0 /mnt/uki
```

### Automatic slot selection

With `--slot auto`, the active slot is computed from the `ukibootctl` content
with ukiboot's own rules:

- the content is rejected if its magic or its CRC32 does not match, in which
  case the ukiboot defaults are used (both slots valid, slot A with the
  highest priority);
- a slot is valid if `successful_boot` is set, or if `tries_remaining` is
  greater than zero;
- the valid slot with the highest priority is selected.

This is the same slot ukiboot boots, so the mounted kernel and initrd are those
of the running (or next) boot.

```bash
$ ./ukifs -v --slot auto /dev/hd0 /mnt/uki
[INFO] Opening ukiboot image: /dev/hd0
[INFO] Reading GPT partition table
[INFO] ukibootctl slot a: priority=14 tries_remaining=0 successful_boot=1
[INFO] ukibootctl slot b: priority=15 tries_remaining=7 successful_boot=0
[INFO] Active ukiboot slot: b
[INFO] ukiboot slot b: offset 167772160, size 157286400 bytes
[INFO] Using ukiboot slot b at offset 167772160 (157286400 bytes)
[INFO] UKI size: 66324224 bytes (157286400 bytes available)
[INFO] Ignoring 90962176 bytes of padding after the UKI (57.8%)
[INFO] Parsed 11 sections from UKI file
[INFO] Attaching resource manager at /mnt/uki
```

Signature verification (`-c <cert.pem>`) works the same way for a UKI stored in
a slot: the Authenticode digest does not cover the padding of the slot
partition. See [SIGNATURE_VERIFICATION.md](SIGNATURE_VERIFICATION.md).

### Disk images and block devices

The ukiboot image can be a regular file, a whole disk, or a disk partition
reached through a block device. Only the beginning of what is opened matters:
the partition table of the ukiboot image has to be at its offset 0, which is
the case both for a whole ukiboot disk and for a device node that exposes the
partition holding a ukiboot image.

```bash
# The ukiboot image is the whole disk
./ukifs --slot auto /dev/hd0 /mnt/uki

# The ukiboot image is stored inside a partition of a disk
./ukifs --slot auto /dev/hd1t131 /mnt/uki

# The ukiboot image is a file
./ukifs --slot auto /tmp/ukiboot.img /mnt/uki
```

A few things are handled specifically for block devices:

- **Size**: `st_size` from `stat()` is only trusted for regular files. QNX
  block devices report their size there in 512 byte blocks (`st_size` equals
  `st_blocks`), which is 512 times smaller than the truth, so the size of a
  device is obtained by seeking to its end instead. With `--slot`, the size is
  not even needed: the partition table of the image gives the offset and the
  size of the slot, and a size reported by a driver never makes `ukifs` reject
  a slot.
- **Mapping is optional**: whether a device can be mapped depends on its
  driver. On an i.MX8QXP target with `devb-sdmmc`, mapping a partition works
  and reads are served straight from the mapping; other drivers reject
  `mmap()`. `ukifs` tries to map, logs the reason when it cannot, and then
  serves reads with `pread()` into a temporary buffer instead of handing out
  pointers into a mapping. This is transparent for clients; only the zero-copy
  optimization is lost. `--no-mmap` forces that mode, which is also useful to
  compare both paths on the same source.
- **Reads are bounded**: in that mode a single read is answered with at most
  1 MB, so a client issuing a huge read gets a short read (which is allowed)
  rather than making the driver allocate a huge buffer.

```bash
$ ukifs -v --slot auto /dev/sd0t131 /mnt/uki
[INFO] Opening ukiboot image: /dev/sd0t131
[INFO] Source: block device, size: 3709861888 bytes (as reported by the driver)
[INFO] Reading GPT partition table
[INFO] ukibootctl slot a: priority=15 tries_remaining=0 successful_boot=1
[INFO] ukibootctl slot b: priority=14 tries_remaining=7 successful_boot=0
[INFO] Active ukiboot slot: a
[INFO] ukiboot slot a: offset 105906176, size 134217728 bytes
[INFO] Using ukiboot slot a at offset 105906176 (134217728 bytes)
[INFO] Mapped UKI at 31e68cf000 (134217728 bytes)
[INFO] UKI size: 65192448 bytes (134217728 bytes available)
[INFO] Ignoring 69025280 bytes of padding after the UKI (51.4%)
[INFO] Parsed 11 sections from UKI file
[INFO] Attaching resource manager at /mnt/uki
```

## Extracting a UKI on Linux (ukiextract)

`ukiextract` is a standalone Linux utility that extracts the UKI stored in a
ukiboot slot, for instance to inspect it, archive it, or feed it to a tool that
expects a plain UKI file. It shares all of its logic with the QNX driver: the
same `ukifs_slot.c` for the partition table and the `ukibootctl` content, the
same `ukifs_size.c` for the size of the UKI, and the same ukiboot PE parser for
the sections and the signature. It therefore extracts exactly the UKI that
`ukifs` would mount and that ukiboot would boot.

```bash
./ukiextract [options] <ukiboot-image>
```

### Options
-  `-s <a|b|auto>`, `--slot`  - Slot to extract (default: `auto`)
-  `-A`, `--slot-a`           - Same as `--slot a`
-  `-B`, `--slot-b`           - Same as `--slot b`
-  `--bootctl <path>`         - Standalone `ukibootctl` partition to use for `--slot auto`
-  `-o <path>`, `--output`    - Output file, `-` for stdout (default: `ukiboot_a.efi` / `ukiboot_b.efi`)
-  `-c <cert.pem>`, `--cert`  - Verify the UKI signature before extracting
-  `--whole-slot`             - Extract the whole slot partition, padding included
-  `-v`, `--verbose`          - Verbose mode (info messages)
-  `-d <level>`, `--debug`    - Debug level: 0=quiet, 1=errors, 2=info, 3=debug
-  `-h`, `--help`             - Show this help

By default only the UKI itself is written out: its size is computed from the PE
headers (headers, sections and certificate table), so the padding of the slot
partition is left behind and the extracted file is byte-identical to the UKI
that was installed in the slot.

### Examples

```bash
# Active slot of a disk image or disk, as ukiboot would select it
./ukiextract disk.img                       # writes ukiboot_a.efi or ukiboot_b.efi
./ukiextract /dev/sda -o /tmp/active.efi

# A specific slot, verified against a certificate before extraction
./ukiextract --slot-b --cert rootCA.crt -o slot_b.efi disk.img

# Boot control data on its own device
./ukiextract --bootctl /dev/sda4 -o active.efi /dev/sda

# ukiboot image stored in a partition, read through its block device
./ukiextract --slot auto -o active.efi /dev/sda1

# Pipe the UKI somewhere else
./ukiextract -o - disk.img | sha256sum

# The complete slot partition, padding included
./ukiextract --slot a --whole-slot -o slot_a_raw.img disk.img
```

```bash
$ ./ukiextract -v disk.img
[INFO] Reading GPT partition table
[INFO] ukibootctl slot a: priority=15 tries_remaining=7 successful_boot=0
[INFO] ukibootctl slot b: priority=14 tries_remaining=7 successful_boot=0
[INFO] Active ukiboot slot: a
[INFO] ukiboot slot a: offset 105906176, size 134217728 bytes
[INFO] UKI size: 65192448 bytes (69025280 bytes of padding ignored)
[INFO] Extracted 65192448 bytes from slot a of 'disk.img' to 'ukiboot_a.efi'
```

Without `-v`, the utility is silent and prints only the path of the extracted
file (nothing when extracting to stdout), so it can be used in scripts:

```bash
UKI=$(./ukiextract --slot auto disk.img) || exit 1
```

## Testing

Host test programs are built with `make tests`; see [TESTING.md](TESTING.md).
The ukiboot slot support can be exercised on Linux with:

```bash
make test-slot utils
./test_slot_demo.sh      # builds GPT and MBR ukiboot images, resolves the slots,
                         # and checks that ukiextract gives the UKIs back byte for byte
sudo ./test_blkdev.sh    # puts a ukiboot image in a partition of a loop device
                         # and reads it back through the block device
```
