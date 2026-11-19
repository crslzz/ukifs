#!/bin/bash
#
# test_slot_demo.sh - Demonstrate the ukiboot slot selection of ukifs
#
# Builds two ukiboot disk images (GPT and MBR), each with a ukiboot_a,
# a ukiboot_b and a ukibootctl partition, then uses test_slot to resolve
# the slots the same way ukifs does when mounting a UKI. If ukiextract
# has been built, the UKIs are extracted back and compared.
#
# Requires: sfdisk, python3 and the test_slot program (make test-slot)
#

set -e

cd "$(dirname "$0")"

. ./test_images.sh

if [ ! -x ./test_slot ]; then
    echo "ERROR: ./test_slot not found, build it with: make test-slot"
    exit 1
fi

require_tools sfdisk python3

TMPDIR=$(mktemp -d /tmp/ukifs_slot_demo_XXXXXX)
trap 'rm -rf "$TMPDIR"' EXIT

GPT_IMG=$TMPDIR/gpt.img
DOS_IMG=$TMPDIR/dos.img
UKI_A=$TMPDIR/uki_a.efi
UKI_B=$TMPDIR/uki_b.efi
CTL=$TMPDIR/bootctl.bin

echo "=== Creating test UKIs ==="
make_uki "$UKI_A" slota
make_uki "$UKI_B" slotb
ls -l "$UKI_A" "$UKI_B"

echo
echo "=== Creating GPT ukiboot image ==="
make_gpt_image "$GPT_IMG" "$UKI_A" "$UKI_B"
sfdisk -l "$GPT_IMG" | tail -4

echo
echo "=== Creating MBR ukiboot image ==="
make_dos_image "$DOS_IMG" "$UKI_A" "$UKI_B"
sfdisk -l "$DOS_IMG" | tail -5

run_test() {
    echo
    echo "---------------------------------------------------------------"
    echo "\$ ./test_slot $*"
    echo "---------------------------------------------------------------"
    ./test_slot "$@"
}

for img in "$GPT_IMG" "$DOS_IMG"; do
    case $img in
	"$GPT_IMG") scheme=GPT; ctl_lba=$GPT_CTL_LBA ;;
	*)          scheme=MBR; ctl_lba=$DOS_CTL_LBA ;;
    esac

    echo
    echo "==============================================================="
    echo " $scheme image: $img"
    echo "==============================================================="

    # Manual selection
    run_test "$img" a
    run_test "$img" b

    # Automatic selection: slot a active (default ukiboot state)
    make_bootctl "$CTL" 15 7 0 14 7 0
    write_at "$img" "$CTL" $ctl_lba
    run_test "$img" auto

    # Automatic selection after a switch to slot b
    make_bootctl "$CTL" 14 0 1 15 7 0
    write_at "$img" "$CTL" $ctl_lba
    run_test "$img" auto

    # Slot b used up its tries and never booted successfully: back to a
    make_bootctl "$CTL" 14 0 1 15 0 0
    write_at "$img" "$CTL" $ctl_lba
    run_test "$img" auto

    # Uninitialized ukibootctl: ukiboot defaults apply (slot a)
    dd if=/dev/zero of="$img" bs=512 seek=$ctl_lba count=1 conv=notrunc status=none
    run_test "$img" auto

    # Standalone ukibootctl partition (--bootctl), selecting slot b
    make_bootctl "$CTL" 14 0 1 15 7 0
    run_test --bootctl "$CTL" "$img" auto
done

# --- Signed UKI in a slot --------------------------------------------
#
# The Authenticode digest covers the PE headers, the sections and the
# certificate table, so the padding of the slot partition is not part of
# it: a UKI signed before being written to a slot verifies in place.

OPENSSL=
for candidate in /usr/bin/openssl openssl; do
    if command -v "$candidate" >/dev/null 2>&1; then
	OPENSSL=$candidate
	break
    fi
done

