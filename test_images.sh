#!/bin/bash
#
# test_images.sh - Helpers to build ukiboot test images
#
# Sourced by test_slot_demo.sh and test_blkdev.sh. Builds minimal UKIs
# (PE/COFF images with UKI sections), ukibootctl contents, and GPT or MBR
# ukiboot disk images laid out as the ukiboot project defines them.
#
# Requires: sfdisk and python3
#

# Partition layout, in 512 byte sectors
GPT_A_LBA=2048
GPT_B_LBA=4096
GPT_CTL_LBA=6144
DOS_EXT_LBA=2048
DOS_A_LBA=2050
DOS_B_LBA=4100
DOS_CTL_LBA=6150

# --- Minimal UKIs (PE/COFF images with UKI sections) ------------------

# make_uki <image> <tag> [kernel-file]
#
# The kernel section holds a short marker unless a file is given, in
# which case its content is used: that is how a UKI carrying a real
# Image, or an EFI zboot image, is built for a test.
make_uki() {
    python3 -I - "$1" "$2" "${3:-}" <<'EOF'
import struct, sys

path, tag, kernel = sys.argv[1], sys.argv[2], sys.argv[3]

if kernel:
    with open(kernel, 'rb') as f:
        linux = f.read()
else:
    linux = b'KERNEL_' + tag.encode() + b'_DATA'

sections = [
    ('.osrel',   b'NAME="TestOS"\nVERSION="%s"\n' % tag.encode()),
    ('.uname',   b'6.12.0-' + tag.encode()),
    ('.cmdline', b'root=/dev/sda1 slot=' + tag.encode()),
    ('.linux',   linux),
    ('.initrd',  b'INITRD_' + tag.encode() + b'_DATA'),
]

nt_offset = 64
opt_header_size = 240
headers_size = nt_offset + 4 + 20 + opt_header_size + len(sections) * 40
aligned_headers = (headers_size + 511) & ~511

dos = bytearray(64)
struct.pack_into('<H', dos, 0, 0x5A4D)
struct.pack_into('<i', dos, 60, nt_offset)

sig = struct.pack('<I', 0x00004550)
file_hdr = struct.pack('<HHIIIHH', 0x8664, len(sections), 0, 0, 0,
                       opt_header_size, 0x22)

opt = bytearray(opt_header_size)
struct.pack_into('<H', opt, 0, 0x20b)              # PE32+
struct.pack_into('<I', opt, 56, aligned_headers)   # SizeOfImage
struct.pack_into('<I', opt, 60, aligned_headers)   # SizeOfHeaders
struct.pack_into('<I', opt, 108, 16)               # NumberOfRvaAndSizes

offset = aligned_headers
sec_hdrs = bytearray()
sec_data = bytearray()
for name, content in sections:
    raw_size = (len(content) + 511) & ~511
    shdr = bytearray(40)
    shdr[0:8] = name.encode('ascii')[:8].ljust(8, b'\x00')
    struct.pack_into('<I', shdr, 8, len(content))   # VirtualSize
    struct.pack_into('<I', shdr, 12, offset)        # VirtualAddress
    struct.pack_into('<I', shdr, 16, raw_size)      # SizeOfRawData
    struct.pack_into('<I', shdr, 20, offset)        # PointerToRawData
    sec_hdrs += shdr
    sec_data += content.ljust(raw_size, b'\x00')
    offset += raw_size

pe = bytes(dos) + sig + file_hdr + bytes(opt) + bytes(sec_hdrs)
pe = pe.ljust(aligned_headers, b'\x00') + bytes(sec_data)

with open(path, 'wb') as f:
    f.write(pe)
EOF
}

# --- reading a UKI ----------------------------------------------------

