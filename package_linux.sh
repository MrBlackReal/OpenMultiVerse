#!/usr/bin/env bash
# Run this on Linux after compiling with: make
# Usage: bash package_linux.sh [version]
# Example: bash package_linux.sh 1.0.0
set -e

VERSION=${1:-"dev"}
NAME="verse-linux-x64-${VERSION}"
DIST="dist/${NAME}"
LIBS_DIR="${DIST}/libs"

echo ">>> Packaging verse for Linux: ${NAME}"

# Clean and create output directories
rm -rf "${DIST}"
mkdir -p "${DIST}"
mkdir -p "${LIBS_DIR}"

# Copy binary
if [ ! -f "verse" ]; then
    echo "ERROR: verse binary not found. Run 'make' first."
    exit 1
fi
cp verse "${DIST}/verse.bin"
chmod +x "${DIST}/verse.bin"

# Copy assets
cp -r assets/ "${DIST}/"
rm -f "${DIST}/assets/soundtrack.mov"

# Bundle shared libraries (SDL2, SDL2_ttf, SDL2_mixer, GLEW and their deps)
echo ">>> Collecting shared libraries via ldd..."
ldd verse \
    | grep -v "linux-vdso\|ld-linux\|libc\.so\|libpthread\|libdl\.so\|libm\.so\|librt\.so\|libGL\|libEGL" \
    | awk '{print $3}' \
    | grep "^/" \
    | while read lib; do
        if [ -f "$lib" ]; then
            echo "  + $(basename $lib)"
            cp "$lib" "${LIBS_DIR}/"
        fi
    done

# Create launch wrapper that sets LD_LIBRARY_PATH
cat > "${DIST}/verse" << 'EOF'
#!/usr/bin/env bash
DIR="$(cd "$(dirname "$0")" && pwd)"
export LD_LIBRARY_PATH="${DIR}/libs:${LD_LIBRARY_PATH}"
exec "${DIR}/verse.bin" "$@"
EOF
chmod +x "${DIST}/verse"

# Create archive
echo ">>> Creating archive..."
mkdir -p dist
cd dist
tar -czf "${NAME}.tar.gz" "${NAME}/"
cd ..

echo ""
echo "Done! Package ready: dist/${NAME}.tar.gz"
echo "Contents:"
ls -lh "${DIST}/"
