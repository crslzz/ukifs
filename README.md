# UKI Virtual Filesystem for QNX

## Overview

`ukifs` is a virtual filesystem for QNX that allows presenting the different sections of a Unified Kernel Image (UKI) as individual files.

## Purpose

This program mounts a UKI as a read-only filesystem. This allows passing individual OS artifacts (typically, the Linux kernel, initial ramdisk, and kernel command-line) directly to QNX's qvm virtual machine manager. Files integrity is ensured via the verification of the UKI's signature.

## Building

This program depends on the 'ukiboot' project at https://gitlab.com/CentOS/automotive/src/ukiboot.

Build the program using the QNX Software Development Platform (QNX SDP) using:

```bash
UKIBOOT=/path/to/ukiboot QNX_SDP=/path/to/qnx/sdp make
```

This creates `ukifs` (x86_64) and `ukifs.aarch64` executables for QNX.

## Usage

```bash
./ukifs [options] <uki-file> <mount-point>
```

### Options
-  `-v`             - Verbose mode (info messages)
-  `-p <perm>`      - File permission (default: 0444)
-  `-d <level>`     - Debug level: 0=quiet, 1=errors, 2=info, 3=debug (default: 0)
-  `-c <cert.pem>`  - X.509 PEM certificate for signature verification
-  `-h`             - Show this help

### Examples:
```bash
./ukifs /boot/ukiboot.efi /mnt/uki
./ukifs -v /boot/ukiboot.efi /mnt/uki
./ukifs -d 3 /boot/ukiboot.efi /mnt/uki
./ukifs -c /etc/uki-cert.pem /boot/ukiboot.efi /mnt/uki


# ./ukifs /boot/ukiboot.efi /mnt/uki
[INFO] Opening UKI file: /boot/ukiboot.efi
[INFO] UKI file size: 134217728 bytes
[INFO] No certificate provided, signature verification skipped
[INFO] Parsing UKI sections...
[INFO] Parsed 11 sections from UKI file
[INFO] Attaching resource manager at /mnt/uki

# ls -l /mnt/uki
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

# cat /mnt/uki/uname
6.12.0-210.el10iv.aarch64
```
