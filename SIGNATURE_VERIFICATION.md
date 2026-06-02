# UKI Signature Verification with Padding Support

## Overview

The QNX UKI filesystem driver (`ukifs`) now includes intelligent signature verification that automatically handles padded UKI files. When a UKI file has been padded to a fixed size after signing, the driver automatically detects the padding and verifies only the actual UKI data.

## Feature Summary

### What's New

**Automatic Padding Detection:**
- Computes actual UKI size from PE headers (including certificate table)
- Detects if file has extra padding beyond the UKI data
- Creates temporary file with only UKI data for verification
- Verifies signature against the truncated file (excluding padding)

**Transparent Operation:**
- No command-line options required
- Works automatically when `-c <cert.pem>` is specified
- Falls back to full-file verification if no padding detected
- Informative logging at INFO level shows what's happening

### Why This Matters

UKI files are often padded to fixed sizes for:
- **Partition alignment** - Align to 128 MB, 256 MB boundaries
- **Pre-allocation** - Reserve space for future updates
- **Storage devices** - Block device alignment requirements
- **Build processes** - Round to power-of-2 sizes

When padding is added **after** signing, the signature only covers the original UKI data, not the padding. Without this feature, signature verification would fail on such files.

### Example: Padded UKI

File: `ukiboot_a_signed.img`

```
Actual file size:  134217728 bytes (128.00 MB)
Computed UKI size:  66324224 bytes (63.25 MB)
Extra padding:      67893504 bytes (64.75 MB, 50.6%)
```

Driver behavior:
```
[INFO] Verifying UKI signature with certificate: rootCA.crt
[INFO] File size: 134217728 bytes, UKI size: 66324224 bytes
[INFO] Detected 67893504 bytes of padding (50.6% of file)
[INFO] Creating temporary file for signature verification (excluding padding)
[INFO] Verifying signature against UKI data only (66324224 bytes)
[VERBOSE] Signature digest algorithm: SHA256
[VERBOSE] Authenticode hash matches
[VERBOSE] Signer certificate trusted
[INFO] ✓ UKI signature verified successfully
```

## Usage

### Basic Usage (Signed UKI)

```bash
# Mount signed UKI with certificate verification
./ukifs -c rootCA.crt ukiboot_a_signed.img /mnt/uki

# With verbose logging
./ukifs -v -c rootCA.crt ukiboot_a_signed.img /mnt/uki

# With debug logging (shows full verification details)
./ukifs -d 3 -c rootCA.crt ukiboot_a_signed.img /mnt/uki
```

### Behavior by File Type

**Unsigned UKI (no certificate specified):**
```bash
./ukifs ukiboot_a.img /mnt/uki
# Mounts without verification (warning logged)
```

**Signed UKI without padding:**
```bash
./ukifs -c cert.pem signed.efi /mnt/uki
# Verifies full file, no temporary file needed
```

**Signed UKI with padding:**
```bash
./ukifs -c cert.pem padded.efi /mnt/uki
# Automatically detects padding
# Creates temp file with UKI data only
# Verifies signature against truncated file
```

**Invalid signature:**
```bash
./ukifs -c wrong-cert.pem signed.efi /mnt/uki
# ERROR: UKI signature verification failed!
# Mount fails, driver exits
```

### Log Levels

Control verbosity with `-d <level>`:

**Level 0 (Quiet):** Errors only
```
ERROR: UKI signature verification failed!
```

**Level 1 (Errors):** Same as quiet
```
[ERROR] PE file has no embedded signature
```

**Level 2 (Info, `-v`):** Verification status
```
[INFO] File size: 134217728 bytes, UKI size: 66324224 bytes
[INFO] Detected 67893504 bytes of padding (50.6% of file)
[INFO] ✓ UKI signature verified successfully
```

**Level 3 (Debug, `-d 3`):** Full details
```
[VERBOSE] Signature digest algorithm: SHA256
[VERBOSE] Authenticode hash matches
[VERBOSE] Signer certificate trusted
```

