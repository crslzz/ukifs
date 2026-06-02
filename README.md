# UKI Virtual Filesystem for QNX

## Overview

`uqifs` is a virtual filesystem for QNX that allows presenting the different sections of a Unified Kernel Image (UKI) as individual files.

## Purpose

This program mounts a UKI as a read-only filesystem. This allows passing OS artifacts (Linux kernel, initial ramdisk, kernel command-line) directly to QNX's qvm virtual machine manager. Files integrity is ensured via the verification of the UKI's signature.

## Building

This program depends on the 'ukiboot' project at https://gitlab.com/CentOS/automotive/src/ukiboot.

Build the program using the QNX Software Development Platform (QNX SDP) using:

```bash
UKIBOOT=/path/to/ukiboot QNX_SDP=/path/to/qnx/sdp make
```

This creates `ukifs` and 'ukifs.aarch64' executables for QNX

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
```