# uki_section <uki-file> <section> <output>
#
# Writes the content of a PE section of a UKI, '.linux' and the like,
# the way ukifs reads it. The UKI must start at the beginning of the
# file, so extract a slot with ukiextract first.
uki_section() {
    python3 -I - "$1" "$2" "$3" <<'EOF'
import struct, sys

path, want, out = sys.argv[1], sys.argv[2].encode(), sys.argv[3]

with open(path, 'rb') as f:
    uki = f.read()

pe = struct.unpack_from('<i', uki, 60)[0]
nsec = struct.unpack_from('<H', uki, pe + 6)[0]
optsz = struct.unpack_from('<H', uki, pe + 20)[0]
table = pe + 24 + optsz

for i in range(nsec):
    name, vsize, _, _, raw = struct.unpack_from('<8sIIII', uki, table + i * 40)
    if name.rstrip(b'\x00') == want:
        with open(out, 'wb') as f:
            f.write(uki[raw:raw + vsize])
        sys.exit(0)

sys.stderr.write("no %s section in %s\n" % (sys.argv[2], path))
sys.exit(1)
EOF
}

# --- arm64 kernels, bare and EFI zboot --------------------------------

# make_arm64_image <image> [text-size]
#
# A bare arm64 Image: the 64 byte boot header the kernel starts with,
# the PE/COFF headers of the EFI stub overlaid on it, and a body that
# compresses like code rather than like a run of zeroes.
make_arm64_image() {
    python3 -I - "$1" "${2:-262144}" <<'EOF'
import random, struct, sys

path, text_size = sys.argv[1], int(sys.argv[2])

# The in-memory image is larger than the file: the kernel pads what it
# compresses up to this size, BSS included
image_size = text_size + 65536

img = bytearray(64)
struct.pack_into('<I', img, 0, 0xfa405a4d)      # code0: "MZ" and a valid insn
struct.pack_into('<I', img, 4, 0x1473bc27)      # code1: b stext
struct.pack_into('<Q', img, 8, 0)               # text_offset
struct.pack_into('<Q', img, 16, image_size)     # image_size
struct.pack_into('<Q', img, 24, 0xa)            # flags
img[56:60] = b'ARMd'                            # arm64 magic
struct.pack_into('<I', img, 60, 0x40)           # PE header offset

# PE/COFF headers of the EFI stub, enough to be recognized as a PE
pe = bytearray(248)
struct.pack_into('<I', pe, 0, 0x00004550)       # "PE\0\0"
struct.pack_into('<HH', pe, 4, 0xaa64, 1)       # aarch64, 1 section
struct.pack_into('<HH', pe, 20, 160, 0x206)     # optional header size, flags
struct.pack_into('<H', pe, 24, 0x20b)           # PE32+
img += pe

random.seed(int.from_bytes(path.encode(), 'little') & 0xffffffff)
img += bytes(random.getrandbits(8) for _ in range(text_size - len(img)))

with open(path, 'wb') as f:
    f.write(bytes(img))
EOF
}

# make_zboot <zboot-image> <arm64-image>
#
# Wraps a kernel the way CONFIG_EFI_ZBOOT does: the payload is the
# kernel padded to its in-memory size and gzipped, behind a DOS header
# carrying "zimg", the offset and size of that payload, and the name of
# the compression used. See drivers/firmware/efi/libstub/zboot-header.S
make_zboot() {
    python3 -I - "$1" "$2" <<'EOF'
import gzip, struct, sys

path, kernel = sys.argv[1], sys.argv[2]

with open(kernel, 'rb') as f:
    image = f.read()

# The kernel build pads what it compresses to the in-memory size of the
# image, BSS included, which is the word at offset 16 of the header
image_size = struct.unpack_from('<I', image, 16)[0]
payload = gzip.compress(image.ljust(image_size, b'\x00'), mtime=0)

payload_offset = 4096

hdr = bytearray(payload_offset)
struct.pack_into('<I', hdr, 0, 0x5A4D)          # IMAGE_DOS_SIGNATURE
hdr[4:8] = b'zimg'
struct.pack_into('<I', hdr, 8, payload_offset)
struct.pack_into('<I', hdr, 12, len(payload))
hdr[24:28] = b'gzip'                            # compression type, NUL padded
struct.pack_into('<I', hdr, 0x38, 0x818223cd)   # LINUX_PE_MAGIC
struct.pack_into('<I', hdr, 0x3c, 0x40)         # PE header offset

struct.pack_into('<I', hdr, 0x40, 0x00004550)   # "PE\0\0"
struct.pack_into('<HH', hdr, 0x44, 0xaa64, 2)   # aarch64, 2 sections
struct.pack_into('<HH', hdr, 0x58, 160, 0x206)  # optional header size, flags
struct.pack_into('<H', hdr, 0x5c, 0x20b)        # PE32+

with open(path, 'wb') as f:
    f.write(bytes(hdr) + payload)

# What the unwrapped kernel must turn out to be
sys.stderr.write("zboot: %u bytes compressed, %u bytes unwrapped\n"
                 % (len(payload), image_size))
EOF
}

