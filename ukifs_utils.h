#ifndef UKI_UTILS_H
#define UKI_UTILS_H

#include <stddef.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

#define UNUSED __attribute__((unused))
#define PACKED __attribute__((packed))

#define container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))

/* Debug logging levels */
#define LOG_QUIET   0  /* No debug output */
#define LOG_ERROR   1  /* Errors only */
#define LOG_INFO    2  /* Errors + info messages */
#define LOG_DEBUG   3  /* Full debug traces */

/* Global log level (set by command-line argument) */
extern int g_log_level;


/* Debug logging macros */
#define DEBUG_LOG(fmt, ...) \
    do { if (g_log_level >= LOG_DEBUG) \
        fprintf(stderr, "[DEBUG %s:%d] " fmt "\n", __func__, __LINE__, ##__VA_ARGS__); \
    } while (0)
#define DEBUG_ENTER() \
    do { if (g_log_level >= LOG_DEBUG) \
        fprintf(stderr, "[ENTER %s:%d]\n", __func__, __LINE__); \
    } while (0)
#define DEBUG_EXIT(ret) \
    do { if (g_log_level >= LOG_DEBUG) \
        fprintf(stderr, "[EXIT  %s:%d] returning %d\n", __func__, __LINE__, (int)(ret)); \
    } while (0)
#define DEBUG_PTR(name, ptr) \
    do { if (g_log_level >= LOG_DEBUG) \
        fprintf(stderr, "[PTR   %s:%d] %s = %p\n", __func__, __LINE__, name, (void*)(ptr)); \
    } while (0)
#define DEBUG_VAL(name, val) \
    do { if (g_log_level >= LOG_DEBUG) \
        fprintf(stderr, "[VAL   %s:%d] %s = %ld\n", __func__, __LINE__, name, (long)(val)); \
    } while (0)
#define DEBUG_VALX(name, val) \
    do { if (g_log_level >= LOG_DEBUG) \
        fprintf(stderr, "[VALX  %s:%d] %s = %lx\n", __func__, __LINE__, name, (long)(val)); \
    } while (0)
#define DEBUG_STR(name, str) \
    do { if (g_log_level >= LOG_DEBUG) \
        fprintf(stderr, "[STR   %s:%d] %s = '%s'\n", __func__, __LINE__, name, (str) ? (str) : "(null)"); \
    } while (0)

#define INFO_LOG(fmt, ...) \
    do { if (g_log_level >= LOG_INFO) \
        fprintf(stderr, "[INFO] " fmt "\n", ##__VA_ARGS__); \
    } while (0)
#define ERROR_LOG(fmt, ...) \
    do { if (g_log_level >= LOG_ERROR) \
        fprintf(stderr, "[ERROR] " fmt "\n", ##__VA_ARGS__); \
    } while (0)


struct uki_context;
struct uki_entry;

struct uki_entry *lookup_path(struct uki_context *ctx, const char *path);
void add_to_hash_table(struct uki_context *ctx, struct uki_entry *entry);
char *sanitize_section_name(const char *pe_name, size_t len);
//bool full_pread(int fd, void *buf, size_t count, off_t offset);

#endif /* UKI_UTILS_H */
