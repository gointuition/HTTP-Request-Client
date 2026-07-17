# Java 绑定编译与执行指南

## 前置条件

### 1. 构建 C 共享库

```bash
cd /Users/wangbingjie/Documents/Clion/Http2
mkdir -p build && cd build
cmake .. && make
```

产物：`lib/shared/libhttp2client.dylib`

### 2. 安装 OpenJDK

```bash
brew install openjdk
# 添加到 PATH（写入 ~/.zshrc 永久生效）
echo 'export PATH="/opt/homebrew/opt/openjdk/bin:$PATH"' >> ~/.zshrc
source ~/.zshrc
# 验证
java -version
```

## 一键构建（编译 + 打包 + 测试）

```bash
cd java
bash build.sh
```

构建完成后生成 fat JAR：`build/http2-client-1.0.0.jar`

## 手动分步构建

### Step 1: 编译 Java 源码 + 生成 JNI 头文件

```bash
cd java
mkdir -p build
javac -cp "lib/json-20240303.jar" -h . -d build *.java
```

生成 `Http2Client.h`（JNI 头文件），需重命名避免与项目头文件冲突：

```bash
mv Http2Client.h Http2Client_jni.h
```

### Step 2: 编译 JNI C 桥接库

```bash
JAVA_HOME="/opt/homebrew/opt/openjdk/libexec/openjdk.jdk/Contents/Home"

gcc -shared -fPIC \
    -I"$JAVA_HOME/include" \
    -I"$JAVA_HOME/include/darwin" \
    -I. \
    -L../lib/shared \
    -lhttp2client \
    -Wl,-rpath,@loader_path \
    -o build/libhttp2jni.dylib \
    Http2Client.c
```

产物：`build/libhttp2jni.dylib`（链接 `libhttp2client.dylib`）

> rpath 设为 `@loader_path`，因为 fat JAR 运行时两个 .dylib 被提取到同一临时目录。

### Step 3: 打包 Fat JAR

```bash
mkdir -p build/stage/native
cd build/stage

# 复制 class 文件
cp ../build/*.class .

# 解压 json 依赖
jar xf ../../lib/json-20240303.jar
rm -rf META-INF

# 复制 native 库到 native/ 目录
cp ../../build/libhttp2jni.dylib native/
cp ../../../lib/shared/libhttp2client.dylib native/

# 创建 manifest
cat > MANIFEST.MF << 'EOF'
Manifest-Version: 1.0
Implementation-Version: 1.0.0
Implementation-Title: HTTP/2 Client
EOF

# 打包
jar cfm ../http2-client-1.0.0.jar MANIFEST.MF *.class org/ native/
```

产物：`build/http2-client-1.0.0.jar`（1.6 MB，自包含）

### Step 4: 运行测试

```bash
cd java
java --enable-native-access=ALL-UNNAMED -cp build/http2-client-1.0.0.jar Test
```

> 使用 classpath 模式运行，需要 `--enable-native-access=ALL-UNNAMED` 参数启用原生访问。

## 使用方式

### 从 Fat JAR 运行（推荐）

```bash
cd java
java --enable-native-access=ALL-UNNAMED -cp build/http2-client-1.0.0.jar Test
java --enable-native-access=ALL-UNNAMED -cp build/http2-client-1.0.0.jar Example
```

### 开发模式（从 build 目录运行）

```bash
cd java
java --enable-native-access=ALL-UNNAMED -cp "build:lib/json-20240303.jar" Test
```

### Java 代码调用

```java
import Http2Client;

Http2Client.init();
String result = Http2Client.request(requestJsonString);
Http2Client.cleanup();
```

## Fat JAR 结构

```
http2-client-1.0.0.jar  (1.6 MB)
├── Http2Client.class
├── Example.class
├── Test.class
├── org/json/*.class        # JSON 依赖（已内联）
└── native/
    ├── libhttp2client.dylib  # C 核心库
    └── libhttp2jni.dylib     # JNI 桥接库
```

运行时 `Http2Client.java` 自动从 JAR 内 `/native/` 提取 .dylib 到临时目录
（`$TMPDIR/http2client-native/`），通过 `System.load()` 加载。

## 构建产物清单

```
java/build/
├── http2-client-1.0.0.jar  # Fat JAR（最终产物）
├── Http2Client.class
├── Example.class
├── Test.class
├── libhttp2jni.dylib       # JNI 桥接库
└── stage/                  # 打包临时目录
```

## 依赖

| 依赖 | 版本 | 位置 | 说明 |
|------|------|------|------|
| `org.json` | 20240303 | `lib/json-20240303.jar` | JSON 解析（Java 无内置 JSON），已内联进 fat JAR |
| OpenJDK | 26+ | 系统安装 | 提供 `java`/`javac`/`jni.h` |

## 文件说明

| 文件 | 说明 |
|------|------|
| `Http2Client.java` | 核心绑定类，声明 `native` 方法，`System.load` 加载库（支持从 JAR 和文件系统加载） |
| `Http2Client.c` | JNI C 桥接，调用 C 库的 `initialiseEnv`/`handleRequest`/`cleanupEnv` |
| `Http2Client_jni.h` | `javac -h` 生成的 JNI 头文件 |
| `Example.java` | 示例（GET / POST / 自定义超时） |
| `Test.java` | 测试 |
| `build.sh` | 自动化构建脚本（编译 → JNI → 打包 → 测试） |
| `pom.xml` | Maven 配置 |

## License

Apache-2.0