if command -v sbsign >/dev/null 2>&1 && [ -n "$OPENSSL" ] &&
   $OPENSSL req -new -x509 -newkey rsa:2048 -nodes \
	-keyout "$TMPDIR/key.pem" -out "$TMPDIR/cert.pem" \
	-days 1 -subj "/CN=ukifs slot test" >/dev/null 2>&1; then
    echo
    echo "==============================================================="
    echo " Signed UKI in slot b of the GPT image"
    echo "==============================================================="

    sbsign --key "$TMPDIR/key.pem" --cert "$TMPDIR/cert.pem" \
	--output "$TMPDIR/uki_b_signed.efi" "$UKI_B" >/dev/null
    write_at "$GPT_IMG" "$TMPDIR/uki_b_signed.efi" $GPT_B_LBA
    GPT_UKI_B=$TMPDIR/uki_b_signed.efi
    CERT=$TMPDIR/cert.pem

    run_test --cert "$TMPDIR/cert.pem" "$GPT_IMG" b
else
    echo
    echo "(sbsign/openssl not usable here, skipping the signed UKI test)"
fi

# --- Extraction round-trip (ukiextract) ------------------------------
#
# Extracting a slot must give back exactly the UKI that was written to
# it: same bytes, padding of the slot excluded.

echo
echo "==============================================================="
echo " Extracting the UKIs back with ukiextract"
echo "==============================================================="

if [ ! -x ./ukiextract ]; then
    echo "(./ukiextract not found, build it with 'make utils' - skipping)"
else
    failures=0

    check_extract() {
	# $1: image, $2: slot, $3: expected UKI file, $4...: extra options
	local img=$1 slot=$2 expected=$3
	shift 3
	local out=$TMPDIR/extracted_$$.efi

	./ukiextract "$@" --slot "$slot" -o "$out" "$img" >/dev/null

	if cmp -s "$out" "$expected"; then
	    echo "OK    slot $slot of $(basename "$img") == $(basename "$expected") ($(stat -c %s "$out") bytes)"
	else
	    echo "FAIL  slot $slot of $(basename "$img") differs from $(basename "$expected")"
	    failures=$((failures + 1))
	fi
	rm -f "$out"
    }

    check_extract "$GPT_IMG" a "$UKI_A"
    check_extract "$GPT_IMG" b "${GPT_UKI_B:-$UKI_B}"
    check_extract "$DOS_IMG" a "$UKI_A"
    check_extract "$DOS_IMG" b "$UKI_B"

    # Automatic slot selection, and extraction to stdout
    make_bootctl "$CTL" 14 0 1 15 7 0
    write_at "$GPT_IMG" "$CTL" $GPT_CTL_LBA
    ./ukiextract -o - "$GPT_IMG" > "$TMPDIR/auto.efi"
    if cmp -s "$TMPDIR/auto.efi" "${GPT_UKI_B:-$UKI_B}"; then
	echo "OK    active slot of $(basename "$GPT_IMG") extracted to stdout"
    else
	echo "FAIL  active slot extraction to stdout"
	failures=$((failures + 1))
    fi

    # Extraction of a signed UKI, with verification before extracting
    if [ -n "${CERT:-}" ]; then
	check_extract "$GPT_IMG" b "$GPT_UKI_B" --cert "$CERT"
	echo "OK    signature verified before extracting slot b"
    fi

    # The whole slot partition, padding included
    ./ukiextract --slot a --whole-slot -o "$TMPDIR/whole.efi" "$GPT_IMG" >/dev/null
    whole_size=$(stat -c %s "$TMPDIR/whole.efi")
    if [ "$whole_size" = "1048576" ]; then
	echo "OK    --whole-slot extracted the full $whole_size byte partition"
    else
	echo "FAIL  --whole-slot extracted $whole_size bytes, expected 1048576"
	failures=$((failures + 1))
    fi

    if [ "$failures" -ne 0 ]; then
	echo
	echo "=== $failures extraction test(s) FAILED ==="
	exit 1
    fi
fi

echo
echo "=== All slot selection and extraction tests completed ==="
