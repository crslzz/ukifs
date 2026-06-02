/*
 * Compatibility wrapper for QNX 7.1's OpenSSL
 */

#include <openssl/evp.h>

const char *EVP_MD_get0_name(const EVP_MD* md) {
    if (!md) {
	return "unknown";
    }
  
    /* Get the numeric identifier (NID) for this digest type */
    int nid = EVP_MD_type(md);

    /* Convert NID to short name string */
    const char *name = OBJ_nid2sn(nid);
    return name ? name : "unknown";
}
