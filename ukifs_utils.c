/*
 * uki_utils.c - Utility Functions
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "ukifs_core.h"
#include "ukifs_pe.h"


/* Hash function (DJB2 algorithm) */
static uint32_t hash_string(const char *str) {
    DEBUG_ENTER();

    uint32_t hash = 5381;
    int c;

    while ((c = *str++)) {
        hash = ((hash << 5) + hash) + c; /* hash * 33 + c */
    }

    DEBUG_EXIT(hash);
    return hash;
}


/* Normalize a path (remove leading/trailing slashes, etc.) */
static char *normalize_path(const char *path) {
    DEBUG_ENTER();

    if (!path) {
        return NULL;
    }

    /* Skip leading slashes */
    while (*path == '/') {
        path++;
    }

    /* Return copy */
    char *result = strdup(path);

    /* Remove trailing slashes */
    size_t len = strlen(result);
    while (len > 0 && result[len-1] == '/') {
        result[--len] = '\0';
    }

    DEBUG_STR("normalized", result);
    DEBUG_EXIT(0);
    return result;
}

/* Lookup a path in the hash table */
uki_entry_t *lookup_path(uki_context_t *ctx, const char *path) {
    DEBUG_ENTER();

    if (!ctx || !path) {
        DEBUG_LOG("ctx or path is NULL");
        DEBUG_EXIT(0);
        return NULL;
    }

    /* Normalize the path for lookup */
    char *norm_path = normalize_path(path);
    if (!norm_path) {
        DEBUG_LOG("Failed to normalize path");
        DEBUG_EXIT(0);
        return NULL;
    }
    DEBUG_STR("norm_path", norm_path);

    /* Calculate hash and search chain */
    uint32_t hash = hash_string(norm_path) % ctx->hash_table_size;
    uki_entry_t *entry = ctx->hash_table[hash];

    while (entry) {
        DEBUG_STR("Checking entry", entry->name);
        if (strcmp(entry->name, norm_path) == 0) {
            DEBUG_LOG("Found match!");
            free(norm_path);
            DEBUG_PTR("returning entry", entry);
            DEBUG_EXIT(0);
            return entry;
        }
        entry = entry->next;
    }

    DEBUG_LOG("Path not found in hash table");
    free(norm_path);
    DEBUG_EXIT(0);
    return NULL;
}

/* Add an entry to the hash table */
void add_to_hash_table(uki_context_t *ctx, uki_entry_t *entry) {
    DEBUG_ENTER();
    DEBUG_PTR("ctx", ctx);
    DEBUG_PTR("entry", entry);
    DEBUG_STR("entry->name", entry ? entry->name : NULL);

    if (!ctx || !entry || !entry->name) {
        DEBUG_LOG("ctx, entry, or entry->name is NULL");
        DEBUG_EXIT(0);
        return;
    }

    /* Calculate hash */
    uint32_t hash = hash_string(entry->name) % ctx->hash_table_size;
    DEBUG_VAL("hash index", hash);

    /* Insert at head of chain */
    entry->next = ctx->hash_table[hash];
    ctx->hash_table[hash] = entry;

    DEBUG_LOG("Entry added to hash table");
    DEBUG_EXIT(0);
}

/* Sanitize PE section name (strip leading '.', copy to buffer) */
char *sanitize_section_name(const char *pe_name, size_t len) {
    DEBUG_ENTER();

    if (!pe_name || len == 0) {
        return NULL;
    }

    /* Ensure not longer than PE_SECTION_NAME_SIZE */
    if (len > PE_SECTION_NAME_SIZE) {
        len = PE_SECTION_NAME_SIZE;
    }

    /* Find actual length (may not be null-terminated in PE) */
    size_t actual_len = 0;
    for (size_t i = 0; i < len && pe_name[i] != '\0'; i++) {
        actual_len++;
    }

    /* Skip leading '.' if present */
    const char *start = pe_name;
    size_t copy_len = actual_len;
    if (actual_len > 0 && pe_name[0] == '.') {
        start++;
        copy_len--;
    }

    /* Allocate and copy */
    char *result = malloc(copy_len + 1);
    if (result) {
        memcpy(result, start, copy_len);
        result[copy_len] = '\0';
    }

    DEBUG_STR("sanitized", result);
    DEBUG_EXIT(0);
    return result;
}

#if 0

/* Safe pread that reads full count or fails */
bool full_pread(int fd, void *buf, size_t count, off_t offset) {
    char *p = (char *)buf;
    while (count > 0) {
        ssize_t n = pread(fd, p, count, offset);
        if (n <= 0) {
            return false;
        }
        p += n;
        count -= n;
        offset += n;
    }
    return true;
}

#endif
