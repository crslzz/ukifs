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

start_ukifs() {
    # $@: extra ukifs options
    # A stopped instance may still write to its log, so give each run
    # its own file rather than truncating one under its feet
    runs=$((runs + 1))
    LOG=$LOGBASE.$runs.log
    $UKIFS -v "$@" $DEV $MNT > $LOG 2>&1 &
    waitfor $MNT 10 > /dev/null 2>&1
    if [ ! -d $MNT ]; then
	echo "--- ukifs log ---"
	cat $LOG
	return 1
    fi
    return 0
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
if [ $failures -ne 0 ]; then
    echo "=== $failures check(s) FAILED ==="
    exit 1
fi

echo "=== All checks passed ==="
