#!/bin/bash
#
# test_blkdev.sh - Check that a ukiboot image can be read from a block device
#
# Covers the case where the ukiboot image is not a regular file but lives
# in a disk partition reached through a block device, which is how ukifs
# sees it on a target (/dev/hd0t179, /dev/emmc0t12, ...).
#
# The test builds an outer disk whose first partition contains a complete
# ukiboot image (its own GPT, ukiboot_a, ukiboot_b and ukibootctl), binds
# it to a loop device with partition scanning, and then runs the host
# tools against the resulting block devices:
#
#   /dev/loopN     the outer disk: no ukiboot partitions, must fail cleanly
#   /dev/loopNp1   the ukiboot image itself, accessed as a block device
#
# Requires root for losetup:
#   sudo ./test_blkdev.sh
#
# Note: the QNX side of ukifs (the resource manager serving reads) cannot
# run here. What this checks is the code ukifs shares with the host tools:
# media size detection, partition table parsing, slot selection and
# pread() based access on a device that is not a regular file.
#

set -e

cd "$(dirname "$0")"

. ./test_images.sh

if [ "$(id -u)" != "0" ]; then
    echo "ERROR: this test needs root to create a loop device"
    echo "Run: sudo $0"
    exit 1
fi

for prog in ./test_slot ./ukiextract; do
    if [ ! -x $prog ]; then
	echo "ERROR: $prog not found, build it with: make test-slot utils"
	exit 1
    fi
done

require_tools sfdisk python3 losetup

TMPDIR=$(mktemp -d /tmp/ukifs_blkdev_XXXXXX)
LOOPDEV=

cleanup() {
    if [ -n "$LOOPDEV" ]; then
	losetup -d "$LOOPDEV" 2>/dev/null || true
    fi
    rm -rf "$TMPDIR"
}
trap cleanup EXIT

UKI_A=$TMPDIR/uki_a.efi
UKI_B=$TMPDIR/uki_b.efi
UKIBOOT_IMG=$TMPDIR/ukiboot.img
OUTER_IMG=$TMPDIR/outer.img
CTL=$TMPDIR/bootctl.bin

# Sector at which the ukiboot image starts in the outer disk
OUTER_PART_LBA=2048

failures=0

ok()   { echo "OK    $*"; }
fail() { echo "FAIL  $*"; failures=$((failures + 1)); }

echo "=== Building the ukiboot image ==="
make_uki "$UKI_A" slota
make_uki "$UKI_B" slotb
make_gpt_image "$UKIBOOT_IMG" "$UKI_A" "$UKI_B"

# Active slot b, so that automatic selection has something to find
make_bootctl "$CTL" 14 0 1 15 7 0
write_at "$UKIBOOT_IMG" "$CTL" $GPT_CTL_LBA

echo "=== Building the outer disk holding it in a partition ==="
truncate -s 32M "$OUTER_IMG"
sfdisk --quiet "$OUTER_IMG" >/dev/null <<EOF
label: gpt
unit: sectors

start=$OUTER_PART_LBA, size=16384, type=0FC63DAF-8483-4772-8E79-3D69D8477DE4, name="ukiboot_image"
EOF
dd if="$UKIBOOT_IMG" of="$OUTER_IMG" bs=512 seek=$OUTER_PART_LBA conv=notrunc status=none

LOOPDEV=$(losetup --find --show --partscan "$OUTER_IMG")
PARTDEV=${LOOPDEV}p1

# Give udev a moment to create the partition node
for _ in 1 2 3 4 5; do
    [ -b "$PARTDEV" ] && break
    sleep 0.2
done

if [ ! -b "$PARTDEV" ]; then
    echo "ERROR: $PARTDEV was not created"
    exit 1
fi

echo "Loop device: $LOOPDEV, ukiboot image partition: $PARTDEV"
ls -l "$LOOPDEV" "$PARTDEV"

echo
echo "=== Media size of a block device ==="
reported=$(./test_slot "$PARTDEV" b 2>/dev/null | sed -n 's/^Media size: *\([0-9]*\).*/\1/p')
expected=$((16384 * 512))
if [ "$reported" = "$expected" ]; then
    ok "size of $PARTDEV reported as $reported bytes (stat reports $(stat -c %s "$PARTDEV"))"
else
    fail "size of $PARTDEV reported as '$reported', expected $expected"
fi

echo
echo "=== Slot selection on the block device ==="
if ./test_slot "$PARTDEV" auto; then
    ok "slot resolved on $PARTDEV"
else
    fail "slot resolution on $PARTDEV"
fi

echo
echo "=== Extraction from the block device ==="
for slot in a b; do
    case $slot in
	a) expected_uki=$UKI_A ;;
	b) expected_uki=$UKI_B ;;
    esac

    if ! ./ukiextract --slot $slot -o "$TMPDIR/out_$slot.efi" "$PARTDEV" >/dev/null; then
	fail "extraction of slot $slot from $PARTDEV"
	continue
    fi

    if cmp -s "$TMPDIR/out_$slot.efi" "$expected_uki"; then
	ok "slot $slot of $PARTDEV == $(basename "$expected_uki")"
    else
	fail "slot $slot of $PARTDEV differs from $(basename "$expected_uki")"
    fi
done

# Automatic selection must pick slot b, as written in the ukibootctl
./ukiextract -o "$TMPDIR/out_auto.efi" "$PARTDEV" >/dev/null || true
if cmp -s "$TMPDIR/out_auto.efi" "$UKI_B"; then
    ok "active slot of $PARTDEV is b"
else
    fail "active slot of $PARTDEV is not b"
fi

# The whole slot partition, which requires the device size handling
./ukiextract --slot a --whole-slot -o "$TMPDIR/out_whole.img" "$PARTDEV" >/dev/null || true
whole_size=$(stat -c %s "$TMPDIR/out_whole.img" 2>/dev/null || echo 0)
if [ "$whole_size" = "1048576" ]; then
    ok "--whole-slot read $whole_size bytes from $PARTDEV"
else
    fail "--whole-slot read $whole_size bytes from $PARTDEV, expected 1048576"
fi

echo
echo "=== The outer disk is not a ukiboot image ==="
if ./ukiextract -o "$TMPDIR/should_not_exist.efi" "$LOOPDEV" 2>"$TMPDIR/err.txt"; then
    fail "$LOOPDEV was accepted as a ukiboot image"
else
    ok "$LOOPDEV rejected: $(tail -1 "$TMPDIR/err.txt")"
fi

echo
if [ "$failures" -ne 0 ]; then
    echo "=== $failures block device test(s) FAILED ==="
    exit 1
fi

echo "=== All block device tests passed ==="
echo
echo "On QNX, the same image in a partition is used with, for instance:"
echo "  ukifs -v --slot auto /dev/hd1t131 /mnt/uki"
