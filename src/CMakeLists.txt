cmake_minimum_required(VERSION ${CMAKE_MINIMUM_REQUIRED_VERSION})
project(src)
# find source files
file(GLOB_RECURSE SRC ./*.c)
# filter out DaemonByMemShare.c
#list(FILTER SRC EXCLUDE REGEX ".*DeamonByMemShare\\.c$")
# find include headers files
include_directories(${HEADER_PATH})
# generated headers (Version.h from configure_file)
include_directories(${CMAKE_BINARY_DIR}/generated)

# threading library (pthread on POSIX / winpthreads on MinGW)
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

# define lib output path
set(LIBRARY_OUTPUT_PATH ${LIB_PATH_SHARED})
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${LIB_PATH_SHARED})
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${LIB_PATH_SHARED})
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${LIB_PATH_SHARED})

# generate lib
add_library(${HTTP_CLIENT} SHARED ${SRC})

# set output name, soname versioning, and ensure it goes to the correct directory
set_target_properties(${HTTP_CLIENT} PROPERTIES
        OUTPUT_NAME ${HTTP_CLIENT}
        VERSION ${HTTPCLIENT_VERSION_MAJOR}.${HTTPCLIENT_VERSION_MINOR}.${HTTPCLIENT_VERSION_PATCH}
        SOVERSION ${HTTPCLIENT_SOVERSION}
        LIBRARY_OUTPUT_DIRECTORY ${LIB_PATH_SHARED}
        ARCHIVE_OUTPUT_DIRECTORY ${LIB_PATH_SHARED}
        LIBRARY_OUTPUT_DIRECTORY_DEBUG ${LIB_PATH_SHARED}
        LIBRARY_OUTPUT_DIRECTORY_RELEASE ${LIB_PATH_SHARED}
        POSITION_INDEPENDENT_CODE ON
)

# ── Size optimization: split each function/data into its own section so the
#    linker can discard unused code (BoringSSL/Brotli/Zstd pull in a lot we
#    never call). Paired with -Wl,--gc-sections / -Wl,-dead_strip below.
target_compile_options(${HTTP_CLIENT} PRIVATE
        $<$<CONFIG:Release>:-ffunction-sections -fdata-sections>
        $<$<CONFIG:RelWithDebInfo>:-ffunction-sections -fdata-sections>
)

# include third parties headers
target_include_directories(${HTTP_CLIENT} PRIVATE
        ${BORINGSSL_DIR}/include
        ${BROTLI_DIR}/c/include
        ${ZSTD_DIR}/lib
        ${JANSSON_DIR}/src
#        ${ZLOG_DIR}/src
)
# linked against third parties
# On Windows, link zlibstatic directly to guarantee static linking;
# newer zlib versions define ZLIB::ZLIB as an alias to the SHARED target,
# which would introduce a runtime dependency on libz.dll.
if (WIN32 AND TARGET zlibstatic)
    set(ZLIB_LINK_TARGET zlibstatic)
else ()
    set(ZLIB_LINK_TARGET ZLIB::ZLIB)
endif ()

target_link_libraries(${HTTP_CLIENT} PRIVATE
        crypto
        ssl
        brotlicommon
        brotlidec
        brotlienc
        ${ZLIB_LINK_TARGET}
        libzstd_static
        jansson
#        zlog
)

# threading + platform-specific system libraries
target_link_libraries(${HTTP_CLIENT} PRIVATE Threads::Threads)

# ── Dead-code stripping: drop unused sections pulled in by the static
#    third-party libs (BoringSSL crypto, Brotli, Zstd, ...).
#    macOS/ld uses -dead_strip; GNU ld/gold/lld use --gc-sections.
if (APPLE)
    target_link_options(${HTTP_CLIENT} PRIVATE
            $<$<CONFIG:Release>:-Wl,-dead_strip>
            $<$<CONFIG:RelWithDebInfo>:-Wl,-dead_strip>
    )
elseif (NOT WIN32)
    target_link_options(${HTTP_CLIENT} PRIVATE
            $<$<CONFIG:Release>:-Wl,--gc-sections>
            $<$<CONFIG:RelWithDebInfo>:-Wl,--gc-sections>
    )
endif ()

# ── Strip symbols from the final shared library in Release builds to shrink
#    the shipped artifact (the 4.1 MB dylib contains a full symbol table).
#    macOS uses -x (or `strip -x`); GNU uses -s. Skipped on Windows (MinGW
#    keeps its runtime DLLs; the addon loads them separately).
if (APPLE)
    target_link_options(${HTTP_CLIENT} PRIVATE $<$<CONFIG:Release>:-Wl,-x>)
elseif (NOT WIN32)
    target_link_options(${HTTP_CLIENT} PRIVATE $<$<CONFIG:Release>:-Wl,-s>)
endif ()

# Windows: link the Winsock2 socket library
if (WIN32)
    target_link_libraries(${HTTP_CLIENT} PRIVATE ws2_32)
endif ()

# Windows/MinGW: stage the MinGW runtime DLLs this GCC links against right next to
# libhttpclient.dll. Downstream consumers (the Node addon, the Java binding, the
# test executables) then copy them from lib/shared -- so the runtimes are ALWAYS
# the exact versions that built the DLL, regardless of what a later shell's PATH
# happens to resolve. Locating them here (from CMAKE_C_COMPILER's own bin dir) is
# deterministic; letting build-addon.js guess via `where cc` is not, and a stale
# copy makes the addon fail to load with "The specified procedure could not be found".
if (WIN32 AND CMAKE_C_COMPILER_ID STREQUAL "GNU")
    get_filename_component(MINGW_BIN_DIR "${CMAKE_C_COMPILER}" DIRECTORY)
    foreach (rt libgcc_s_seh-1.dll libstdc++-6.dll libwinpthread-1.dll)
        if (EXISTS "${MINGW_BIN_DIR}/${rt}")
            add_custom_command(TARGET ${HTTP_CLIENT} POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                            "${MINGW_BIN_DIR}/${rt}" "${LIB_PATH_SHARED}/${rt}"
                    COMMENT "Staging MinGW runtime ${rt} next to libhttpclient.dll")
        endif ()
    endforeach ()
endif ()