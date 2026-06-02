/*
 * PE/COFF Parsing
 */

#ifndef UKIFS_PARSE_H
#define UKIFS_PARSE_H

#include <stddef.h>
#include <stdbool.h>
#include <sys/types.h>

struct uki_context;

int uki_parse_sections(struct uki_context *ctx);

#endif /* UKIFS_PARSE_H */
