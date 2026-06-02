/*
 * Compatibility wrapper for ukiboot's PE parser
 */

#ifndef UKI_ENDIAN_H
#define UKI_ENDIAN_H

#ifdef __QNXNTO__

/* Endian conversion (PE is always little-endian) */
#include <gulliver.h>

#define le16toh(x) ENDIAN_LE16(x)
#define le32toh(x) ENDIAN_LE32(x)
#define le64toh(x) ENDIAN_LE64(x)
#define htole16(x) ENDIAN_LE16(x)
#define htole32(x) ENDIAN_LE32(x)
#define htole64(x) ENDIAN_LE64(x)

#endif /* __QNXNTO__ */

#endif /* UKI_ENDIAN_H */
