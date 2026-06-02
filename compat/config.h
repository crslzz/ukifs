#ifndef CONFIG_H
#define CONFIG_H

#define HAVE_OPENSSL 1

#ifdef __QNXNTO__

/* Include compat/sys/endian.h */
#define HAVE_SYS_ENDIAN_H 1

/* QNX 7.1's OpenSSL does not implement EVP_MD_get0_name() */
const char *EVP_MD_get0_name(const void *vmd);

#else /* __QNXNTO__ */

/* Include host /usr/include/endian.h */
#define HAVE_ENDIAN_H 1

#endif /* __QNXNTO__ */

#endif /* CONFIG_H */
