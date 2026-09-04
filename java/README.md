# Java Binding — Build & Run Guide

## Prerequisites

### 1. Build the C shared library

**macOS / Linux:**

```bash
cd <project_root>
cmake -B build && cmake --build build -j$(nproc)
```

Output: `lib/shared/libhttpclient.dylib` (macOS) or `lib/shared/libhttpclient.so` (Linux)

**Windows (MSYS2 MINGW64 shell):**

```bash
cd <project_root>
cmake -B build -G "MinGW Makefiles"
cmake --build build -j4
```

Output: `lib/shared/libhttpclient.dll`

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
| [MSYS2](https://www.msys2.org/) MINGW64 | Builds the C library (`libhttpclient.dll`) and the JNI bridge |
| MinGW-w64 toolchain | `pacman -S mingw-w64-x86_64-toolchain` |
| NASM, Go | BoringSSL build dependencies |
| CMake >= 3.29 | Drives the C library build |

> **Note:** On Windows the JNI bridge (`libhttpjni.dll`) supports two compilers:
>
> | Compiler | Command | Requirements |
> |----------|---------|-------------|
> | MSVC `cl.exe` (default) | `mvn clean package` | Run from "x64 Native Tools Command Prompt for VS 2022"; requires `httpclient.lib` in `lib/shared/` |
> | MinGW `gcc` | `mvn clean package -Djni.compiler=gcc` | `gcc` on PATH (add `C:\msys64\mingw64\bin`) |
>
> Using MinGW gcc links directly against `libhttpclient.dll.a`, no import library needed.

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
java --enable-native-access=ALL-UNNAMED -cp build/http-client-1.0.0.jar Test
```

**Windows (MSYS2 MINGW64 shell):**

```bash
cd <project_root>/java
export MSYS2_ARG_CONV_EXCL="*"
mvn clean package -Djni.compiler=gcc
java --enable-native-access=ALL-UNNAMED -cp build/http-client-1.0.0.jar Test
```

> In MINGW64 shell, `export MSYS2_ARG_CONV_EXCL="*"` is required to prevent
> MSYS2 from mangling Maven `-D` arguments.

`build.sh` only performs environment checks (C library, Java, Maven), invokes
`mvn clean package`, then runs the test. All actual build steps live in `pom.xml`.
The build produces the fat JAR: `build/http-client-1.0.0.jar`

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
| `process-classes` | `maven-antrun-plugin` | Copy the generated header to `HttpClient_jni.h`; compile the JNI bridge with `gcc` or MSVC `cl.exe` (via `-Djni.compiler`); copy native libs into `build/classes/native/` |
| `package` | `maven-shade-plugin` | Build the fat JAR (classes + `org.json` dependency + `native/` libraries) |

- `org.json` is resolved as a standard Maven dependency (no manual `lib/*.jar` needed).
- OS-specific settings (`.dylib`/`.so`/`.dll`, include dir, rpath) are selected automatically by `<profiles>` for macOS / Linux / Windows.
- The JNI bridge rpath is set to `@loader_path` (macOS) / `$ORIGIN` (Linux) because both native files are extracted to the same temp directory at runtime.
- On Windows, `-Djni.compiler=gcc` selects MinGW; default is MSVC `cl.exe`.

## Usage

### Run from the fat JAR (recommended)

```bash
cd java
java --enable-native-access=ALL-UNNAMED -cp build/http-client-1.0.0.jar Test
java --enable-native-access=ALL-UNNAMED -cp build/http-client-1.0.0.jar TestStreaming
java --enable-native-access=ALL-UNNAMED -cp build/http-client-1.0.0.jar Example
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

`HttpClient` lives in the unnamed module (no package). On the same classpath it can be used directly, without an `import`:

```java
HttpClient.init();
String result = HttpClient.request(requestJsonString);
HttpClient.cleanup();
```

### Streaming response

`request`, `startRequest` and `requestNonBlocking` each have an overload taking an
`HttpClient.ResponseListener`. With one, the decoded body is handed to `onData`
chunk by chunk instead of being buffered, and the returned JSON reports
`"streamed": 1` with an empty `payload`. Without one the library collects the body
itself and the JSON carries it in `response.payload`.

```java
import java.util.Map;   // the only import a listener needs

final long[] received = {0};
String result = HttpClient.request(requestJsonString, new HttpClient.ResponseListener() {
    @Override
    public void onHeaders(Map<String, String> headers) {
        System.out.println(headers.get(":status"));
    }

    @Override
    public boolean onData(byte[] chunk) {
        received[0] += chunk.length;
        // returning true tears the response down (RST_STREAM); the JSON then reports 3-0014
        return received[0] >= 64 * 1024;
    }

    @Override
    public void onComplete(String errorCode, String errorMessage) {
        System.out.println(errorCode == null ? "clean" : errorCode);
    }
});
```

| Callback | Signature | Notes |
|----------|-----------|-------|
| `onHeaders` | `void onHeaders(Map<String, String> headers)` | Once per attempt, before the body, `:status` first, in wire order |
| `onData` | `boolean onData(byte[] chunk)` | One decoded chunk (gzip / deflate / Brotli / Zstd already inflated), copied per call; `true` stops the response |
| `onComplete` | `void onComplete(String errorCode, String errorMessage)` | Once per attempt, after the last chunk; both `null` when the body ended cleanly |

All three methods are abstract, so the compiler rejects a listener that skips one:
a hole in the contract would leave the body with neither a consumer nor a place in
the response. They run on the thread that receives the bytes — the connection
reader — not on the calling thread, so implementations must be thread safe and must
not call back into this library. The JNI call is synchronous, so `onData` returning
`true` stops the response at once; an exception thrown from it is described on
stderr and stops the response too. On a non-blocking request the listener is held
until `pollRequest` reaps the id.

## Fat JAR layout

```
http-client-1.0.0.jar
├── HttpClient.class
├── Example.class
├── Test.class
├── org/json/*.class        # JSON dependency (inlined)
└── native/
    ├── libhttpclient.{dylib|so|dll}  # C core library
    └── libhttpjni.{dylib|so|dll}     # JNI bridge
```

At runtime `HttpClient.java` automatically extracts the native libraries from
`/native/` inside the JAR to a temp directory and loads them via `System.load()`.

> **Windows note:** `libhttpjni.dll` depends on `libhttpclient.dll` and MinGW
> runtime DLLs (`libwinpthread-1.dll`, `libgcc_s_seh-1.dll`, `libstdc++-6.dll`).
> Ensure `C:\msys64\mingw64\bin` is on your system PATH, or copy those DLLs
> next to the extracted files.

## Using the Release Artifacts

Each [GitHub Release](https://github.com/gointuition/HTTP-Request-Client/releases) ships the binding as a **single, cross-platform fat JAR**:

- `http-client-java-<ver>.jar` — Java bytecode **plus** all three platforms'
  native libraries laid out as `native/linux/`, `native/macos/`, `native/win/`

At runtime `HttpClient.loadNativeLibrary()` selects the matching `native/<plat>/`
sub-directory from `os.name`, so **one artifact runs unchanged on Linux, macOS, and
Windows** — no classifier, no per-platform dependency.

Put the single JAR on the classpath:

```bash
# JDK >= 16 needs --enable-native-access; JDK < 16 must omit it
java --enable-native-access=ALL-UNNAMED \
  -cp "http-client-java-1.0.0.jar" Test
java --enable-native-access=ALL-UNNAMED \
  -cp "http-client-java-1.0.0.jar" Example
```

In Maven you declare just one dependency, regardless of deployment OS:

```xml
<dependency>
  <groupId>com.example</groupId>
  <artifactId>http-client-java</artifactId>
  <version>1.0.0</version>
</dependency>
```

Embed it as a normal dependency (`HttpClient` lives in the unnamed module, call directly without import):

```java
HttpClient.init();
String result = HttpClient.request(requestJsonString);
HttpClient.cleanup();
```

> **Windows note:** the JNI bridge depends on MinGW runtime DLLs (`libwinpthread-1.dll`, `libgcc_s_seh-1.dll`, `libstdc++-6.dll`). Ensure `C:\msys64\mingw64\bin` is on `PATH`, or copy those DLLs next to the JAR.

## Build artifacts

```
java/build/
├── http-client-1.0.0.jar          # Fat JAR (final artifact, shade-packaged)
├── original-http-client-1.0.0.jar # Original JAR before shading
├── generated-jni/HttpClient.h     # JNI header generated by javac -h
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
| `HttpClient.java` | Core binding class; declares `native` methods and loads libraries via `System.load` (from JAR or filesystem) |
| `HttpClient.c` | JNI C bridge; calls the C library's `initialiseEnv`/`handleRequest`/`handleResponse`/`cleanupEnv`, and bridges a `ResponseListener` onto the `ResponseStream` contract |
| `HttpClient_jni.h` | JNI header generated by `javac -h` |
| `Example.java` | Example (GET / POST / custom timeout) |
| `Test.java` | Test |
| `TestStreaming.java` | Streaming test (callbacks, abort, incremental gzip decode) |
| `build.sh` | Build wrapper (checks env, runs `mvn clean package`, runs test) |
| `pom.xml` | Maven configuration that drives the whole build |

## License

Apache-2.0
