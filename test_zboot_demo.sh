#!/bin/bash
#
# test_zboot_demo.sh - Check the EFI zboot kernel support of ukifs
#
# A UKI whose kernel section was built with CONFIG_EFI_ZBOOT does not
# hold a kernel a non-EFI loader can boot: it holds a small EFI
# application carrying a compressed Image. ukifs unwraps it and serves
# the result as vmlinux, next to the linux section it comes from.
#
# This builds such a UKI around a known kernel, then checks with
# test_zboot that what comes back out is exactly the kernel that went
# in. The same checks run on the UKI stored in a ukiboot slot, where
# everything is offset by the position of the slot in the image, and on
# kernels that must be left alone or refused.
#
# The QNX side (serving vmlinux through the filesystem) cannot run
# here; test_target.sh covers it on a target, with the UKI this script
# writes when it is given an output directory.
#
# Usage: ./test_zboot_demo.sh [output-directory]
#
# Requires: python3, sfdisk and ./test_zboot (make test-zboot)
#

set -e

cd "$(dirname "$0")"

. ./test_images.sh

if [ ! -x ./test_zboot ]; then
    echo "ERROR: ./test_zboot not found, build it with: make test-zboot"
    exit 1
fi

require_tools sfdisk python3 cmp gzip strings

KEEP=${1:-}

TMPDIR=$(mktemp -d /tmp/ukifs_zboot_XXXXXX)
trap 'rm -rf "$TMPDIR"' EXIT

KERNEL=$TMPDIR/Image
PADDED=$TMPDIR/Image.padded
ZBOOT=$TMPDIR/vmlinuz.efi
ZBOOT_UKI=$TMPDIR/zboot_uki.efi
BARE_UKI=$TMPDIR/bare_uki.efi
PLAIN_UKI=$TMPDIR/plain_uki.efi
GPT_IMG=$TMPDIR/gpt.img
OUT=$TMPDIR/vmlinux

failures=0

ok()   { echo "OK    $*"; }
bad()  { echo "FAIL  $*"; failures=$((failures + 1)); }

# run_zboot <expected-status> <args>...
# Runs test_zboot and keeps its output in $TMPDIR/out for inspection
run_zboot() {
    want=$1
    shift
    set +e
    ./test_zboot -d 1 "$@" > $TMPDIR/out 2>&1
    got=$?
    set -e
    [ "$got" = "$want" ]
}

echo "=== Building a kernel and its EFI zboot image ==="
make_arm64_image "$KERNEL" 196608
padded_image "$PADDED" "$KERNEL"
make_zboot "$ZBOOT" "$KERNEL"
ls -l "$KERNEL" "$PADDED" "$ZBOOT"

echo
echo "=== Building the UKIs ==="
make_uki "$ZBOOT_UKI" zboot "$ZBOOT"    # kernel section: an EFI zboot image
make_uki "$BARE_UKI"  bare  "$KERNEL"   # kernel section: a bare arm64 Image
make_uki "$PLAIN_UKI" plain             # kernel section: neither of those
ls -l "$ZBOOT_UKI" "$BARE_UKI" "$PLAIN_UKI"

echo
echo "=== 1. A zboot kernel is unwrapped ==="
if run_zboot 0 -o "$OUT" "$ZBOOT_UKI"; then
    ok "test_zboot unwrapped the kernel"
    sed -n '4,$p' $TMPDIR/out
else
    bad "test_zboot did not unwrap the kernel"
    cat $TMPDIR/out
fi

if cmp -s "$OUT" "$PADDED"; then
    ok "vmlinux is exactly the kernel that was wrapped"
else
    bad "vmlinux differs from the kernel that was wrapped"
    ls -l "$OUT" "$PADDED"
fi

if grep -q "an arm64 Image" $TMPDIR/out; then
    ok "the unwrapped kernel is an arm64 Image"
else
    bad "the unwrapped kernel is not an arm64 Image"
fi

# The wrapper is what the section still serves: linux is left alone
if cmp -s <(dd if="$OUT" bs=8 count=1 2>/dev/null) \
	  <(dd if="$ZBOOT" bs=8 count=1 2>/dev/null); then
    bad "the kernel section and vmlinux start with the same bytes"
else
    ok "vmlinux is not a second name for the kernel section"
fi

echo
echo "=== 2. A bare Image is left alone ==="
if run_zboot 2 "$BARE_UKI"; then
    ok "a bare arm64 Image is not unwrapped"
    grep -e "nothing to unwrap" $TMPDIR/out
else
    bad "a bare arm64 Image was not recognized"
    cat $TMPDIR/out
fi

if run_zboot 2 "$PLAIN_UKI"; then
    ok "a kernel section that is not a PE image is not unwrapped"
