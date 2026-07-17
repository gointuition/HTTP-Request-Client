#!/bin/bash
# nodejs/build.sh

set -e

echo "=== Building HTTP/2 Node.js Addon ==="

# Get the directory where this script is located
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

echo "Project root: $PROJECT_ROOT"
echo "Node.js addon directory: $SCRIPT_DIR"

# Check if shared library exists
LIB_NAME="libhttp2client.dylib"
if [ "$(uname)" = "Linux" ]; then
    LIB_NAME="libhttp2client.so"
fi

if [ ! -f "$PROJECT_ROOT/lib/shared/$LIB_NAME" ]; then
    echo "Error: $LIB_NAME not found. Please build the shared library first."
    echo "Run: cd $PROJECT_ROOT && mkdir -p build && cd build && cmake .. && make"
    exit 1
fi

echo ""
echo "Shared library found: $PROJECT_ROOT/lib/shared/$LIB_NAME"

# Change to nodejs directory
cd "$SCRIPT_DIR"

# Install dependencies
echo ""
echo "Installing dependencies..."
npm install

# Build the addon
echo ""
echo "Building native addon..."
npm run build

echo ""
echo "✓ Build completed successfully!"
echo ""
echo "To test the addon, run:"
echo "  npm test"
echo ""
echo "To use in your project:"
echo "  const httpClient = require('./nodejs');"