# padded_image <output> <arm64-image>
#
# The kernel as an EFI zboot payload decompresses to it: the Image
# zero-padded to the in-memory size its header announces.
padded_image() {
    python3 -I - "$1" "$2" <<'EOF'
import struct, sys

with open(sys.argv[2], 'rb') as f:
    image = f.read()

image_size = struct.unpack_from('<I', image, 16)[0]

with open(sys.argv[1], 'wb') as f:
    f.write(image.ljust(image_size, b'\x00'))
EOF
}

# --- ukibootctl content ----------------------------------------------
#
# struct UkiBootCtrl { magic, slots[2]{prio, tries, successful, resv},
#                      reserved[8], CRC32 }

make_bootctl() {
    # $1: output file, $2..: prio_a tries_a ok_a prio_b tries_b ok_b
    python3 -I - "$@" <<'EOF'
import struct, sys, zlib

path = sys.argv[1]
pa, ta, oa, pb, tb, ob = (int(v) for v in sys.argv[2:8])

data = struct.pack('<IBBBBBBBB8s', 1420550408,
                   pa, ta, oa, 0,
                   pb, tb, ob, 0,
                   b'\x00' * 8)
data += struct.pack('<I', zlib.crc32(data) & 0xffffffff)

with open(path, 'wb') as f:
    f.write(data)
EOF
}

write_at() {
    # $1: image, $2: file to write, $3: lba
    dd if="$2" of="$1" bs=512 seek="$3" conv=notrunc status=none
}

# --- ukiboot disk images ---------------------------------------------

# make_gpt_image <image> <uki_a> <uki_b>
make_gpt_image() {
    truncate -s 8M "$1"
    sfdisk --quiet "$1" >/dev/null <<EOF
label: gpt
unit: sectors

start=$GPT_A_LBA,   size=2048, type=DF331E4D-BE00-463F-B4A7-8B43E18FB53A, name="ukiboot_a"
start=$GPT_B_LBA,   size=2048, type=DF331E4D-BE00-463F-B4A7-8B43E18FB53A, name="ukiboot_b"
start=$GPT_CTL_LBA, size=8,    type=FEFD9070-346F-4C9A-85E6-17F07F922773, name="ukibootctl"
EOF
    [ -n "$2" ] && write_at "$1" "$2" $GPT_A_LBA
    [ -n "$3" ] && write_at "$1" "$3" $GPT_B_LBA
    return 0
}

# make_dos_image <image> <uki_a> <uki_b>
make_dos_image() {
    truncate -s 8M "$1"
    sfdisk --quiet "$1" >/dev/null <<EOF
label: dos
unit: sectors

start=$DOS_EXT_LBA, size=10240, type=5
start=$DOS_A_LBA,   size=2048,  type=46
start=$DOS_B_LBA,   size=2048,  type=47
start=$DOS_CTL_LBA, size=8,     type=48
EOF
    [ -n "$2" ] && write_at "$1" "$2" $DOS_A_LBA
    [ -n "$3" ] && write_at "$1" "$3" $DOS_B_LBA
    return 0
}

# require_tools <tool>...
require_tools() {
    for tool in "$@"; do
	if ! command -v "$tool" >/dev/null 2>&1; then
	    echo "ERROR: $tool is required"
	    exit 1
	fi
    done
}