else
    bad "a kernel section that is not a PE image was mishandled"
    cat $TMPDIR/out
fi

echo
echo "=== 3. The same UKI in a ukiboot slot ==="
#
# Everything moves by the offset of the slot in the disk image, which
# is where an offset mistake would show up.
#
make_gpt_image "$GPT_IMG" "$ZBOOT_UKI" "$BARE_UKI"

if run_zboot 0 --slot a -o "$OUT.slot" "$GPT_IMG"; then
    ok "the zboot kernel of slot a is unwrapped"
else
    bad "the zboot kernel of slot a was not unwrapped"
    cat $TMPDIR/out
fi

if cmp -s "$OUT.slot" "$PADDED"; then
    ok "slot a serves the same vmlinux as the plain UKI"
else
    bad "slot a serves a different vmlinux"
fi

if run_zboot 2 --slot b "$GPT_IMG"; then
    ok "the bare kernel of slot b is left alone"
else
    bad "the bare kernel of slot b was mishandled"
    cat $TMPDIR/out
fi

echo
echo "=== 4. Damaged zboot images are refused, not served ==="
#
# A truncated or corrupted payload must fail while decompressing, with
# a message, rather than hand out a kernel that is not one.
#
CORRUPT=$TMPDIR/corrupt.efi
cp "$ZBOOT" "$CORRUPT"
# Overwrite the middle of the compressed payload
dd if=/dev/zero of="$CORRUPT" bs=1 seek=40960 count=4096 conv=notrunc \
   status=none
make_uki "$TMPDIR/corrupt_uki.efi" corrupt "$CORRUPT"

if run_zboot 1 -o /dev/null "$TMPDIR/corrupt_uki.efi"; then
    ok "a corrupted payload is refused"
    grep -e "Failed to decompress" -e "unwrapped kernel is" $TMPDIR/out
else
    bad "a corrupted payload was not refused"
    cat $TMPDIR/out
fi

# A payload that claims to be larger than the section holding it
LIAR=$TMPDIR/liar.efi
cp "$ZBOOT" "$LIAR"
python3 -I -c 'import struct,sys
d = bytearray(open(sys.argv[1], "rb").read())
struct.pack_into("<I", d, 12, 0xfffffff0)   # payload_size
open(sys.argv[1], "wb").write(bytes(d))' "$LIAR"
make_uki "$TMPDIR/liar_uki.efi" liar "$LIAR"

if run_zboot 2 "$TMPDIR/liar_uki.efi"; then
    ok "a payload that does not fit in its section is ignored"
    grep -e "does not fit" $TMPDIR/out
else
    bad "a payload that does not fit in its section was accepted"
    cat $TMPDIR/out
fi

# A compression type nothing here can decompress
ZSTD=$TMPDIR/zstd.efi
cp "$ZBOOT" "$ZSTD"
python3 -I -c 'import sys
d = bytearray(open(sys.argv[1], "rb").read())
d[24:32] = b"zstd\x00\x00\x00\x00"
open(sys.argv[1], "wb").write(bytes(d))' "$ZSTD"
make_uki "$TMPDIR/zstd_uki.efi" zstd "$ZSTD"

if run_zboot 2 "$TMPDIR/zstd_uki.efi"; then
    ok "an unsupported compression type is reported, not guessed at"
    grep -e "cannot decompress" $TMPDIR/out
else
    bad "an unsupported compression type was not reported"
    cat $TMPDIR/out
fi

echo
echo "=== 5. A real kernel, if one is at hand ==="
#
# The synthetic Image above is small and incompressible; a real one is
# tens of megabytes and compresses by half. Extracting it from a UKI
# that has one makes the test run against real data.
#
REAL_IMG=autosd10-ebbr-signed.img

if [ -r "$REAL_IMG" ] && [ -x ./ukiextract ]; then
    REAL_UKI=$TMPDIR/real.efi
    REAL_KERNEL=$TMPDIR/real_Image

    ./ukiextract --slot auto -o "$REAL_UKI" "$REAL_IMG" > /dev/null 2>&1
    uki_section "$REAL_UKI" .linux "$REAL_KERNEL"

    make_zboot "$TMPDIR/real_vmlinuz.efi" "$REAL_KERNEL"
    padded_image "$TMPDIR/real_padded" "$REAL_KERNEL"
    make_uki "$TMPDIR/real_zboot_uki.efi" real "$TMPDIR/real_vmlinuz.efi"

    if run_zboot 0 -o "$OUT.real" "$TMPDIR/real_zboot_uki.efi"; then
	ok "a real kernel is unwrapped"
	sed -n '4,$p' $TMPDIR/out
    else
	bad "a real kernel was not unwrapped"
	cat $TMPDIR/out
    fi

    if cmp -s "$OUT.real" "$TMPDIR/real_padded"; then
	ok "the unwrapped real kernel is byte for byte the original"
    else
	bad "the unwrapped real kernel differs from the original"
    fi

    REAL_ZBOOT_UKI=$TMPDIR/real_zboot_uki.efi
    REAL_PADDED=$TMPDIR/real_padded

    # The real UKI as it is: its kernel needs no unwrapping at all
    if run_zboot 2 --slot auto "$REAL_IMG"; then
	ok "the kernel of $REAL_IMG is a bare Image, left alone"
    else
	bad "the kernel of $REAL_IMG was mishandled"
	cat $TMPDIR/out
    fi
