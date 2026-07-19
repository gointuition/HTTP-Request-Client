cmake_minimum_required(VERSION 3.29)
project(Http2 C)

set(CMAKE_C_STANDARD 17)

set(CMAKE_MINIMUM_REQUIRED_VERSION 3.29)

# libssl.a can not be used when making a shared object, so compile it with PIC
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Jansson: Warning: Policy CMP0148 is not set: The FindPythonInterp and FindPythonLibs modules are removed.
cmake_policy(SET CMP0148 NEW)

# define headers path
set(HEADER_PATH ${PROJECT_SOURCE_DIR}/include)
# define app path
set(EXEC_PATH ${PROJECT_SOURCE_DIR}/bin)
# define lib path
set(LIB_PATH ${PROJECT_SOURCE_DIR}/lib/static)
set(LIB_PATH_SHARED ${PROJECT_SOURCE_DIR}/lib/shared)

# define lib name
set(HTTP2_CLIENT http2client)

# force static linking for all libraries
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build shared libraries" FORCE)

########## third parties ####################################

##### Boringssl start

set(BORINGSSL_DIR ${PROJECT_SOURCE_DIR}/third_party/boringssl)

# check if Boringssl exists, if not, download it
if (NOT EXISTS ${BORINGSSL_DIR})
    message(STATUS "BoringSSL not found, downloading...")
    execute_process(
            COMMAND git clone https://github.com/google/boringssl.git ${BORINGSSL_DIR}
            RESULT_VARIABLE GIT_RESULT
    )
    if (NOT GIT_RESULT EQUAL 0)
        message(FATAL_ERROR "Failed to download BoringSSL")
    endif ()
endif ()

# add BoringSSL as a subdirectory
add_subdirectory(${BORINGSSL_DIR})

##### Boringssl end

##### Brotli start

set(BROTLI_DIR ${PROJECT_SOURCE_DIR}/third_party/brotli)

if (NOT EXISTS ${BROTLI_DIR})
    message(STATUS "Brotli not found, downloading...")
    execute_process(
            COMMAND git clone https://github.com/google/brotli.git ${BROTLI_DIR}
            RESULT_VARIABLE GIT_RESULT
    )
    if (NOT GIT_RESULT EQUAL 0)
        message(FATAL_ERROR "Failed to download Brotli")
    endif ()
endif ()

add_subdirectory(${BROTLI_DIR})

##### zlib start

# Prefer the system zlib; if it is not installed (common on Windows),
# download and build it from source, then expose the same ZLIB::ZLIB target
# so downstream link lines don't need to change.
find_package(ZLIB QUIET)

if (NOT ZLIB_FOUND)
    message(STATUS "System zlib not found, downloading and building from source...")

    set(ZLIB_DIR ${PROJECT_SOURCE_DIR}/third_party/zlib)

    if (NOT EXISTS ${ZLIB_DIR})
        execute_process(
                COMMAND git clone https://github.com/madler/zlib.git ${ZLIB_DIR}
                RESULT_VARIABLE GIT_RESULT
        )
        if (NOT GIT_RESULT EQUAL 0)
            message(FATAL_ERROR "Failed to download zlib")
        endif ()
    endif ()

    # Only build the static library; skip examples/tests
    set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "Build zlib examples" FORCE)
    set(SKIP_INSTALL_ALL ON CACHE BOOL "Skip zlib install" FORCE)

    add_subdirectory(${ZLIB_DIR})

    # zlib's CMake exports 'zlib' (shared) and 'zlibstatic' (static) targets.
    # Recent zlib versions already define the ZLIB::ZLIB alias themselves, so
    # only create it when it is missing to keep downstream ZLIB::ZLIB links working.
    if (NOT TARGET ZLIB::ZLIB)
        if (TARGET zlibstatic)
            add_library(ZLIB::ZLIB ALIAS zlibstatic)
        else ()
            add_library(ZLIB::ZLIB ALIAS zlib)
        endif ()
    endif ()

    # zlib.h is generated into the build dir (zconf.h) plus the source dir.
    if (TARGET zlibstatic)
        target_include_directories(zlibstatic PUBLIC
                $<BUILD_INTERFACE:${ZLIB_DIR}>
                $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/third_party/zlib>
        )
    endif ()
