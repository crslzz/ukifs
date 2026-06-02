#!/bin/bash
#
# test_ukifs_signature.sh - Demonstrate ukifs signature verification with padding
#
# This script shows how ukifs handles padded UKI files during signature verification.
# Since we can't run the QNX binary on Linux, this demonstrates the equivalent
# using test_verify with the same logic.
#

set -e

echo "=========================================="
echo "ukifs Signature Verification Demo"
echo "=========================================="
echo ""
echo "This demonstrates how ukifs handles UKI signature verification"
echo "for padded files using the same logic as test_verify -i"
echo ""

SIGNED_UKI="ukiboot_a_signed.img"
CERT="rootCA.crt"

if [ ! -f "$SIGNED_UKI" ]; then
    echo "ERROR: $SIGNED_UKI not found"
    exit 1
fi

if [ ! -f "$CERT" ]; then
    echo "ERROR: $CERT not found"
    exit 1
fi

echo "Step 1: Analyze UKI structure"
echo "=========================================="
./test_size "$SIGNED_UKI" | grep -E "Actual file size:|Expected UKI size:|Certificate table:|Extra padding:"
echo ""

echo "Step 2: Verify signature (ukifs behavior simulation)"
echo "=========================================="
echo "The ukifs driver performs these steps automatically:"
echo "  1. Detect padding"
echo "  2. Create temporary file with UKI data only"
echo "  3. Verify signature against truncated file"
echo "  4. Clean up temporary file"
echo ""

echo "Running test_verify -i (simulates ukifs behavior)..."
echo ""
./test_verify -i "$SIGNED_UKI" "$CERT" 2>&1 | grep -E "Mode:|File size:|Computed UKI|Certificate table:|Extra padding:|Copying UKI|PASSED|FAILED"

echo ""
echo "=========================================="
echo "ukifs Command Equivalent"
echo "=========================================="
echo ""
echo "On QNX, you would mount with:"
echo "  ./ukifs -c rootCA.crt ukiboot_a_signed.img /mnt/uki"
echo ""
echo "The driver would automatically:"
echo "  - Detect 67893504 bytes of padding (50.6%)"
echo "  - Verify signature on 66324224 bytes (actual UKI)"
echo "  - Mount successfully if signature is valid"
echo ""
echo "Log output (with -v flag):"
echo "  [INFO] File size: 134217728 bytes, UKI size: 66324224 bytes"
echo "  [INFO] Detected 67893504 bytes of padding (50.6% of file)"
echo "  [INFO] Creating temporary file for signature verification"
echo "  [INFO] Verifying signature against UKI data only"
echo "  [INFO] ✓ UKI signature verified successfully"
echo ""