else
    echo "skipped: $REAL_IMG or ./ukiextract is missing"
fi

echo
echo "=== 6. A UKI that really was built with CONFIG_EFI_ZBOOT ==="
#
# Everything above wraps a kernel the same way this code unwraps it, so
# a shared misreading of the format would pass unnoticed. An image built
# by the distribution settles that, and the result is checked against
# two things this code had no hand in: the system gzip, and the version
# the UKI records in its own .uname section.
#
EFI_IMG=autosd10-ebbr-minimal-image-efi.img

if [ -r "$EFI_IMG" ] && [ -x ./ukiextract ]; then
    EFI_OUT=$TMPDIR/efi_vmlinux

    if run_zboot 0 --slot auto -o "$EFI_OUT" "$EFI_IMG"; then
	ok "the zboot kernel of $EFI_IMG is unwrapped"
	sed -n '4,$p' $TMPDIR/out
    else
	bad "the zboot kernel of $EFI_IMG was not unwrapped"
	cat $TMPDIR/out
    fi

    # Decompress the payload again, with a decompressor that is not ours
    off=`sed -n 's/^Payload:.*offset \([0-9]*\),.*/\1/p' $TMPDIR/out`
    len=`sed -n 's/^Payload:.*, \([0-9]*\) bytes.*/\1/p' $TMPDIR/out`

    if [ -n "$off" ] && [ -n "$len" ]; then
	dd if="$EFI_IMG" of=$TMPDIR/payload.gz bs=65536 status=none \
	   iflag=skip_bytes,count_bytes skip=$off count=$len
	gzip -dc $TMPDIR/payload.gz > $TMPDIR/gzip_out 2>/dev/null

	if cmp -s "$EFI_OUT" $TMPDIR/gzip_out; then
	    ok "it is byte for byte what the system gzip decompresses"
	else
	    bad "it differs from what the system gzip decompresses"
	    ls -l "$EFI_OUT" $TMPDIR/gzip_out
	fi
    else
	bad "could not find the payload of $EFI_IMG"
    fi

    # The kernel inside has to be the one the UKI says it carries
    ./ukiextract --slot auto -o $TMPDIR/efi_uki.efi "$EFI_IMG" > /dev/null 2>&1
    uki_section $TMPDIR/efi_uki.efi .uname $TMPDIR/efi_uname
    uname=`cat $TMPDIR/efi_uname`

    if strings -a "$EFI_OUT" | grep -q "^Linux version $uname "; then
	ok "its banner is the $uname of the .uname section"
    else
	bad "its banner is not the $uname of the .uname section"
	strings -a "$EFI_OUT" | grep -m1 "^Linux version"
    fi
else
    echo "skipped: $EFI_IMG or ./ukiextract is missing"
fi

# The UKIs built here are what test_target.sh mounts on a QNX target,
# with the checksum the vmlinux each of them holds must have
if [ -n "$KEEP" ]; then
    mkdir -p "$KEEP"
    cp "$ZBOOT_UKI" "$KEEP/zboot_uki.efi"
    cksum < "$PADDED" > "$KEEP/zboot_uki.vmlinux.cksum"
    echo
    echo "Kept $KEEP/zboot_uki.efi and the checksum of the vmlinux it holds"

    # A real kernel is what says how long unwrapping one takes on a target
    if [ -n "${REAL_ZBOOT_UKI:-}" ]; then
	cp "$REAL_ZBOOT_UKI" "$KEEP/zboot_real_uki.efi"
	cksum < "$REAL_PADDED" > "$KEEP/zboot_real_uki.vmlinux.cksum"
	echo "Kept $KEEP/zboot_real_uki.efi, built around a real kernel"
    fi
fi

echo
if [ $failures -ne 0 ]; then
    echo "=== $failures EFI zboot check(s) FAILED ==="
    exit 1
fi

echo "=== All EFI zboot checks passed ==="
