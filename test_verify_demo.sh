#!/bin/bash
#
# test_verify_demo.sh - Demonstrate UKI signature verification test program
#

set -e

echo "=========================================="
echo "UKI Signature Verification Test Demo"
echo "=========================================="
echo ""

# Check if test_verify exists
if [ ! -x ./test_verify ]; then
    echo "ERROR: test_verify not found or not executable"
    echo "Build it first with: make test-verify"
    exit 1
fi

# Check if ukiboot_a.img exists
UKI_FILE="../ukiboot_a.img"
if [ ! -f "$UKI_FILE" ]; then
    echo "ERROR: $UKI_FILE not found"
    exit 1
fi

echo "Test 1: Attempt verification without a certificate"
echo "Expected: Should fail (no certificate provided)"
echo "----------------------------------------"
echo ""

# Create a dummy certificate for testing (self-signed)
echo "Creating a temporary self-signed certificate for testing..."
TEMP_CERT=$(mktemp --suffix=.pem)
TEMP_KEY=$(mktemp --suffix=.key)

# Generate a self-signed certificate
openssl req -x509 -newkey rsa:2048 -keyout "$TEMP_KEY" -out "$TEMP_CERT" \
    -days 1 -nodes -subj "/CN=Test UKI Certificate" 2>/dev/null

echo "Generated temporary certificate: $TEMP_CERT"
echo ""

echo "Test 2: Attempt verification with test certificate"
echo "Expected: Should fail (UKI not signed with this cert)"
echo "----------------------------------------"
echo ""

# Run the test (expected to fail since UKI is not signed with our test cert)
if ./test_verify "$UKI_FILE" "$TEMP_CERT"; then
    echo ""
    echo "✓ Verification succeeded (UKI is properly signed)"
else
    EXIT_CODE=$?
    echo ""
    echo "✗ Verification failed (exit code: $EXIT_CODE)"
    echo ""
    echo "This is expected if the UKI is either:"
    echo "  - Not signed at all"
    echo "  - Signed with a different certificate"
    echo "  - Has an invalid signature"
fi

# Cleanup
rm -f "$TEMP_CERT" "$TEMP_KEY"

echo ""
echo "=========================================="
echo "Demo complete"
echo "=========================================="
echo ""
echo "To test with a real signed UKI:"
echo "  ./test_verify <signed-uki.efi> <signing-cert.pem>"
echo ""
