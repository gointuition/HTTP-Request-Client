cmake_minimum_required(VERSION ${CMAKE_MINIMUM_REQUIRED_VERSION})
project(src)
# find source files
file(GLOB_RECURSE SRC ./*.c)
# filter out DaemonByMemShare.c
#list(FILTER SRC EXCLUDE REGEX ".*DeamonByMemShare\\.c$")
# find include headers files
include_directories(${HEADER_PATH})

# threading library (pthread on POSIX / winpthreads on MinGW)
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

# define lib output path
set(LIBRARY_OUTPUT_PATH ${LIB_PATH_SHARED})
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${LIB_PATH_SHARED})
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${LIB_PATH_SHARED})
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${LIB_PATH_SHARED})

# generate lib
add_library(${HTTP2_CLIENT} SHARED ${SRC})

# set output name and ensure it goes to the correct directory
set_target_properties(${HTTP2_CLIENT} PROPERTIES
        OUTPUT_NAME ${HTTP2_CLIENT}
        LIBRARY_OUTPUT_DIRECTORY ${LIB_PATH_SHARED}
        ARCHIVE_OUTPUT_DIRECTORY ${LIB_PATH_SHARED}
        LIBRARY_OUTPUT_DIRECTORY_DEBUG ${LIB_PATH_SHARED}
        LIBRARY_OUTPUT_DIRECTORY_RELEASE ${LIB_PATH_SHARED}
        POSITION_INDEPENDENT_CODE ON
)

# include third parties headers
target_include_directories(${HTTP2_CLIENT} PRIVATE
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

target_link_libraries(${HTTP2_CLIENT} PRIVATE
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
target_link_libraries(${HTTP2_CLIENT} PRIVATE Threads::Threads)

# Windows: link the Winsock2 socket library
if (WIN32)
    target_link_libraries(${HTTP2_CLIENT} PRIVATE ws2_32)
endif ()