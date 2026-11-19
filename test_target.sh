#!/bin/sh
#
# test_target.sh - Exercise ukifs on a QNX target
#
# Runs ON the QNX target, against a ukiboot image that is usually a
# partition reached through a block device. It mounts the active slot,
# reads every section through the filesystem, and checks that the two
# read paths of the driver (data served from the mapping, and data read
# with pread() when the source cannot be mapped or --no-mmap is given)
# return exactly the same bytes.
#
# Copy ukifs and this script to the target, then:
#   DEV=/dev/sd0t131 sh test_target.sh
#
# Variables:
#   UKIFS  path to the ukifs binary       (default /tmp/ukifs)
#   DEV    ukiboot image to mount         (default /dev/sd0t131)
#   MNT    mountpoint to attach           (default /mnt/uki)
#
# Note: on QNX the utilities live in /proc/boot, and a binary cannot be
# executed from a DOS filesystem, so keep ukifs on /tmp. The script only
# uses what a minimal QNX image provides: no cmp, expr or wc here.
#

PATH=/proc/boot:$PATH
export PATH

UKIFS=${UKIFS:-/tmp/ukifs}
DEV=${DEV:-/dev/sd0t131}
MNT=${MNT:-/mnt/uki}
UKIFS_NAME=${UKIFS##*/}
LOGBASE=/tmp/ukifs_test
LOG=$LOGBASE.0.log
SUMS=/tmp/ukifs_sums

failures=0
runs=0

ok() {
    echo "OK    $*"
}

bad() {
    echo "FAIL  $*"
    failures=$((failures + 1))
}

start_ukifs_src() {
    # $1: what to mount, $@: extra ukifs options
    # A stopped instance may still write to its log, so give each run
    # its own file rather than truncating one under its feet
    src=$1
    shift
    runs=$((runs + 1))
    LOG=$LOGBASE.$runs.log
    $UKIFS -v "$@" $src $MNT > $LOG 2>&1 &
    waitfor $MNT 60 > /dev/null 2>&1
    if [ ! -d $MNT ]; then
	echo "--- ukifs log ---"
	cat $LOG
	return 1
    fi
    return 0
}

start_ukifs() {
    start_ukifs_src $DEV "$@"
}

ukifs_running() {
    # pidin prints process names without their leading slash and padded
    # with spaces, so /tmp/ukifs shows up as "tmp/ukifs    "
    pidin -F "%n" 2>/dev/null | grep "/$UKIFS_NAME *\$" > /dev/null 2>&1
}

watchdog_fired() {
    grep -e "timed out" -e "Forced shutdown" $LOG > /dev/null 2>&1
}

stop_ukifs() {
    slay -f ukifs > /dev/null 2>&1
    sleep 1
}

checksum_sections() {
    # $1: file to write the checksums to
    for section in cmdline data initrd linux osrel reloc rodata sbat sdmagic text uname; do
	if [ -f $MNT/$section ]; then
	    echo "$section `cksum < $MNT/$section`"
	fi
    done > $1
}

echo "=== ukifs on $DEV ==="
$UKIFS -h 2>&1 | sed -n '1,3p'

echo
echo "=== 1. Mounting the active slot ==="
if start_ukifs --slot auto; then
    ok "filesystem attached at $MNT"
else
    bad "filesystem did not attach"
    exit 1
fi
grep INFO $LOG

echo
echo "=== 2. Directory listing ==="
ls -l $MNT
for section in linux initrd cmdline uname osrel; do
    if [ -f $MNT/$section ]; then
	ok "$section is present"
    else
	bad "$section is missing"
    fi
done

echo
echo "=== 3. Text sections ==="
echo "uname:   `cat $MNT/uname`"
echo "sdmagic: `cat $MNT/sdmagic`"
echo "osrel:"
cat $MNT/osrel
echo "cmdline:"
cat $MNT/cmdline
echo

echo
echo "=== 4. Checksums of every section, read from the mapping ==="
checksum_sections $SUMS.mapped
cat $SUMS.mapped

echo
echo "=== 5. Partial and seeking reads ==="
dd if=$MNT/linux bs=512 count=2048 2>/dev/null | cksum
dd if=$MNT/linux bs=512 skip=100000 count=64 2>/dev/null | cksum
dd if=$MNT/initrd bs=4096 count=16 2>/dev/null | cksum

echo
echo "=== 6. Read-only and missing files ==="
cat $MNT/does_not_exist > /dev/null 2>&1
if [ $? -ne 0 ]; then
    ok "missing file rejected"
else
    bad "missing file was opened"
fi

echo written > $MNT/uname 2>/dev/null
if [ $? -ne 0 ]; then
    ok "write rejected on the read-only filesystem"
else
    bad "write accepted on the read-only filesystem"
fi

stop_ukifs

echo
echo "=== 7. Same slot, read with pread() instead of the mapping ==="
if start_ukifs --slot auto --no-mmap; then
    ok "filesystem attached with --no-mmap"
    grep -e "Mapping disabled" -e "Cannot map" $LOG
    checksum_sections $SUMS.pread

    mapped_sum=`cksum < $SUMS.mapped`
    pread_sum=`cksum < $SUMS.pread`

    if [ "$mapped_sum" = "$pread_sum" ]; then
	ok "pread() and mapped reads return identical data"
    else
	bad "pread() and mapped reads differ"
	echo "--- mapped ---"; cat $SUMS.mapped
	echo "--- pread  ---"; cat $SUMS.pread
    fi
    stop_ukifs
else
    bad "filesystem did not attach with --no-mmap"
fi

echo
echo "=== 8. Explicit slots ==="
for slot in a b; do
    if start_ukifs --slot $slot; then
	entries=0
	for entry in $MNT/*; do
	    entries=$((entries + 1))
	done
	echo "slot $slot: $entries entries, uname: `cat $MNT/uname 2>/dev/null`"
	ok "slot $slot mounted"
	stop_ukifs
    else
	echo "slot $slot: not mountable"
	grep ERROR $LOG
    fi
done

echo
echo "=== 9. Unmounting ==="
if start_ukifs --slot auto; then
    # A client holding a file open must keep the filesystem mounted
    exec 3< $MNT/uname
    umount $MNT > /dev/null 2>&1
    if [ $? -ne 0 ]; then
	ok "umount refused while a file is open"
    else
	bad "umount accepted while a file is open"
    fi

    exec 3<&-
    umount $MNT > /dev/null 2>&1
    if [ $? -eq 0 ]; then
	ok "umount accepted once the file is closed"
    else
	bad "umount refused after the file was closed"
    fi

    sleep 1
    if [ -d $MNT ]; then
	bad "$MNT is still there after umount"
    else
	ok "$MNT is gone after umount"
    fi

    if ukifs_running; then
	bad "ukifs is still running after umount"
    else
	ok "ukifs exited after umount"
    fi

    if watchdog_fired; then
	bad "shutdown did not complete in time"
	grep -e "timed out" -e "Forced shutdown" $LOG
    else
	ok "shutdown completed without the watchdog firing"
    fi
else
    bad "filesystem did not attach for the unmount test"
fi

if start_ukifs --slot auto; then
    if ukifs_running; then
	ok "ukifs is running before the signal test"
    else
	bad "ukifs is not running before the signal test"
    fi

    slay -f ukifs > /dev/null 2>&1
    sleep 2

    if [ -d $MNT ]; then
	bad "$MNT is still there after SIGTERM"
    else
	ok "$MNT is gone after SIGTERM"
    fi

    if ukifs_running; then
	bad "ukifs is still running after SIGTERM"
    else
	ok "ukifs exited on SIGTERM"
    fi

    if watchdog_fired; then
	bad "SIGTERM shutdown did not complete in time"
	grep -e "timed out" -e "Forced shutdown" $LOG
    else
	ok "SIGTERM shutdown completed without the watchdog firing"
    fi
else
    bad "filesystem did not attach for the signal test"
fi

if start_ukifs --slot auto; then
    exec 3< $MNT/linux
    umount -f $MNT > /dev/null 2>&1
    if [ $? -eq 0 ]; then
	ok "umount -f accepted while a file is open"
    else
	bad "umount -f refused while a file is open"
    fi
    exec 3<&-
    sleep 1

    if ukifs_running; then
	bad "ukifs is still running after umount -f"
    else
	ok "ukifs exited after umount -f"
    fi
else
    bad "filesystem did not attach for the forced unmount test"
fi

echo
echo "=== 10. Plain UKI file, without a slot ==="
#
# The UKI of the slot is copied out and mounted as an ordinary file, so
# that the path ukifs takes when it is given a UKI rather than a ukiboot
# image is covered, and so that both paths can be shown to serve exactly
# the same bytes.
#
UKI_FILE=/tmp/ukifs_plain.efi

if start_ukifs --slot auto; then
    uki_size=`grep "UKI size:" $LOG | sed -e "s/.*UKI size: \([0-9]*\) bytes.*/\1/"`
    slot_off=`grep "Using ukiboot slot" $LOG | sed -e "s/.*at offset \([0-9]*\).*/\1/"`
    stop_ukifs

    if [ -z "$uki_size" ] || [ -z "$slot_off" ]; then
	bad "could not read the UKI size and offset from the log"
    else
	echo "copying $uki_size bytes from offset $slot_off into $UKI_FILE"
	dd if=$DEV of=$UKI_FILE bs=512 skip=$((slot_off / 512)) \
	   count=$(((uki_size + 511) / 512)) 2>/dev/null

	if start_ukifs_src $UKI_FILE; then
	    ok "a plain UKI file mounts without a slot"
	    grep -e "Source:" -e "Mapped" $LOG

	    checksum_sections $SUMS.plain
	    if [ "`cksum < $SUMS.mapped`" = "`cksum < $SUMS.plain`" ]; then
		ok "the file and the slot serve identical data"
	    else
		bad "the file and the slot differ"
		echo "--- slot ---"; cat $SUMS.mapped
		echo "--- file ---"; cat $SUMS.plain
	    fi
	    stop_ukifs
	else
	    bad "a plain UKI file did not mount"
	fi

	if start_ukifs_src $UKI_FILE --no-mmap; then
	    checksum_sections $SUMS.plain_pread
	    if [ "`cksum < $SUMS.mapped`" = "`cksum < $SUMS.plain_pread`" ]; then
		ok "a plain UKI file read with pread() serves identical data"
	    else
		bad "pread() on a plain UKI file differs from the slot"
	    fi
	    stop_ukifs
	else
	    bad "a plain UKI file did not mount with --no-mmap"
	fi

	if start_ukifs_src $UKI_FILE --eager-map; then
	    ok "a plain UKI file mounts with --eager-map"
	    stop_ukifs
	else
	    bad "a plain UKI file did not mount with --eager-map"
	fi
    fi

    rm -f $UKI_FILE
else
    bad "the slot did not mount, cannot extract a plain UKI file"
fi

# A signed UKI and its certificate, if the caller provides them, cover
# the authentication of a UKI. The image of a target may well carry an
# unsigned UKI, which cannot cover any of this.
if [ -n "${SIGNED_UKI:-}" ] && [ -n "${CERT:-}" ]; then
    echo
    echo "=== 11. Authentication ==="

    if start_ukifs_src $SIGNED_UKI --cert $CERT; then
	ok "a signed UKI mounts with its certificate"
	grep "verified successfully" $LOG
	stop_ukifs
    else
	bad "a signed UKI did not mount with its certificate"
    fi

    # A file that is not a certificate at all must be refused. The UKI
    # itself does the job and needs no second artifact.
    if start_ukifs_src $SIGNED_UKI --cert $SIGNED_UKI; then
	bad "a signed UKI mounted with a file that is not a certificate"
	stop_ukifs
    else
	ok "a UKI is refused when the certificate cannot be read"
	grep -e "Failed to parse" -e "verification failed" $LOG
    fi

    # A valid certificate from another authority must be refused too.
    # That is a different path: the digest matches, the signer does not.
    if [ -n "${WRONG_CERT:-}" ]; then
	if start_ukifs_src $SIGNED_UKI --cert $WRONG_CERT; then
	    bad "a signed UKI mounted with a certificate of another authority"
	    stop_ukifs
	else
	    ok "a UKI is refused when it was signed by another authority"
	    grep -e "Signer certificate" $LOG
	fi
    fi

    # A single modified byte in the UKI has to break the signature
    TAMPERED=/tmp/ukifs_tampered.efi
    cp $SIGNED_UKI $TAMPERED
    dd if=/dev/zero of=$TAMPERED bs=1 seek=100000 count=1 conv=notrunc 2>/dev/null

    if start_ukifs_src $TAMPERED --cert $CERT; then
	bad "a tampered UKI mounted"
	stop_ukifs
    else
	ok "a tampered UKI is refused"
	grep "hash mismatch" $LOG
    fi

    rm -f $TAMPERED

    # Without a certificate the UKI is served unverified, as documented
    if start_ukifs_src $SIGNED_UKI; then
	ok "a signed UKI mounts unverified when no certificate is given"
	stop_ukifs
    else
	bad "a signed UKI did not mount without a certificate"
    fi
fi

echo
if [ $failures -ne 0 ]; then
    echo "=== $failures check(s) FAILED ==="
    exit 1
fi

echo "=== All checks passed ==="
