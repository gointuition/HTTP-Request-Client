#!/usr/bin/env bash
# java/build.sh
#
# Build script for HTTP/2 Java binding (JNI) — Maven driven.
# Mirrors nodejs/build.sh and python/build.sh.
#
# All build steps live in pom.xml and run via `mvn clean package`:
#   1. maven-compiler-plugin : compile *.java + generate JNI header (javac -h)
#   2. maven-antrun-plugin    : compile JNI C bridge (gcc) + stage native libs
#   3. maven-shade-plugin     : package fat JAR (classes + org.json + native/)
# This script only checks prerequisites, runs Maven, then runs the test.

set -e

echo "=== Building HTTP/2 Java Binding (JNI, Maven) ==="

# Get the directory where this script is located
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

echo "Project root: $PROJECT_ROOT"
echo "Java binding directory: $SCRIPT_DIR"

# Check if the prebuilt native C library exists
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

# Check Java
echo ""
JAVA_VERSION_OUTPUT="$(java -version 2>&1)" || {
    echo "Error: Java not found. Install with: brew install openjdk"
    echo "Then add to PATH: export PATH=\"/opt/homebrew/opt/openjdk/bin:\$PATH\""
    exit 1
}
echo "Java version:"
echo "$JAVA_VERSION_OUTPUT"

# Parse the major version (JDK 8 reports "1.8.x"; JDK 9+ reports "N.x.x")
JAVA_VERSION_STR="$(echo "$JAVA_VERSION_OUTPUT" | head -1 | sed -E 's/.*version "([^"]+)".*/\1/')"
if [ "${JAVA_VERSION_STR%%.*}" = "1" ]; then
    JAVA_MAJOR="$(echo "$JAVA_VERSION_STR" | cut -d. -f2)"
else
    JAVA_MAJOR="${JAVA_VERSION_STR%%.*}"
fi
echo "Java major version: $JAVA_MAJOR"

# --enable-native-access exists only on JDK 16+ (JEP 389); older JDKs reject it
# as an unrecognized option. JDK 24+ (JEP 472) also warns on native access
# without this flag, so pass it whenever the running JDK supports it.
NATIVE_ACCESS_FLAG=""
if [ "$JAVA_MAJOR" -ge 16 ] 2>/dev/null; then
    NATIVE_ACCESS_FLAG="--enable-native-access=ALL-UNNAMED"
fi

# Determine JAVA_HOME (Maven and the JNI header path both rely on a JDK)
JAVA_HOME="${JAVA_HOME:-$(/usr/libexec/java_home 2>/dev/null || echo "/opt/homebrew/opt/openjdk/libexec/openjdk.jdk/Contents/Home")}"
export JAVA_HOME
echo "JAVA_HOME: $JAVA_HOME"

# Check Maven
echo ""
if ! command -v mvn >/dev/null 2>&1; then
    echo "Error: Maven (mvn) not found. Install with: brew install maven"
    exit 1
fi
mvn -version 2>&1 | head -1

VERSION="1.0.0"
JAR_NAME="http2-client-${VERSION}.jar"

# ── Build everything through Maven ────────────────────────────────────
echo ""
echo "Running: mvn -f \"$SCRIPT_DIR/pom.xml\" clean package"
mvn -f "$SCRIPT_DIR/pom.xml" clean package

echo ""
echo "Fat JAR created: build/$JAR_NAME"
echo ""
echo "JAR contents:"
jar tf "$SCRIPT_DIR/build/$JAR_NAME" | head -20
echo "  ..."

# ── Run test from the fat JAR (classpath mode) ────────────────────────
echo ""
echo "Running test from fat JAR (classpath mode)..."
cd "$SCRIPT_DIR"
java $NATIVE_ACCESS_FLAG -cp "build/$JAR_NAME" Test

echo ""
echo "Build completed successfully!"
echo ""
echo "Fat JAR: build/$JAR_NAME"
echo ""
echo "To use in your project:"
echo "  java $NATIVE_ACCESS_FLAG -cp build/$JAR_NAME Example"
echo ""
echo "Java code:"
echo "  Http2Client.init();"
echo "  String result = Http2Client.request(\"{...}\");"
echo "  Http2Client.cleanup();"
