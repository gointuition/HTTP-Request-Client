#!/bin/bash
# java/build.sh
#
# Build script for HTTP/2 Java binding (JNI).
# Mirrors nodejs/build.sh and python/build.sh.
#
# Steps:
#   1. Compile Java sources (javac -h generates JNI header)
#   2. Compile JNI C bridge (gcc -> libhttp2jni.dylib)
#   3. Package fat JAR (class files + json jar + native libs)
#   4. Run test (classpath mode, --enable-native-access=ALL-UNNAMED)

set -e

echo "=== Building HTTP/2 Java Binding (JNI) ==="

# Get the directory where this script is located
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

echo "Project root: $PROJECT_ROOT"
echo "Java binding directory: $SCRIPT_DIR"

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

# Check Java version
echo ""
echo "Java version:"
java -version 2>&1 || {
    echo "Error: Java not found. Install with: brew install openjdk"
    echo "Then add to PATH: export PATH=\"/opt/homebrew/opt/openjdk/bin:\$PATH\""
    exit 1
}

# Determine JAVA_HOME
JAVA_HOME="${JAVA_HOME:-$(/usr/libexec/java_home 2>/dev/null || echo "/opt/homebrew/opt/openjdk/libexec/openjdk.jdk/Contents/Home")}"
echo "JAVA_HOME: $JAVA_HOME"

# Determine OS-specific settings
OS_NAME="$(uname)"
if [ "$OS_NAME" = "Darwin" ]; then
    JNI_LIB="libhttp2jni.dylib"
    CLIENT_LIB="libhttp2client.dylib"
    RPATH_FLAG="-Wl,-rpath,@loader_path"
elif [ "$OS_NAME" = "Linux" ]; then
    JNI_LIB="libhttp2jni.so"
    CLIENT_LIB="libhttp2client.so"
    RPATH_FLAG="-Wl,-rpath,\$ORIGIN"
else
    echo "Error: Unsupported OS: $OS_NAME"
    exit 1
fi

VERSION="1.0.0"
JAR_NAME="http2-client-${VERSION}.jar"

# Clean
rm -rf "$SCRIPT_DIR/build"
mkdir -p "$SCRIPT_DIR/build"

# ── Step 1: Compile Java sources + generate JNI header ────────────────
echo ""
echo "Step 1: Compiling Java sources..."
CLASSPATH="$SCRIPT_DIR/lib/json-20240303.jar"
javac -cp "$CLASSPATH" -h "$SCRIPT_DIR" -d "$SCRIPT_DIR/build" "$SCRIPT_DIR"/*.java
echo "Java compilation successful."

# Rename generated JNI header to avoid clash with project's Http2Client.h
if [ -f "$SCRIPT_DIR/Http2Client.h" ]; then
    mv -f "$SCRIPT_DIR/Http2Client.h" "$SCRIPT_DIR/Http2Client_jni.h"
    echo "Generated JNI header: Http2Client_jni.h"
fi

# ── Step 2: Compile JNI C bridge ──────────────────────────────────────
echo ""
echo "Step 2: Compiling JNI bridge (Http2Client.c -> $JNI_LIB)..."

# For fat JAR packaging, native libs sit in the same /native/ directory
# inside the JAR, so rpath should point to @loader_path (same dir at runtime)
gcc -shared -fPIC \
    -I"$JAVA_HOME/include" \
    -I"$JAVA_HOME/include/darwin" \
    -I"$SCRIPT_DIR" \
    -L"$PROJECT_ROOT/lib/shared" \
    -lhttp2client \
    $RPATH_FLAG \
    -o "$SCRIPT_DIR/build/$JNI_LIB" \
    "$SCRIPT_DIR/Http2Client.c"

echo "JNI bridge compiled: build/$JNI_LIB"

# ── Step 3: Package fat JAR ───────────────────────────────────────────
echo ""
echo "Step 3: Packaging fat JAR ($JAR_NAME)..."

STAGE_DIR="$SCRIPT_DIR/build/stage"
mkdir -p "$STAGE_DIR/native"

# Copy class files
cp -r "$SCRIPT_DIR"/build/*.class "$STAGE_DIR/"

# Unpack json dependency into staging dir
cd "$STAGE_DIR"
jar xf "$SCRIPT_DIR/lib/json-20240303.jar"
# Remove json's META-INF to avoid signing conflicts
rm -rf "$STAGE_DIR/META-INF"

# Copy native libraries into /native/ directory
cp "$SCRIPT_DIR/build/$JNI_LIB" "$STAGE_DIR/native/"
cp "$PROJECT_ROOT/lib/shared/$CLIENT_LIB" "$STAGE_DIR/native/"

# Create manifest
cat > "$STAGE_DIR/MANIFEST.MF" << EOF
Manifest-Version: 1.0
Implementation-Version: ${VERSION}
Implementation-Title: HTTP/2 Client
EOF

# Build the fat JAR (include class files, org.json, native libs)
cd "$STAGE_DIR"
jar cfm "$SCRIPT_DIR/build/$JAR_NAME" MANIFEST.MF *.class org/ native/
echo "Fat JAR created: build/$JAR_NAME"

# Show JAR contents
echo ""
echo "JAR contents:"
jar tf "$SCRIPT_DIR/build/$JAR_NAME" | head -20
echo "  ..."

# ── Step 4: Run test from fat JAR (classpath mode) ───────────────────
echo ""
echo "Step 4: Running test from fat JAR (classpath mode)..."
cd "$PROJECT_ROOT/java"
java --enable-native-access=ALL-UNNAMED -cp "build/$JAR_NAME" Test

echo ""
echo "Build completed successfully!"
echo ""
echo "Fat JAR: build/$JAR_NAME"
echo ""
echo "To use in your project:"
echo "  java --enable-native-access=ALL-UNNAMED -cp build/$JAR_NAME Example"
echo ""
echo "Java code:"
echo "  Http2Client.init();"
echo "  String result = Http2Client.request(\"{...}\");"
echo "  Http2Client.cleanup();"
