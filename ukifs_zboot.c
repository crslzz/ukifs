/*
 * EFI zboot kernel images
 *
 * The kernel section of a UKI is usually a bare Image: on arm64 the
 * PE/COFF headers of the EFI stub are overlaid on the boot header of
 * the kernel, so the very same bytes are both an EFI application and a
 * kernel a non-EFI loader can start. Nothing has to be unwrapped there.
 *
 * CONFIG_EFI_ZBOOT builds a different kernel. The section is then a
 * small EFI application that carries the real Image compressed in its
 * payload and decompresses it at boot, which only works when there is
 * an EFI firmware to run it. A loader such as qvm is handed the section
 * as it is and cannot boot it.
 *
 * This unwraps that container: the payload is decompressed and served
 * as an extra 'vmlinux' file, next to the 'linux' section it comes
 * from, which stays available untouched.
 *
 * The layout comes from the kernel's drivers/firmware/efi/libstub/
 * zboot-header.S and zboot.lds.
 */

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <zlib.h>

#include "ukifs_zboot.h"
#include "ukifs_pe.h"
#include "ukifs_utils.h"

/*
 * ukiboot's utils.h, which has full_pread(), declares a crc32() that
 * clashes with the one of zlib.h, so the few reads done here use their
 * own helper rather than dragging both headers into this file.
 *
 * Reads exactly count bytes, retrying the short reads a device may
 * return. Returns true on success.
 */
static bool zboot_pread(int fd, void *buf, size_t count, off_t offset) {
    uint8_t *p = buf;

    while (count > 0) {
        ssize_t n = pread(fd, p, count, offset);

        if (n < 0) {
            ERROR_LOG("pread failed: %s", strerror(errno));
            return false;
        }

        if (n == 0) {
            ERROR_LOG("Unexpected end of file");
            return false;
        }

        p += n;
        count -= (size_t)n;
        offset += n;
    }

    return true;
}

/* Marks a PE image as an EFI zboot container */
#define UKI_ZBOOT_MAGIC "zimg"

/* Magic the Linux EFI stub writes at offset 0x38 of a bootable PE image */
#define UKI_LINUX_PE_MAGIC 0x818223cd

/* Magic at offset 56 of an arm64 Image, used for logging only */
#define UKI_ARM64_MAGIC "ARMd"

/* A gzip stream is at least a 10 byte header and an 8 byte trailer */
#define UKI_ZBOOT_MIN_PAYLOAD 18

/* Compressed data read from the source at a time while decompressing */
#define UKI_ZBOOT_CHUNK (64 * 1024)

/*
 * The MS-DOS header of an EFI zboot image, as laid out by the kernel.
 * All fields are little-endian, like the PE headers next to them.
 */
typedef struct {
    uint16_t mz_magic;            /* 0x00 IMAGE_DOS_SIGNATURE */
    uint8_t  mz_pad[2];
    uint8_t  zimg[4];             /* 0x04 "zimg" */
    uint32_t payload_offset;      /* 0x08 Start of the compressed payload */
    uint32_t payload_size;        /* 0x0c Size of the compressed payload */
    uint32_t reserved[2];         /* 0x10 */
    char     comp_type[UKI_ZBOOT_COMP_SIZE];  /* 0x18 NUL terminated */
    uint32_t linux_pe_magic;      /* 0x38 */
    uint32_t pe_header_offset;    /* 0x3c */
} PACKED uki_zboot_header_t;

/*
 * The compression type as it can be put in a message: the field is a
 * fixed size array in the image, and a damaged one must not end up in
 * a log line as it is.
 */
static void copy_comp_type(char *dst, const char *src) {
    size_t len = strnlen(src, UKI_ZBOOT_COMP_SIZE);

    for (size_t i = 0; i < len; i++) {
        dst[i] = isprint((unsigned char)src[i]) ? src[i] : '?';
    }

    dst[len] = '\0';
}

/*
 * Examine the kernel section of a UKI, see ukifs_zboot.h
 */