## Test Programs

The signature verification logic is also used in test programs:

### test_verify

Linux test program with same functionality:

```bash
# Standard mode (verify full file)
./test_verify ukiboot_a_signed.img rootCA.crt

# Ignore padding mode (same as driver behavior)
./test_verify -i ukiboot_a_signed.img rootCA.crt
```

Both modes should pass for properly padded signed UKIs.

### test_size

Analyze UKI structure to see padding:

```bash
./test_size ukiboot_a_signed.img
```

Output shows:
- Expected UKI size (computed from PE headers)
- Actual file size
- Extra padding amount and percentage
- Certificate table location (if signed)

## Common Scenarios

### Scenario 1: Pre-Allocated Partition

**Setup:**
- UKI file: 63 MB
- Partition: 128 MB (power-of-2 for alignment)
- Solution: Pad UKI to 128 MB after signing

**Driver behavior:**
```
[INFO] Detected 65 MB of padding (50.8% of file)
[INFO] Verifying signature against UKI data only (66324224 bytes)
[INFO] ✓ UKI signature verified successfully
```

**Result:** Mount succeeds, signature valid ✓

### Scenario 2: Build Artifact

**Setup:**
- Build process generates 65 MB UKI
- Build system rounds to 128 MB for storage
- UKI was signed before padding added

**Without this feature:**
```
ERROR: UKI signature verification failed!
```

**With this feature:**
```
[INFO] Detected 63 MB of padding (49.2% of file)
[INFO] ✓ UKI signature verified successfully
```

**Result:** Mount succeeds ✓

### Scenario 3: No Padding

**Setup:**
- UKI file: 63 MB
- File size: 63 MB (no padding)

**Driver behavior:**
```
[INFO] File size: 66324224 bytes, UKI size: 66324224 bytes
[INFO] ✓ UKI signature verified successfully
```

**Result:** No temporary file created, direct verification ✓

## Security Considerations

### What This Feature Does

✓ **Validates signature** on actual UKI content (headers + sections + certificate)
✓ **Prevents tampering** with UKI data
✓ **Verifies certificate** against trusted root CA
✓ **Handles padding** transparently without compromising security

### What This Feature Doesn't Do

✗ **Does NOT validate padding** - Padding is not signed, can be modified
✗ **Does NOT verify file size** - Only content up to certificate table is checked
✗ **Does NOT prevent padding removal** - Attacker could remove padding (but signature still valid)

### Security Properties

**Padding is untrusted:**
- Padding bytes are NOT covered by signature
- Any data after certificate table is ignored
- Modifying padding does NOT invalidate signature

**UKI content is trusted:**
- All PE headers are verified
- All sections are verified
- Certificate table is verified
- Any modification to UKI data breaks signature

**Best practices:**
1. Sign UKI files before adding padding
2. Use padding only for alignment/pre-allocation
3. Don't store sensitive data in padding region
4. Verify certificate against trusted root CA

## Troubleshooting

### Signature Verification Fails

**Problem:** Mount fails with signature verification error

**Diagnosis:**
```bash
# Check if UKI is signed
./test_size ukiboot_a_signed.img | grep Certificate

# Test verification manually
./test_verify ukiboot_a_signed.img rootCA.crt

# Try with padding ignored
./test_verify -i ukiboot_a_signed.img rootCA.crt
```

**Solutions:**
- Ensure certificate matches signer
- Check if UKI was modified after signing
- Verify padding was added after signing (not before)

### Mount Hangs or Fails

**Problem:** Driver doesn't exit cleanly

**Check temporary files:**
```bash
ls -la /tmp/ukifs_verify_*
```

**If files remain:**
- Temporary file cleanup failed
- Driver was killed during verification
- Clean up manually: `rm /tmp/ukifs_verify_*`

**Prevention:**
- Temporary files are auto-deleted (unlink after creation)
- Cleanup happens even on error paths
