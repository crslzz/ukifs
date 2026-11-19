# test_slot - ukiboot Slot Selection Test

## Overview

`test_slot` is a Linux host program that exercises the ukiboot A/B slot support
of `ukifs` without a QNX target. It locates the UKI of a `ukiboot_a` /
`ukiboot_b` slot inside a ukiboot disk image and lists the sections the driver
would expose, using the very same code (`ukifs_slot.c`, `ukifs_size.c` and
ukiboot's `gpt_parser.c` / `pe_parser_posix.c`).

## Building

```bash
UKIBOOT=/path/to/ukiboot QNX_SDP=/path/to/qnx/sdp make test-slot
```

## Usage

```bash
./test_slot [options] <disk-image> [a|b|auto]
```

The slot defaults to `auto`.

### Options

- `--bootctl <path>` - Use this standalone `ukibootctl` partition instead of
  the one found in the image
- `--cert <cert.pem>` - Verify the Authenticode signature of the UKI where it
  sits in the slot
- `-d <level>` - Debug level: 0=quiet, 1=errors, 2=info (default), 3=debug
- `-h` - Show help

## Output

```
$ ./test_slot ukiboot.img auto
[INFO] Reading GPT partition table
[INFO] ukibootctl slot a: priority=14 tries_remaining=0 successful_boot=1
[INFO] ukibootctl slot b: priority=15 tries_remaining=7 successful_boot=0
[INFO] Active ukiboot slot: b
[INFO] ukiboot slot b: offset 2097152, size 1048576 bytes
========================================
ukiboot slot selection
========================================
Image: ukiboot.img
Requested slot: auto

Selected slot:  b
Active slot:    b
Slot offset:    2097152 bytes
Slot size:      1048576 bytes
UKI size:       5144 bytes (1043432 bytes of padding)
Certificate:    offset 3584, size 1560 bytes

Section                Offset         Size   Image offset
--------------------------------------------------------------
.osrel                   1024           30        2098176
.uname                   1536           12        2098688
.cmdline                 2048           25        2099200
.linux                   2560           17        2099712
.initrd                  3072           17        2100224
```

- **Slot offset / size**: the slot partition in the image
- **UKI size**: size computed from the PE headers, the rest of the partition
  being padding
- **Image offset**: where each section actually lives in the image, which is
  what `ukifs` maps

## What Is Tested

**Partition tables:**
- GPT images: `ukiboot_a`, `ukiboot_b` and `ukibootctl` found by name
- MBR images: logical partitions of an extended partition, found by their
  partition ids `0x46`, `0x47` and `0x48`

**Slot selection:**
- Manual selection (`a` / `b`), regardless of the `ukibootctl` content
- Automatic selection (`auto`) with ukiboot's rules: highest priority among
  the slots that either booted successfully or still have boot attempts left
- Fallback to the ukiboot defaults when the `ukibootctl` content has a bad
  magic or a bad CRC32
- `--bootctl` for setups where the boot control data is a separate device

**UKI handling at an offset:**
- PE section table read at the slot offset
- UKI size and certificate table computed at the slot offset
- Authenticode verification of a UKI stored in a padded slot (`--cert`)

## Demo Script

```bash
./test_slot_demo.sh
```

The script requires `sfdisk` and `python3` (and optionally `sbsign` and
`openssl` for the signature case). It builds, in a temporary directory:

- two minimal UKIs (PE/COFF images with `.osrel`, `.uname`, `.cmdline`,
  `.linux` and `.initrd` sections),
- a GPT ukiboot image and an MBR ukiboot image, each with the three ukiboot
  partitions,
- several `ukibootctl` contents.

It then runs `test_slot` for each case:

| Case                                        | Expected slot |
|---------------------------------------------|---------------|
| `a` / `b` (manual)                          | a / b         |
| default boot control (a: prio 15, b: 14)    | a             |
| after a switch to b (b: prio 15, 7 tries)   | b             |
| b out of tries and never successful         | a             |
| zeroed (invalid) `ukibootctl`               | a (defaults)  |
| `--bootctl` file selecting b                | b             |
| signed UKI in slot b, with `--cert`         | b, signature OK |

When `ukiextract` has been built (`make utils`), the script then extracts every
slot of both images and checks that the extracted file is byte-identical to the
UKI that was written to the slot, including the signed one, plus the
`--slot auto`, stdout and `--whole-slot` modes.

## Block Devices

`test_blkdev.sh` (needs root for `losetup`) runs the same checks against a
ukiboot image stored in a partition and reached through a block device, which
is how `ukifs` sees it on a target. See [TESTING.md](TESTING.md).

## Cross-checking With ukiboot

The result can be compared with ukiboot's own tool on the same image:

```bash
ukibootimg --image ukiboot.img status
ukibootimg --image ukiboot.img list
```

Both should report the same active slot, and the kernel offset printed by
`ukibootimg status` should match the `.linux` image offset printed by
`test_slot`.
