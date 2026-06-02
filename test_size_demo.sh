#!/bin/bash
#
# test_size_demo.sh - Demonstrate UKI size analysis
#

set -e

echo "=========================================="
echo "UKI Size Analysis Demo"
echo "=========================================="
echo ""

# Check if test_size exists
if [ ! -x ./test_size ]; then
    echo "ERROR: test_size not found or not executable"
    echo "Build it first with: make test-size"
    exit 1
fi

# Find UKI files to analyze
UKI_FILES=(
    "../ukiboot_a.img"
)

# Add any .efi files found in /boot if accessible
if [ -d /boot ]; then
    while IFS= read -r -d '' file; do
        UKI_FILES+=("$file")
    done < <(find /boot -maxdepth 2 -name "*.efi" -type f -print0 2>/dev/null)
fi

if [ ${#UKI_FILES[@]} -eq 0 ]; then
    echo "No UKI files found to analyze"
    exit 1
fi

echo "Found ${#UKI_FILES[@]} UKI file(s) to analyze"
echo ""

# Analyze each UKI file
for uki_file in "${UKI_FILES[@]}"; do
    if [ ! -f "$uki_file" ]; then
        echo "Skipping missing file: $uki_file"
        echo ""
        continue
    fi

    echo "Analyzing: $uki_file"
    echo "----------------------------------------"

    # Run the size analysis
    ./test_size "$uki_file" || {
        echo "Failed to analyze $uki_file"
        echo ""
        continue
    }

    echo ""
done

echo "=========================================="
echo "Demo complete"
echo "=========================================="
echo ""
echo "The size analysis shows:"
echo "  - Expected UKI size: Based on PE headers and sections"
echo "  - Actual file size: Physical file size on disk"
echo "  - Extra padding: Any data appended after the UKI"
echo ""
echo "Extra padding can be:"
echo "  - Alignment padding for storage devices"
echo "  - Reserved space for future updates"
echo "  - Unused space from file truncation/allocation"
echo ""
