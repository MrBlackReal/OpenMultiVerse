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

# ------------------------------------------------------------
# Clean and create output directories
# ------------------------------------------------------------

rm -rf "${DIST}"
mkdir -p "${DIST}"
mkdir -p "${LIBS_DIR}"

# ------------------------------------------------------------
# Copy binary
# ------------------------------------------------------------

if [ ! -f "verse" ]; then
    echo "ERROR: verse binary not found. Run 'make' first."
    exit 1
fi

cp verse "${DIST}/verse.bin"
chmod +x "${DIST}/verse.bin"

# ------------------------------------------------------------
# Copy assets
# ------------------------------------------------------------

cp -r assets/ "${DIST}/"
rm -f "${DIST}/assets/soundtrack.mov"

# ------------------------------------------------------------
# Bundle shared libraries
# ------------------------------------------------------------

echo ">>> Collecting shared libraries via ldd..."

ldd verse \
    | grep -v "linux-vdso\|ld-linux\|libc\.so\|libpthread\|libdl\.so\|libm\.so\|librt\.so\|libGL\|libEGL" \
    | awk '{print $3}' \
    | grep "^/" \
    | while read -r lib; do
        if [ -f "$lib" ]; then
            echo "  + $(basename "$lib")"
            cp -L "$lib" "${LIBS_DIR}/"
        fi
    done

# ------------------------------------------------------------
# Explicitly bundle GLEW
# ------------------------------------------------------------

echo ">>> Collecting GLEW..."

GLEW_LIB=""

# Try ldconfig first
if command -v ldconfig >/dev/null 2>&1; then
    GLEW_LIB=$(ldconfig -p 2>/dev/null \
        | awk '/libGLEW\.so\.2\.3[[:space:]]/ {print $NF; exit}')
fi

# Fallback locations
if [ -z "${GLEW_LIB}" ]; then
    for candidate in \
        /usr/lib/x86_64-linux-gnu/libGLEW.so.2.3 \
        /usr/lib/x86_64-linux-gnu/libGLEW.so.2.3.0 \
        /usr/local/lib/libGLEW.so.2.3 \
        /usr/local/lib/libGLEW.so.2.3.0
    do
        if [ -f "$candidate" ]; then
            GLEW_LIB="$candidate"
            break
        fi
    done
fi

if [ -z "${GLEW_LIB}" ]; then
    echo "ERROR: libGLEW.so.2.3 was not found."
    echo "Install GLEW development/runtime libraries first."
    exit 1
fi

echo "  + $(basename "${GLEW_LIB}")"
cp -L "${GLEW_LIB}" "${LIBS_DIR}/libGLEW.so.2.3.0"

# Create the expected GLEW SONAME symlinks
ln -sf libGLEW.so.2.3.0 "${LIBS_DIR}/libGLEW.so.2.3"
ln -sf libGLEW.so.2.3.0 "${LIBS_DIR}/libGLEW.so"

# ------------------------------------------------------------
# Show packaged libraries
# ------------------------------------------------------------

echo ""
echo ">>> Packaged libraries:"
ls -lh "${LIBS_DIR}"

# ------------------------------------------------------------
# Create launch wrapper
# ------------------------------------------------------------

cat > "${DIST}/verse" << 'EOF'
#!/usr/bin/env bash

DIR="$(cd "$(dirname "$0")" && pwd)"

export LD_LIBRARY_PATH="${DIR}/libs:${LD_LIBRARY_PATH}"

exec "${DIR}/verse.bin" "$@"
EOF

chmod +x "${DIST}/verse"

# ------------------------------------------------------------
# Verify GLEW is actually present
# ------------------------------------------------------------

echo ""
echo ">>> Verifying GLEW..."

if [ ! -f "${LIBS_DIR}/libGLEW.so.2.3.0" ]; then
    echo "ERROR: GLEW was not packaged correctly."
    exit 1
fi

ls -l "${LIBS_DIR}"/libGLEW.so*

# ------------------------------------------------------------
# Create archive
# ------------------------------------------------------------

echo ""
echo ">>> Creating archive..."

mkdir -p dist
cd dist
tar -czf "${NAME}.tar.gz" "${NAME}/"
cd ..

echo ""
echo "Done! Package ready:"
echo "  dist/${NAME}.tar.gz"
echo ""
echo "Contents:"
ls -lh "${DIST}/"
