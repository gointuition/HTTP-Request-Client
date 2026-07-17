cmake_minimum_required(VERSION ${CMAKE_MINIMUM_REQUIRED_VERSION})
project(src)
# find source files
file(GLOB_RECURSE SRC ./*.c)
# filter out DaemonByMemShare.c
#list(FILTER SRC EXCLUDE REGEX ".*DeamonByMemShare\\.c$")
# find include headers files
include_directories(${HEADER_PATH})

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
target_link_libraries(${HTTP2_CLIENT} PRIVATE
        crypto
        ssl
        brotlicommon
        brotlidec
        brotlienc
        ZLIB::ZLIB
        libzstd_static
        jansson
#        zlog
)