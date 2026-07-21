# Java Binding — Build & Run Guide

## Prerequisites

### 1. Build the C shared library

**macOS / Linux:**

```bash
cd <project_root>
cmake -B build && cmake --build build -j$(nproc)
```

Output: `lib/shared/libhttp2client.dylib` (macOS) or `lib/shared/libhttp2client.so` (Linux)

**Windows (MSYS2 MINGW64 shell):**

```bash
cd <project_root>
cmake -B build -G "MinGW Makefiles"
cmake --build build -j4
```

Output: `lib/shared/libhttp2client.dll`

### 2. Install JDK

**macOS:**

```bash
brew install openjdk
echo 'export PATH="/opt/homebrew/opt/openjdk/bin:$PATH"' >> ~/.zshrc
source ~/.zshrc
```

**Windows:**

Download and install [Eclipse Temurin JDK](https://adoptium.net/) (or Oracle JDK).
Ensure `java` and `javac` are on PATH.

```powershell
java -version
```

### 3. Install Maven

**macOS:**

```bash
brew install maven
```

**Windows:**

Download from https://maven.apache.org/download.cgi, extract, and add `bin/` to PATH.

```powershell
mvn -version
```

### Windows additional requirements

| Tool | Purpose |
|------|---------|
| [MSYS2](https://www.msys2.org/) MINGW64 | Builds the C library (`libhttp2client.dll`) and the JNI bridge |
| MinGW-w64 toolchain | `pacman -S mingw-w64-x86_64-toolchain` |
| NASM, Go | BoringSSL build dependencies |
| CMake >= 3.29 | Drives the C library build |

> **Note:** On Windows the JNI bridge (`libhttp2jni.dll`) supports two compilers:
>
> | Compiler | Command | Requirements |
> |----------|---------|-------------|
> | MSVC `cl.exe` (default) | `mvn clean package` | Run from "x64 Native Tools Command Prompt for VS 2022"; requires `http2client.lib` in `lib/shared/` |
> | MinGW `gcc` | `mvn clean package -Djni.compiler=gcc` | `gcc` on PATH (add `C:\msys64\mingw64\bin`) |
>
> Using MinGW gcc links directly against `libhttp2client.dll.a`, no import library needed.

## One-shot build (compile + package + test)

**macOS / Linux:**

```bash
cd java
bash build.sh
```

**Windows (PowerShell, with MinGW on PATH):**

```powershell
cd <project_root>\java
$env:PATH = "C:\msys64\mingw64\bin;" + $env:PATH
mvn clean package "-Djni.compiler=gcc"
java --enable-native-access=ALL-UNNAMED -cp build/http2-client-1.0.0.jar Test
```

**Windows (MSYS2 MINGW64 shell):**

```bash
cd <project_root>/java
export MSYS2_ARG_CONV_EXCL="*"
mvn clean package -Djni.compiler=gcc
java --enable-native-access=ALL-UNNAMED -cp build/http2-client-1.0.0.jar Test
```

> In MINGW64 shell, `export MSYS2_ARG_CONV_EXCL="*"` is required to prevent
> MSYS2 from mangling Maven `-D` arguments.

`build.sh` only performs environment checks (C library, Java, Maven), invokes
`mvn clean package`, then runs the test. All actual build steps live in `pom.xml`.
The build produces the fat JAR: `build/http2-client-1.0.0.jar`

## Using Maven directly

```bash
cd java

# Full build (compile Java + generate JNI header + gcc bridge + package fat JAR)
mvn clean package

# Run a class
mvn exec:java -Dexec.mainClass=Test
mvn exec:java -Dexec.mainClass=Example
```

## Maven build flow (pom.xml)

`mvn clean package` runs the following plugins in order:

| Phase | Plugin | Purpose |
|-------|--------|---------|
| `compile` | `maven-compiler-plugin` | Compile `*.java` and generate the JNI header via `javac -h` into `build/generated-jni/` |
| `process-classes` | `maven-antrun-plugin` | Copy the generated header to `Http2Client_jni.h`; compile the JNI bridge with `gcc` or MSVC `cl.exe` (via `-Djni.compiler`); copy native libs into `build/classes/native/` |
| `package` | `maven-shade-plugin` | Build the fat JAR (classes + `org.json` dependency + `native/` libraries) |

- `org.json` is resolved as a standard Maven dependency (no manual `lib/*.jar` needed).
- OS-specific settings (`.dylib`/`.so`/`.dll`, include dir, rpath) are selected automatically by `<profiles>` for macOS / Linux / Windows.
- The JNI bridge rpath is set to `@loader_path` (macOS) / `$ORIGIN` (Linux) because both native files are extracted to the same temp directory at runtime.
- On Windows, `-Djni.compiler=gcc` selects MinGW; default is MSVC `cl.exe`.

## Usage

### Run from the fat JAR (recommended)

```bash
cd java
java --enable-native-access=ALL-UNNAMED -cp build/http2-client-1.0.0.jar Test
java --enable-native-access=ALL-UNNAMED -cp build/http2-client-1.0.0.jar Example
```

> **`--enable-native-access=ALL-UNNAMED` 版本说明：**
>
> | JDK 版本 | 行为 |
> |----------|------|
> | < 16 | 不支持此参数，使用会报 `Unrecognized option` 错误，**必须省略** |
> | 16 – 23 | 参数已引入（JEP 389），但 JNI 调用不受限制，加不加均可正常运行 |
> | 24+ | JEP 472 生效：未加此参数时，JNI 调用会打印警告（未来版本将变为错误）；加上后警告消除 |
>
> `build.sh` 会自动检测 Java 主版本号，仅在 >= 16 时添加此参数。

### Calling from Java code

`Http2Client` lives in the unnamed module (no package). On the same classpath it can be used directly, without an `import`:

```java
Http2Client.init();
String result = Http2Client.request(requestJsonString);
Http2Client.cleanup();
```

## Fat JAR layout

```
http2-client-1.0.0.jar
├── Http2Client.class
├── Example.class
├── Test.class
├── org/json/*.class        # JSON dependency (inlined)
└── native/
    ├── libhttp2client.{dylib|so|dll}  # C core library
    └── libhttp2jni.{dylib|so|dll}     # JNI bridge
```

At runtime `Http2Client.java` automatically extracts the native libraries from
`/native/` inside the JAR to a temp directory and loads them via `System.load()`.

> **Windows note:** `libhttp2jni.dll` depends on `libhttp2client.dll` and MinGW
> runtime DLLs (`libwinpthread-1.dll`, `libgcc_s_seh-1.dll`, `libstdc++-6.dll`).
> Ensure `C:\msys64\mingw64\bin` is on your system PATH, or copy those DLLs
> next to the extracted files.

## Build artifacts

```
java/build/
├── http2-client-1.0.0.jar          # Fat JAR (final artifact, shade-packaged)
├── original-http2-client-1.0.0.jar # Original JAR before shading
├── generated-jni/Http2Client.h     # JNI header generated by javac -h
└── classes/                        # Compilation output
    ├── *.class
    └── native/                     # gcc output + copied C library
```

## Dependencies

| Dependency | Version | Location | Notes |
|------------|---------|----------|-------|
| `org.json` | 20240303 | Maven dependency | JSON parsing (Java has no built-in JSON), inlined into the fat JAR |
| OpenJDK | 26+ | System install | Provides `java`/`javac`/`jni.h` |
| Maven | 3.9+ | System install | Drives the build via `pom.xml` |

## File overview

| File | Description |
|------|-------------|
| `Http2Client.java` | Core binding class; declares `native` methods and loads libraries via `System.load` (from JAR or filesystem) |
| `Http2Client.c` | JNI C bridge; calls the C library's `initialiseEnv`/`handleRequest`/`cleanupEnv` |
| `Http2Client_jni.h` | JNI header generated by `javac -h` |
| `Example.java` | Example (GET / POST / custom timeout) |
| `Test.java` | Test |
| `build.sh` | Build wrapper (checks env, runs `mvn clean package`, runs test) |
| `pom.xml` | Maven configuration that drives the whole build |

## License

Apache-2.0
