# UKI Signature Verification with Padding Support

## Overview

The QNX UKI filesystem driver (`ukifs`) verifies the Authenticode signature of a UKI before exposing its sections. Verification works whether the UKI is a plain file, a file padded to a fixed size, or the content of a `ukiboot_a` / `ukiboot_b` slot of a ukiboot disk image.

## Feature Summary

### How It Works

Verification is done by ukiboot's PE parser (`pe_verify_signature()`), which is given the offset of the UKI in the file. The Authenticode digest it computes covers:

- the PE headers (minus the `CheckSum` field and the Security Directory entry),
- the raw data of every section,
- the gap between the last section and the certificate table.

Nothing beyond the certificate table is part of the digest, so **any padding after the UKI is ignored**, and the UKI does not have to start at offset 0 of the file.

**Transparent operation:**
- No command-line options required
- Works automatically when `-c <cert.pem>` is specified
- Applies identically to a plain UKI file and to a ukiboot slot
- Informative logging at INFO level shows what's happening

Before verifying, the driver computes the size of the UKI from the PE headers and compares it with the space available (file size, or size of the slot partition). A UKI that does not fit is rejected as truncated, and left-over space is reported as padding.

### Why This Matters

UKI files are often padded to fixed sizes for:
- **Partition alignment** - Align to 128 MB, 256 MB boundaries
- **Pre-allocation** - Reserve space for future updates
- **Storage devices** - Block device alignment requirements
- **Build processes** - Round to power-of-2 sizes

A ukiboot slot partition is a case of this: the UKI is written at the start of a fixed-size partition, and the rest of the partition keeps whatever it contained before. When padding is added **after** signing, the signature only covers the original UKI data, not the padding.

### Example: Padded UKI

File: `ukiboot_a_signed.img`

```
Actual file size:  134217728 bytes (128.00 MB)
Computed UKI size:  66324224 bytes (63.25 MB)
Extra padding:      67893504 bytes (64.75 MB, 50.6%)
```

Driver behavior:
```
[INFO] UKI size: 66324224 bytes (134217728 bytes available)
[INFO] Ignoring 67893504 bytes of padding after the UKI (50.6%)
[INFO] Verifying UKI signature with certificate: rootCA.crt
[VERBOSE] Signature digest algorithm: SHA256
[VERBOSE] Authenticode hash matches
[VERBOSE] Signer certificate trusted
[INFO] UKI signature verified successfully
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

# Verify the UKI of the active slot of a ukiboot disk image
./ukifs -v -c rootCA.crt --slot auto /dev/hd0 /mnt/uki
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
# Verifies headers, sections and certificate table
```

**Signed UKI with padding:**
```bash
./ukifs -c cert.pem padded.efi /mnt/uki
# Reports the padding, which is not part of the digest
```

**Signed UKI in a ukiboot slot:**
```bash
./ukifs -c cert.pem --slot b ukiboot.img /mnt/uki
# Verifies the UKI where it sits in the slot partition
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
[INFO] UKI size: 66324224 bytes (134217728 bytes available)
[INFO] Ignoring 67893504 bytes of padding after the UKI (50.6%)
[INFO] UKI signature verified successfully
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

# Ignore padding mode (verify a copy stripped of its padding)
./test_verify -i ukiboot_a_signed.img rootCA.crt
```

Both modes should pass for properly padded signed UKIs.

### test_slot

Verify a UKI where it sits in a slot of a ukiboot disk image:

```bash
./test_slot --cert rootCA.crt ukiboot.img auto
```

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
[INFO] UKI size: 66324224 bytes (134217728 bytes available)
[INFO] Ignoring 67893504 bytes of padding after the UKI (50.6%)
[INFO] UKI signature verified successfully
```

**Result:** Mount succeeds, signature valid ✓

### Scenario 2: ukiboot Slot

**Setup:**
- UKI signed, then written at the start of the `ukiboot_b` partition
- Partition is larger than the UKI and keeps older data after it

**Driver behavior:**
```
[INFO] Selected ukiboot slot: b
[INFO] ukiboot slot b: offset 2097152, size 1048576 bytes
[INFO] UKI size: 5144 bytes (1043432 bytes available)
[INFO] UKI signature verified successfully
```

**Result:** Mount succeeds ✓

### Scenario 3: No Padding

**Setup:**
- UKI file: 63 MB
- File size: 63 MB (no padding)

**Driver behavior:**
```
[INFO] UKI size: 66324224 bytes (66324224 bytes available)
[INFO] UKI signature verified successfully
```

**Result:** Direct verification ✓

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
✗ **Does NOT authenticate the slot selection** - The `ukibootctl` content is CRC32 protected, not signed

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
5. Verify the signature of whichever slot is mounted, including with `--slot auto`

## Troubleshooting

### Signature Verification Fails

**Problem:** Mount fails with signature verification error

**Diagnosis:**
```bash
# Check if UKI is signed
./test_size ukiboot_a_signed.img | grep Certificate

# Test verification manually
./test_verify ukiboot_a_signed.img rootCA.crt

# Test verification of a UKI stored in a slot
./test_slot --cert rootCA.crt ukiboot.img auto
```

**Solutions:**
- Ensure certificate matches signer
- Check if UKI was modified after signing
- Check that the expected slot is being mounted (`-v` shows the selected slot)

### Truncated UKI

**Problem:** Mount fails with `Truncated UKI: N bytes needed, M bytes available`

**Cause:** The UKI does not fit in the space available for it, typically a slot
partition smaller than the UKI that was written to it, or an incomplete write.

**Diagnosis:**
```bash
./test_slot ukiboot.img a   # compare "UKI size" with "Slot size"
```