uki_zboot_t *uki_zboot_probe(int fd, off_t kernel_pos, uint32_t kernel_size) {
    DEBUG_ENTER();

    uki_zboot_header_t hdr;

    if (kernel_size < sizeof(hdr)) {
        DEBUG_LOG("Kernel section is too small to hold a PE image");
        DEBUG_EXIT(0);
        return NULL;
    }

    if (!zboot_pread(fd, &hdr, sizeof(hdr), kernel_pos)) {
        ERROR_LOG("Failed to read the header of the kernel section");
        DEBUG_EXIT(0);
        return NULL;
    }

    if (hdr.mz_magic != IMAGE_DOS_SIGNATURE) {
        DEBUG_LOG("Kernel section is not a PE image");
        DEBUG_EXIT(0);
        return NULL;
    }

    if (memcmp(hdr.zimg, UKI_ZBOOT_MAGIC, sizeof(hdr.zimg)) != 0) {
        /* A PE kernel that is not a zboot container carries the EFI
         * stub on top of a bare Image, which boots as it is */
        INFO_LOG("The kernel is a bare Image with an EFI stub, "
                 "there is nothing to unwrap");
        DEBUG_EXIT(0);
        return NULL;
    }

    DEBUG_VALX("linux_pe_magic", hdr.linux_pe_magic);

    if (hdr.linux_pe_magic != UKI_LINUX_PE_MAGIC) {
        ERROR_LOG("Warning: the kernel claims to be an EFI zboot image but "
                  "carries no Linux PE magic, ignoring it");
        DEBUG_EXIT(0);
        return NULL;
    }

    char comp_type[UKI_ZBOOT_COMP_SIZE + 1];

    copy_comp_type(comp_type, hdr.comp_type);

    /* The payload must lie inside the section it is announced in */
    uint64_t payload_end = (uint64_t)hdr.payload_offset + hdr.payload_size;

    if (hdr.payload_offset < sizeof(hdr) ||
        hdr.payload_size < UKI_ZBOOT_MIN_PAYLOAD ||
        payload_end > kernel_size) {
        ERROR_LOG("Warning: the EFI zboot payload (offset %u, %u bytes) does "
                  "not fit in the %u byte kernel section, ignoring it",
                  hdr.payload_offset, hdr.payload_size, kernel_size);
        DEBUG_EXIT(0);
        return NULL;
    }

    /*
     * Only gzip is handled. zstd is the other type the kernel builds,
     * and QNX has no zstd library to decompress it with; saying so is
     * more useful than exposing a vmlinux nothing can read.
     */
    if (strcmp(comp_type, "gzip") != 0) {
        ERROR_LOG("Warning: the kernel is an EFI zboot image compressed with "
                  "'%s', which this build cannot decompress, so no %s is "
                  "exposed", comp_type, UKI_ZBOOT_NAME);
        DEBUG_EXIT(0);
        return NULL;
    }

    /*
     * The size of the decompressed kernel is the little-endian 32 bit
     * word ending the compressed blob. For gzip that is the ISIZE of
     * the stream itself; the other compression types have it appended
     * after the payload, which is why the kernel reads it the same way
     * whatever the type (see payload_size in zboot.lds).
     */
    uint32_t image_size = 0;
    off_t size_pos = kernel_pos + (off_t)payload_end - (off_t)sizeof(image_size);

    if (!zboot_pread(fd, &image_size, sizeof(image_size), size_pos)) {
        ERROR_LOG("Failed to read the size of the compressed kernel");
        DEBUG_EXIT(0);
        return NULL;
    }

    if (image_size < sizeof(hdr)) {
        ERROR_LOG("Warning: the EFI zboot payload announces a %u byte kernel, "
                  "which is too small to be one", image_size);
        DEBUG_EXIT(0);
        return NULL;
    }

    uki_zboot_t *zb = calloc(1, sizeof(*zb));

    if (!zb) {
        ERROR_LOG("Failed to allocate the EFI zboot context");
        DEBUG_EXIT(0);
        return NULL;
    }

    if (pthread_mutex_init(&zb->lock, NULL) != 0) {
        ERROR_LOG("Failed to create the EFI zboot lock: %s", strerror(errno));
        free(zb);
        DEBUG_EXIT(0);
        return NULL;
    }

    zb->fd = fd;
    zb->payload_pos = kernel_pos + (off_t)hdr.payload_offset;
    zb->payload_size = hdr.payload_size;
    zb->image_size = image_size;
    strcpy(zb->comp_type, comp_type);

    INFO_LOG("The kernel is an EFI zboot image (%s, %u bytes compressed, "
             "%u bytes once unwrapped)", zb->comp_type, zb->payload_size,
             zb->image_size);

    DEBUG_EXIT(0);
    return zb;
}

/* Milliseconds elapsed since the given point, for the timing messages */
static long elapsed_ms(const struct timespec *start) {
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return -1;
    }

    return (now.tv_sec - start->tv_sec) * 1000 +
           (now.tv_nsec - start->tv_nsec) / 1000000;
}

