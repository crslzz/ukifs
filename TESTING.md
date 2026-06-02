# UKI Testing Suite

This directory contains test programs for UKI (Unified Kernel Image) analysis and verification.

## Test Programs

### 1. test_size - UKI Size Analysis

Computes the actual UKI size from PE/COFF headers, identifying extra padding.

```bash
./test_size <uki-file>
```

**Features:**
- Analyzes PE structure (DOS header, NT headers, sections)
- Detects certificate table (Authenticode signatures)
- Computes minimum file size (headers + sections + certificate)
- Identifies extra padding beyond UKI data
- Works with both signed and unsigned UKIs

**Example:**
```bash
$ ./test_size ukiboot_signed.img
Expected UKI size: 66324224 bytes (63.25 MB)
Actual file size:  134217728 bytes (128.00 MB)
Extra padding:     67893504 bytes (64.75 MB)
```

### 2. test_verify - Signature Verification

Verifies UKI Authenticode signatures using X.509 certificates.

```bash
./test_verify [options] <uki-file> <cert.pem>
```

**Options:**
- `-i` - Ignore padding (verify only actual UKI data)
- `-h` - Show help

**Features:**
- PKCS#7/Authenticode signature verification
- X.509 certificate chain validation
- Optional padding removal for padded UKI files
- Detailed verbose output (digest algorithm, hash verification)

**Example (Standard Mode):**
```bash
$ ./test_verify ukiboot_signed.img rootCA.crt
✓ SIGNATURE VERIFICATION PASSED
```

**Example (Ignore Padding):**
```bash
$ ./test_verify -i ukiboot_signed.img rootCA.crt
Computed UKI size: 66324224 bytes (63.25 MB)
Extra padding: 67893504 bytes (64.75 MB, 50.6% of file)
✓ SIGNATURE VERIFICATION PASSED
```

## Building

Build all test programs:

```bash
UKIBOOT=/path/to/ukiboot QNX_SDP=/path/to/qnx/sdp make tests
```

Build individually:

```bash
UKIBOOT=/path/to/ukiboot QNX_SDP=/path/to/qnx/sdp make test-size    # UKI size analysis
UKIBOOT=/path/to/ukiboot QNX_SDP=/path/to/qnx/sdp make test-verify  # Signature verification
```

## Running Tests

### Quick Test
```bash
# Size analysis
./test_size ukiboot_signed.img

# Signature verification (both modes)
./test_verify ukiboot_signed.img rootCA.crt
./test_verify -i ukiboot_signed.img rootCA.crt
```

### Demo Scripts
```bash
./test_size_demo.sh         # Demonstrates size analysis
./test_verify_demo.sh       # Demonstrates signature verification
./test_comprehensive.sh     # Runs all tests
```

### Comprehensive Test Suite
```bash
./test_comprehensive.sh
```

**Tests performed:**
1. Size analysis on unsigned UKI
2. Size analysis on signed UKI
3. Signature verification (standard mode)
4. Signature verification (ignore padding mode)
5. Size computation accuracy validation

**Expected output:**
```
All Tests Passed!

Summary:
  - test_size correctly computes UKI size including certificate table
  - test_verify standard mode works with padded files
  - test_verify -i mode correctly handles padding
  - Both modes successfully verify signed UKI with rootCA.crt
```

## Use Cases

### 1. Detecting Padding in UKI Files

```bash
./test_size production.efi
```

Identifies if UKI has been padded to a fixed size (common for partition alignment or pre-allocation).

### 2. Verifying Signed UKIs

```bash
./test_verify signed.efi signing-cert.pem
```

Validates Authenticode signature and certificate chain.

### 3. Verifying Padded Signed UKIs

When a UKI is signed, then padded to a fixed size:

```bash
# Standard mode: verifies full file (may work if padding is after signature)
./test_verify padded.efi cert.pem

# Ignore padding mode: verifies only UKI data (recommended)
./test_verify -i padded.efi cert.pem
```

The `-i` option is crucial when:
- UKI was signed before padding was added
- File was padded to 128 MB, 256 MB, or other power-of-2 sizes
- Signature verification fails in standard mode due to padding

### 4. Integration Testing

```bash
# Verify UKI size matches expected value
COMPUTED=$(./test_size uki.efi | grep "Expected UKI" | awk '{print $4}')
if [ "$COMPUTED" != "66324224" ]; then
    echo "ERROR: Size mismatch"
    exit 1
fi

# Verify signature
if ! ./test_verify -i uki.efi cert.pem; then
    echo "ERROR: Signature invalid"
    exit 1
fi
```

## Technical Details

### Certificate Table Location

In PE files, the certificate table (Authenticode signature) is:
- Stored in the Security Data Directory (Optional Header, index 4)
- Located at a file offset (not RVA like other data directories)
- Appended after all sections
- Offset: PE32+ = 144 bytes, PE32 = 128 bytes from optional header start

### Size Computation Algorithm

```
expected_size = max(
    headers_end,           // End of section table
    last_section_end,      // Highest section offset + size
    cert_table_end         // Certificate offset + size (if present)
)

extra_padding = actual_file_size - expected_size
```

### Signature Verification with Padding

The `-i` option works by:
1. Computing actual UKI size using `uki_compute_size()`
2. Creating temporary file with only UKI data (no padding)
3. Verifying signature against temporary file
4. Cleaning up temporary file

This is necessary because Authenticode signatures are computed over:
- File content excluding checksum field
- File content excluding certificate table pointers
- File content up to the certificate table

If padding is added after signing, it's not included in the signature hash.

## Files

- `test_size.c` - Size analysis program
- `test_verify.c` - Signature verification program
- `uki_size.c` / `uki_size.h` - Common size computation library
- `config_linux.h` - Linux build configuration
- `test_size_demo.sh` - Size analysis demo
- `test_verify_demo.sh` - Signature verification demo
- `test_comprehensive.sh` - Complete test suite
- `TEST_SIZE_README.md` - Detailed size analysis documentation
- `TEST_VERIFY_README.md` - Detailed verification documentation
- `TESTING.md` - This file

## Dependencies

- GCC or Clang
- OpenSSL development libraries (`libssl-dev` or `openssl-devel`)
- Linux x86_64 system

Install on Fedora/RHEL:
```bash
sudo dnf install openssl-devel gcc
```

Install on Debian/Ubuntu:
```bash
sudo apt install libssl-dev gcc
```

## See Also

- **ukifs** - QNX UKI filesystem driver (uses same PE parsing logic)
- **PE/COFF Specification** - Microsoft Portable Executable format
- **Authenticode Specification** - Microsoft code signing format
