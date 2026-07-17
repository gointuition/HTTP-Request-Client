#!/bin/bash
# python/build.sh

set -e

echo "=== Building HTTP/2 Python Binding ==="

# Get the directory where this script is located
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

echo "Project root: $PROJECT_ROOT"
echo "Python binding directory: $SCRIPT_DIR"

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

# Check Python version
echo ""
echo "Python version:"
python3 --version

# Install cffi dependency
echo ""
echo "Installing cffi dependency..."
pip3 install cffi

# Run test
echo ""
echo "Running test..."
cd "$PROJECT_ROOT"
python3 python/test.py

echo ""
echo "Build completed successfully!"
echo ""
echo "To use in your project:"
echo "  from python import httpClient"
echo "  httpClient.init()"
echo "  result = httpClient.request({...})"
echo "  httpClient.cleanup()"