/*
 * Decompress the payload into a buffer of exactly image_size bytes.
 *
 * The payload is read from the source in chunks rather than mapped or
 * read in one go, so that decompressing a 60 MB kernel needs the room
 * for the kernel and nothing more.
 *
 * Returns the kernel on success, NULL on error.
 */
static char *zboot_inflate(uki_zboot_t *zb) {
    DEBUG_ENTER();

    struct timespec start;

    if (clock_gettime(CLOCK_MONOTONIC, &start) != 0) {
        start.tv_sec = 0;
        start.tv_nsec = 0;
    }

    char *image = malloc(zb->image_size);
    void *in = malloc(UKI_ZBOOT_CHUNK);

    if (!image || !in) {
        ERROR_LOG("Failed to allocate %u bytes for the unwrapped kernel",
                  zb->image_size);
        free(image);
        free(in);
        DEBUG_EXIT(-1);
        return NULL;
    }

    z_stream zs;

    memset(&zs, 0, sizeof(zs));

    /* 16 + MAX_WBITS: the payload is a complete gzip stream */
    int rc = inflateInit2(&zs, 16 + MAX_WBITS);

    if (rc != Z_OK) {
        ERROR_LOG("Failed to initialize the decompressor: %s",
                  zs.msg ? zs.msg : "unknown error");
        free(image);
        free(in);
        DEBUG_EXIT(-1);
        return NULL;
    }

    zs.next_out = (Bytef *)image;
    zs.avail_out = zb->image_size;

    uint32_t read_in = 0;
    bool failed = false;

    while (!failed) {
        if (zs.avail_in == 0) {
            if (read_in >= zb->payload_size) {
                ERROR_LOG("The compressed kernel ends before the end of its "
                          "stream (%u bytes)", zb->payload_size);
                failed = true;
                break;
            }

            uint32_t chunk = zb->payload_size - read_in;

            if (chunk > UKI_ZBOOT_CHUNK) {
                chunk = UKI_ZBOOT_CHUNK;
            }

            if (!zboot_pread(zb->fd, in, chunk,
                            zb->payload_pos + (off_t)read_in)) {
                ERROR_LOG("Failed to read %u bytes of the compressed kernel "
                          "at offset %lld", chunk,
                          (long long)(zb->payload_pos + read_in));
                failed = true;
                break;
            }

            zs.next_in = in;
            zs.avail_in = chunk;
            read_in += chunk;
        }

        rc = inflate(&zs, Z_NO_FLUSH);

        if (rc == Z_STREAM_END) {
            break;
        }

        if (rc != Z_OK) {
            /* Z_BUF_ERROR lands here too: no progress is possible, which
             * means the kernel is larger than the size it announced */
            ERROR_LOG("Failed to decompress the kernel: %s (%d)",
                      zs.msg ? zs.msg : "no progress", rc);
            failed = true;
        }
    }

    size_t produced = zs.total_out;

    inflateEnd(&zs);
    free(in);

    if (!failed && produced != zb->image_size) {
        ERROR_LOG("The unwrapped kernel is %zu bytes, %u were announced",
                  produced, zb->image_size);
        failed = true;
    }

    if (failed) {
        free(image);
        DEBUG_EXIT(-1);
        return NULL;
    }

    bool is_arm64 = memcmp(image + 56, UKI_ARM64_MAGIC,
                           sizeof(UKI_ARM64_MAGIC) - 1) == 0;

    INFO_LOG("Unwrapped the kernel of the EFI zboot image: %u bytes in %ld ms%s",
             zb->image_size, elapsed_ms(&start),
             is_arm64 ? " (an arm64 Image)" : "");

    DEBUG_EXIT(0);
    return image;
}

/*
 * The unwrapped kernel, see ukifs_zboot.h
 */
const char *uki_zboot_image(uki_zboot_t *zb) {
    if (!zb) {
        return NULL;
    }

    pthread_mutex_lock(&zb->lock);

    if (!zb->image && !zb->failed) {
        zb->image = zboot_inflate(zb);
        zb->failed = (zb->image == NULL);
    }

    const char *image = zb->image;

    pthread_mutex_unlock(&zb->lock);

    return image;
}

/* Release the context and the decompressed kernel it holds */
void uki_zboot_free(uki_zboot_t *zb) {
    if (!zb) {
        return;
    }

    pthread_mutex_destroy(&zb->lock);
    free(zb->image);
    free(zb);
}
