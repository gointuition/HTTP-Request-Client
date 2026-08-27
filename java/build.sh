#!/usr/bin/env bash
# java/build.sh
#
# Build script for HTTP Java binding (JNI) — Maven driven.
# Mirrors nodejs/build.sh and python/build.sh.
#
# All build steps live in pom.xml and run via `mvn clean package`:
#   1. maven-compiler-plugin : compile *.java + generate JNI header (javac -h)
#   2. maven-antrun-plugin    : compile JNI C bridge (gcc) + stage native libs
#   3. maven-shade-plugin     : package fat JAR (classes + org.json + native/)
# This script only checks prerequisites, runs Maven, then runs the test.

set -e

echo "=== Building HTTP Java Binding (JNI, Maven) ==="

# Get the directory where this script is located
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

echo "Project root: $PROJECT_ROOT"
echo "Java binding directory: $SCRIPT_DIR"

# Check if the prebuilt native C library exists
OS_NAME="$(uname)"
case "$OS_NAME" in
    Darwin)  LIB_NAME="libhttpclient.dylib" ;;
    Linux)   LIB_NAME="libhttpclient.so" ;;
    MINGW*|MSYS*|CYGWIN*)  LIB_NAME="libhttpclient.dll" ;;
    *)       LIB_NAME="libhttpclient.so" ;;
esac

if [ ! -f "$PROJECT_ROOT/lib/shared/$LIB_NAME" ]; then
    echo "Error: $LIB_NAME not found. Please build the shared library first."
    echo "  macOS/Linux: cd $PROJECT_ROOT && cmake -B build && cmake --build build -j\$(nproc)"
    echo "  Windows:     cd $PROJECT_ROOT && cmake -B build -G 'MinGW Makefiles' && cmake --build build -j4"
    exit 1
fi

echo ""
echo "Shared library found: $PROJECT_ROOT/lib/shared/$LIB_NAME"

# Check Java
echo ""
JAVA_VERSION_OUTPUT="$(java -version 2>&1)" || {
    echo "Error: Java not found."
    echo "  macOS:   brew install openjdk"
    echo "  Windows: install Eclipse Temurin JDK and add to PATH"
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
if [ -z "$JAVA_HOME" ]; then
    case "$OS_NAME" in
        Darwin)
            JAVA_HOME="$(/usr/libexec/java_home 2>/dev/null || echo "/opt/homebrew/opt/openjdk/libexec/openjdk.jdk/Contents/Home")"
            ;;
        MINGW*|MSYS*|CYGWIN*)
            # On Windows, derive from 'java' on PATH (resolve symlinks)
            JAVA_EXE="$(which java 2>/dev/null)"
            if [ -n "$JAVA_EXE" ]; then
                # java.exe is in JAVA_HOME/bin/java.exe
                JAVA_HOME="$(cd "$(dirname "$JAVA_EXE")/.." && pwd -W 2>/dev/null || cd "$(dirname "$JAVA_EXE")/.." && pwd)"
            fi
            ;;
        *)
            # Linux: try common locations
            JAVA_HOME="$(dirname "$(dirname "$(readlink -f "$(which java)")")")"
            ;;
    esac
fi
export JAVA_HOME
echo "JAVA_HOME: $JAVA_HOME"

# Check Maven
# Prefer the Maven Wrapper (./mvnw) when present: it downloads a pinned Maven
# from Maven Central (repo.maven.apache.org), which is stable and does not
# suffer the rate-limiting / truncated-download failures that apache.org's
# dlcdn / archive hosts intermittently hit on CI runners (which surface as
# "Could not find or load main class
# org.codehaus.plexus.classworlds.launcher.Launcher"). Fall back to a system
# `mvn` only if there is no wrapper.
echo ""
MVN="mvn"
if [ -x "$SCRIPT_DIR/mvnw" ]; then
    MVN="$SCRIPT_DIR/mvnw"
    echo "Using Maven Wrapper: $MVN"
else
    if ! command -v mvn >/dev/null 2>&1; then
        echo "Error: Maven (mvn) not found and no Maven Wrapper (./mvnw) present."
        echo "  macOS:   brew install maven"
        echo "  Windows: download from https://maven.apache.org/download.cgi and add bin/ to PATH"
        echo "  Or generate a wrapper: cd java && mvn -N wrapper:wrapper"
        exit 1
    fi
fi
"$MVN" -version 2>&1 | head -1

# On Windows (MSYS2), verify gcc is available for JNI bridge compilation
if [[ "$OS_NAME" == MINGW* || "$OS_NAME" == MSYS* ]]; then
    if ! command -v gcc >/dev/null 2>&1; then
        echo "Error: gcc not found. Run this script from the MSYS2 MINGW64 shell,"
        echo "or add C:\\msys64\\mingw64\\bin to your PATH."
        exit 1
    fi
fi

VERSION="1.0.0"
JAR_NAME="http-client-${VERSION}.jar"

# On Windows (MSYS2), override the Maven windows profile's default MSVC
# compiler with gcc so the JNI bridge links against the MinGW-built
# libhttpclient.dll without needing an import library.
JNI_COMPILER_FLAG=""
if [[ "$OS_NAME" == MINGW* || "$OS_NAME" == MSYS* ]]; then
    JNI_COMPILER_FLAG="-Djni.compiler=gcc"
fi

# ── Build everything through Maven ────────────────────────────────────
echo ""
echo "Running: $MVN -f \"$SCRIPT_DIR/pom.xml\" clean package $JNI_COMPILER_FLAG"
"$MVN" -f "$SCRIPT_DIR/pom.xml" clean package $JNI_COMPILER_FLAG

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
echo "  HttpClient.init();"
echo "  String result = HttpClient.request(\"{...}\");"
echo "  HttpClient.cleanup();"
