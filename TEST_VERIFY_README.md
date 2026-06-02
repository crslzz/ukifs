# UKI Signature Verification Test Program

## Overview

`test_verify` is a standalone Linux test program that validates the UKI (Unified Kernel Image) signature verification functionality used in the QNX UKI filesystem driver.

## Purpose

This program tests the `pe_verify_signature()` function from `pe_parser_posix.c` to ensure:
- PE/COFF signature verification works correctly
- X.509 certificate validation is functional
- OpenSSL integration is working properly
- Authenticode/PKCS7 signature parsing is correct

## Building

Build the test program natively for Linux:

```bash
UKIBOOT=/path/to/ukiboot QNX_SDP=/path/to/qnx/sdp make test-verify
```

This creates a `test_verify` executable (66KB, x86-64 Linux binary).

## Usage

```bash
./test_verify [options] <uki-file> <cert.pem>
```

### Options

- `-i` - Ignore padding (verify only actual UKI, not extra padding)
- `-h` - Show help message

### Arguments

- `<uki-file>` - Path to UKI file (PE executable, typically `.efi` or `.img`)
- `<cert.pem>` - Path to X.509 certificate in PEM format

### Examples

```bash
# Test with a signed UKI and matching certificate
./test_verify /boot/archlinux.efi /etc/uki-cert.pem

# Test with padded UKI, ignoring padding
./test_verify -i ukiboot_a_signed.img rootCA.crt

# Test with test files
./test_verify ../ukiboot_a.img test-cert.pem

# Run automated demo
./test_verify_demo.sh
```

### The -i Option (Ignore Padding)

UKI files are sometimes padded to a fixed size (e.g., 128 MB) for storage allocation or partition alignment. When this padding is added **after** signing, the signature was computed over the original UKI data, not including the padding.

The `-i` option:
1. Computes the actual UKI size from PE headers (including certificate table)
2. Creates a temporary file with only the UKI data (excluding padding)
3. Verifies the signature against this truncated file

This is useful when:
- UKI was signed, then padded to a fixed size
- File system added block alignment padding
- Build process appended extra data after signing

Example with `ukiboot_a_signed.img`:
- Actual file size: 134217728 bytes (128.00 MB)
- Computed UKI size: 66324224 bytes (63.25 MB)
- Extra padding: 67893504 bytes (64.75 MB, 50.6% of file)

```bash
# Without -i: verifies full file including padding
./test_verify ukiboot_a_signed.img rootCA.crt

# With -i: verifies only actual UKI (66324224 bytes)
./test_verify -i ukiboot_a_signed.img rootCA.crt
```

Both should succeed if the signature was computed before padding was added.

## Exit Codes

- `0` - Signature verification **passed** (UKI is properly signed and valid)
- `1` - Signature verification **failed** (UKI is unsigned, improperly signed, or certificate mismatch)

## Expected Behavior

### When Verification Succeeds

```
========================================
UKI Signature Verification Test
========================================
UKI file:     signed.efi
Certificate:  signing-cert.pem
========================================

Starting signature verification...

[VERBOSE] Authenticode signature found
[VERBOSE] Signature digest algorithm: sha256
[VERBOSE] Signature verification successful
[VERBOSE] Certificate matches

========================================
✓ SIGNATURE VERIFICATION PASSED
========================================

The UKI file is properly signed and the signature is valid.
```

### When Verification Fails

```
========================================
UKI Signature Verification Test
========================================
UKI file:     unsigned.efi
Certificate:  test-cert.pem
========================================

Starting signature verification...

[ERROR] PE file has no embedded signature

========================================
✗ SIGNATURE VERIFICATION FAILED
========================================

The UKI file is either not signed, improperly signed,
or the signature does not match the provided certificate.
```

## Common Failure Scenarios

1. **No embedded signature**
   - The UKI file is not signed at all
   - Most test UKI files are unsigned

2. **Certificate mismatch**
   - UKI is signed, but not with the provided certificate
   - Different signing key was used

3. **Invalid signature**
   - Signature is present but corrupted
   - File was modified after signing

4. **Unsupported signature format**
   - Not using Authenticode/PKCS7 format
   - Wrong certificate revision

## Demo Script

Run the automated demo to see the test program in action:

```bash
./test_verify_demo.sh
```

This script:
- Creates a temporary self-signed certificate
- Tests verification against `ukiboot_a.img`
- Demonstrates expected failure (unsigned UKI)
- Cleans up temporary files

## Integration with QNX Driver

The same `pe_verify_signature()` function is used in the QNX UKI filesystem driver (`ukifs`):

```bash
# QNX driver usage (on QNX system)
./ukifs -c /path/to/cert.pem /boot/uki.efi /mnt/uki
```

If signature verification fails, the driver refuses to mount the UKI.

## Technical Details

### OpenSSL Version Compatibility

- Linux build: Uses system OpenSSL (typically 3.x)
- QNX build: Uses OpenSSL 1.1.x (via compatibility macros in config.h)

### Signature Format

The program expects Authenticode signatures embedded in PE executables:
- WIN_CERT_REVISION_2_0 format
- WIN_CERT_TYPE_PKCS7 signature type
- X.509 certificate chain
- PKCS7 signed data structure

### Verbose Output

The program runs with `opt_verbose = true` by default, showing:
- Digest algorithm used (SHA-256, SHA-384, etc.)
- Signature validation steps
- Certificate verification results

## Creating Test Certificates

To create a self-signed certificate for testing:

```bash
# Generate private key
openssl genrsa -out test-key.pem 2048

# Create self-signed certificate
openssl req -new -x509 -key test-key.pem -out test-cert.pem \
    -days 365 -subj "/CN=Test UKI Signing"
```

Note: Self-signed certificates are fine for testing, but production UKIs should use proper PKI certificates.

## Files

- `test_verify.c` - Main test program (~240 lines)
- `uki_size.c` / `uki_size.h` - Common UKI size computation library
- `config_linux.h` - Linux build configuration
- `test_verify_demo.sh` - Automated test demonstration
- `TEST_VERIFY_README.md` - This file

## Dependencies

- OpenSSL development libraries (`libssl-dev`)
- GCC or Clang compiler
- Linux x86_64 system

Install on Fedora/RHEL:
```bash
sudo dnf install openssl-devel gcc
```

Install on Debian/Ubuntu:
```bash
sudo apt install libssl-dev gcc
```

## Troubleshooting

### Build Errors

**Error:** `fatal error: openssl/evp.h: No such file or directory`

**Solution:** Install OpenSSL development headers:
```bash
sudo dnf install openssl-devel  # Fedora/RHEL
sudo apt install libssl-dev     # Debian/Ubuntu
```

### Runtime Errors

**Error:** `error while loading shared libraries: libssl.so.3`

**Solution:** Ensure OpenSSL runtime is installed:
```bash
sudo dnf install openssl-libs   # Fedora/RHEL
sudo apt install libssl3        # Debian/Ubuntu
```

## License

Same license as the parent ukiboot project.
