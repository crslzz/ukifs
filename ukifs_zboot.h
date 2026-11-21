/*
 * EFI zboot kernel images
 *
 * Unwraps the Linux kernel of a UKI whose kernel section is an EFI
 * zboot image, so that a loader which is not EFI can boot it.
 */

#ifndef UKIFS_ZBOOT_H
#define UKIFS_ZBOOT_H

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <sys/types.h>

/* Section of a UKI holding the kernel, once its leading '.' is stripped */
#define UKI_KERNEL_NAME "linux"

/* File exposing the kernel unwrapped from an EFI zboot image */
#define UKI_ZBOOT_NAME "vmlinux"

/* Length of the compression type of a zboot header */
#define UKI_ZBOOT_COMP_SIZE 32

/*
 * An EFI zboot kernel section, and the kernel unwrapped from it.
 *
 * The kernel is decompressed on the first read rather than while
 * mounting: it is tens of megabytes, and a client that only reads
 * cmdline or uname should not pay for it.
 */
typedef struct uki_zboot {
    int      fd;                  /* Source holding the compressed payload */
    off_t    payload_pos;         /* Position of the payload in that source */
    uint32_t payload_size;        /* Size of the compressed payload */
    uint32_t image_size;          /* Size of the kernel once decompressed */
    char     comp_type[UKI_ZBOOT_COMP_SIZE + 1];  /* "gzip", "zstd", ... */

    pthread_mutex_t lock;         /* Serializes the one-shot decompression */
    char    *image;               /* Decompressed kernel, NULL until read */
    bool     failed;              /* Decompression failed, do not retry */
} uki_zboot_t;

/*
 * Examine the kernel section of a UKI.
 *
 * Returns an allocated context when the section is an EFI zboot image
 * that can be unwrapped, NULL otherwise: a kernel that is already a
 * bare Image needs nothing done to it, and one compressed in a way
 * this build cannot decompress cannot be unwrapped at all.
 *
 * kernel_pos is the position of the section in the file, not in the UKI.
 */
uki_zboot_t *uki_zboot_probe(int fd, off_t kernel_pos, uint32_t kernel_size);

/*
 * The unwrapped kernel, decompressed on the first call.
 *
 * Returns NULL if the kernel cannot be decompressed; the failure is
 * remembered, so a client retrying a read does not pay for it again.
 */
const char *uki_zboot_image(uki_zboot_t *zb);

/* Release the context and the decompressed kernel it holds */
void uki_zboot_free(uki_zboot_t *zb);

#endif /* UKIFS_ZBOOT_H */
