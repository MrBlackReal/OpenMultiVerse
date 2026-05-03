#!/usr/bin/env bash
# Run this in MSYS2 MINGW64 after compiling with: mingw32-make
# Usage: bash package_windows.sh [version]
# Example: bash package_windows.sh 1.0.0
set -e

VERSION=${1:-"dev"}
NAME="verse-windows-x64-${VERSION}"
DIST="dist/${NAME}"

echo ">>> Packaging verse for Windows: ${NAME}"

# Clean and create output directory
rm -rf "${DIST}"
mkdir -p "${DIST}"

# Copy executable
if [ ! -f "verse.exe" ]; then
    echo "ERROR: verse.exe not found. Run 'mingw32-make' first."
    exit 1
fi
cp verse.exe "${DIST}/"

# Copy assets
cp -r assets/ "${DIST}/"

# Copy all required DLLs (filter out Windows system DLLs)
echo ">>> Collecting DLLs via ldd..."
ldd verse.exe \
    | grep -iv "/c/windows" \
    | grep -v "not found" \
    | awk '{print $3}' \
    | grep -v "^$" \
    | while read dll; do
        if [ -f "$dll" ]; then
            echo "  + $(basename $dll)"
            cp "$dll" "${DIST}/"
        fi
    done

# Create ZIP archive
echo ">>> Creating archive..."
mkdir -p dist
cd dist
zip -r "${NAME}.zip" "${NAME}/"
cd ..

echo ""
echo "Done! Package ready: dist/${NAME}.zip"
echo "Contents:"
ls "${DIST}/"