endif ()

##### zlib end

##### Brotli end

##### Jansson start

set(JANSSON_DIR ${PROJECT_SOURCE_DIR}/third_party/jansson)

# check if Jansson exists, if not, download it
if (NOT EXISTS ${JANSSON_DIR})
    message(STATUS "Jansson not found, downloading...")
    execute_process(
            COMMAND git clone https://github.com/akheron/jansson.git ${JANSSON_DIR}
            RESULT_VARIABLE GIT_RESULT
    )
    if (NOT GIT_RESULT EQUAL 0)
        message(FATAL_ERROR "Failed to download Jansson")
    endif ()
endif ()

# Build Jansson as a static library
set(JANSSON_BUILD_SHARED_LIBS OFF CACHE BOOL "Build shared libraries" FORCE)
set(JANSSON_BUILD_EXECUTABLES OFF CACHE BOOL "Build executables" FORCE)
set(JANSSON_BUILD_TESTS OFF CACHE BOOL "Build tests" FORCE)
set(JANSSON_BUILD_DOCS OFF CACHE BOOL "Build documentation" FORCE)
set(JANSSON_COVERAGE OFF CACHE BOOL "Enable coverage" FORCE)
set(JANSSON_EXAMPLES OFF CACHE BOOL "Build examples" FORCE)
set(JANSSON_WITHOUT_TESTS ON CACHE BOOL "Disable tests" FORCE)

# add Jansson as a subdirectory
add_subdirectory(${JANSSON_DIR})

##### Jansson end

##### zst start

set(ZSTD_DIR ${PROJECT_SOURCE_DIR}/third_party/zstd)

# Check if Zstd exists, if not, download it
if (NOT EXISTS ${ZSTD_DIR})
    message(STATUS "Zstd not found, downloading...")
    execute_process(
            COMMAND git clone https://github.com/facebook/zstd.git ${ZSTD_DIR}
            RESULT_VARIABLE GIT_RESULT
    )
    if (NOT GIT_RESULT EQUAL 0)
        message(FATAL_ERROR "Failed to download Zstd")
    endif ()
endif ()

# Build Zstd as a static library
set(ZSTD_BUILD_STATIC ON CACHE BOOL "Build static library" FORCE)
set(ZSTD_BUILD_SHARED OFF CACHE BOOL "Build shared library" FORCE)
set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "Build programs" FORCE)
set(ZSTD_BUILD_TESTS OFF CACHE BOOL "Build tests" FORCE)
set(ZSTD_BUILD_CONTRIB OFF CACHE BOOL "Build contrib" FORCE)

# Add Zstd cmake subdirectory
add_subdirectory(${ZSTD_DIR}/build/cmake)

##### zstd end

##### ZLOG start

#set(ZLOG_DIR ${PROJECT_SOURCE_DIR}/third_party/zlog)
#
#if (NOT EXISTS ${ZLOG_DIR})
#    message(STATUS "zlog not found. Downloading...")
#    execute_process(
#        COMMAND git clone https://github.com/HardySimpson/zlog.git ${ZLOG_DIR}
#        RESULT_VARIABLE GIT_RESULT
#    )
#    if (NOT GIT_RESULT EQUAL 0)
#        message(FATAL_ERROR "Failed to download zlog")
#    endif ()
#endif ()
## configure BoringSSL for static linking
#set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build shared libraries" FORCE)
#add_subdirectory(${ZLOG_DIR})
#
#configure_file(${PROJECT_SOURCE_DIR}/src/config/zlog.conf ${LIB_PATH}/zlog.conf COPYONLY)

##### ZLOG end

########## third parties ####################################

# add sub-directories
add_subdirectory(src)
add_subdirectory(tests)