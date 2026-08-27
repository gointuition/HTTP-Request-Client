cmake_minimum_required(VERSION 3.29)
# ASM is enabled here (not just C) so the GAS assembler rule is set up once at the
# top level using the already-identified GNU compiler. Third parties that turn on
# assembly in a nested project() scope (e.g. zstd's huf_decompress_amd64.S) then
# inherit a working CMAKE_ASM_COMPILE_OBJECT; otherwise CMake's ASM detection can
# misfire under Ninja/MinGW and fail the generate step with
# "required internal CMake variable not set: CMAKE_ASM_COMPILE_OBJECT".
project(Http VERSION 1.0.0 LANGUAGES C ASM)

set(CMAKE_C_STANDARD 17)

set(CMAKE_MINIMUM_REQUIRED_VERSION 3.29)

########## API / soname versioning ##########################
# SOVERSION (major) — bump on ABI-breaking changes
# VERSION (major.minor.patch) — full library version
set(HTTPCLIENT_VERSION_MAJOR ${PROJECT_VERSION_MAJOR})
set(HTTPCLIENT_VERSION_MINOR ${PROJECT_VERSION_MINOR})
set(HTTPCLIENT_VERSION_PATCH ${PROJECT_VERSION_PATCH})
set(HTTPCLIENT_SOVERSION ${PROJECT_VERSION_MAJOR})
############################################################

# Generate Version.h from template (single source of truth: project VERSION)
configure_file(
        ${PROJECT_SOURCE_DIR}/include/Version.h.in
        ${CMAKE_BINARY_DIR}/generated/Version.h
        @ONLY
)

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
set(HTTP_CLIENT httpclient)

# force static linking for all libraries
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build shared libraries" FORCE)

########## third parties ####################################

# Apply local patches from patches/<lib>/*.patch to a third-party source tree.
# Patches are applied in filename order (use 0001-, 0002-, ... prefixes).
# A patch that `git apply --reverse --check` accepts is already applied and skipped,
# so re-running cmake never double-applies.
#
# --ignore-whitespace lets a LF patch apply onto a checkout that Git for Windows
# converted to CRLF (default core.autocrlf=true); without it the LF context lines
# fail to match the CRLF source and `git apply` reports "patch does not apply".
function(apply_third_party_patches LIB_NAME LIB_DIR)
    file(GLOB PATCH_FILES ${PROJECT_SOURCE_DIR}/patches/${LIB_NAME}/*.patch)
    list(SORT PATCH_FILES)
    foreach (PATCH_FILE ${PATCH_FILES})
        execute_process(
                COMMAND git apply --reverse --check --ignore-whitespace ${PATCH_FILE}
                WORKING_DIRECTORY ${LIB_DIR}
                RESULT_VARIABLE PATCH_ALREADY_APPLIED
                ERROR_QUIET
        )
        if (PATCH_ALREADY_APPLIED EQUAL 0)
            message(STATUS "Patch already applied, skipping: ${PATCH_FILE}")
            continue()
        endif ()
        execute_process(
                COMMAND git apply --ignore-whitespace ${PATCH_FILE}
                WORKING_DIRECTORY ${LIB_DIR}
                RESULT_VARIABLE PATCH_RESULT
        )
        if (NOT PATCH_RESULT EQUAL 0)
            message(FATAL_ERROR "Failed to apply patch: ${PATCH_FILE}")
        endif ()
        message(STATUS "Applied patch: ${PATCH_FILE}")
    endforeach ()
endfunction()

# Clone a third-party repo at a fixed, pinned revision for reproducible builds.
# REF may be a tag, branch, or commit hash. A valid existing checkout is reused
# as-is (so local patches survive); an absent or partial directory is (re)cloned.
function(clone_third_party_pinned NAME URL REF DIR)
    # A valid, existing git checkout is treated as already provisioned. A leftover
    # directory without a .git (e.g. from an interrupted/failed clone) is NOT a
    # valid tree — trusting it would make the later patch step fail with
    # "No such file or directory" — so remove it and re-clone.
    if (EXISTS ${DIR})
        if (EXISTS ${DIR}/.git)
            return()
        endif ()
        message(WARNING "${NAME}: ${DIR} exists but is not a git checkout (partial clone?); removing and re-cloning")
        file(REMOVE_RECURSE ${DIR})
    endif ()
    message(STATUS "${NAME} not found, cloning at pinned revision ${REF}...")
    # --config core.autocrlf=false/core.eol=lf is persisted into the new repo
    # before checkout, so the working tree keeps LF endings on Windows too. This
    # keeps the LF patches in patches/ applying cleanly (Git for Windows would
    # otherwise check out CRLF and break `git apply`).
    execute_process(
            COMMAND git clone --config core.autocrlf=false --config core.eol=lf ${URL} ${DIR}
            RESULT_VARIABLE GIT_CLONE_RESULT
    )
    if (NOT GIT_CLONE_RESULT EQUAL 0)
        # Drop any partial clone so the next configure retries from scratch.
        file(REMOVE_RECURSE ${DIR})
        message(FATAL_ERROR "Failed to clone ${NAME} from ${URL} — check network connectivity to the host and re-run CMake")
    endif ()
    execute_process(
            COMMAND git checkout --quiet --detach ${REF}
            WORKING_DIRECTORY ${DIR}
            RESULT_VARIABLE GIT_CHECKOUT_RESULT
    )
    if (NOT GIT_CHECKOUT_RESULT EQUAL 0)
        file(REMOVE_RECURSE ${DIR})
        message(FATAL_ERROR "Failed to check out pinned revision ${REF} for ${NAME}")
    endif ()
    message(STATUS "${NAME} pinned to ${REF}")
endfunction()

########## Pinned third-party revisions ####################
# Fixed source versions for reproducible builds. Bump these deliberately (and
# re-verify the local patches afterwards) instead of tracking upstream HEAD.
set(BORINGSSL_GIT_REF 9134af991b3440140812a0c1bd466cd92b9e19ec) # 0.20250818.0 series
set(BROTLI_GIT_REF    3efb30f96ba50389c4a2fcd9f884b7dbfbadebae) # v1.1.0 (bcr.0 + 6)
set(ZLIB_GIT_REF      v1.3.1)                                   # latest stable release
set(JANSSON_GIT_REF   v2.15.1)                                  # v2.15.1 series
set(ZSTD_GIT_REF      v1.5.7)                                   # latest stable release tag
############################################################

##### Boringssl start

set(BORINGSSL_DIR ${PROJECT_SOURCE_DIR}/third_party/boringssl)

# Clone at the pinned revision if the source tree is absent.
clone_third_party_pinned(BoringSSL https://github.com/google/boringssl.git ${BORINGSSL_GIT_REF} ${BORINGSSL_DIR})

# apply local patches (patches/boringssl/*.patch) before building
apply_third_party_patches(boringssl ${BORINGSSL_DIR})

# add BoringSSL as a subdirectory
# Only the crypto/ssl libraries are needed; skip building BoringSSL's own
# test suite (crypto_test/ssl_test), which fails to compile under some
# toolchains (e.g. MinGW-w64) and is irrelevant to this project.
set(BUILD_TESTING OFF CACHE BOOL "Build the testing tree" FORCE)
# NOTE: OPENSSL_SMALL is intentionally left OFF. Its compact ML-KEM
# implementation breaks SSL_connect when the post-quantum group
# X25519MLKEM768 is advertised (internal error in the EC routines), so we
# keep the full, optimized BoringSSL build to preserve the PQ fingerprint.
add_subdirectory(${BORINGSSL_DIR})

##### Boringssl end

##### Brotli start

set(BROTLI_DIR ${PROJECT_SOURCE_DIR}/third_party/brotli)

clone_third_party_pinned(Brotli https://github.com/google/brotli.git ${BROTLI_GIT_REF} ${BROTLI_DIR})

add_subdirectory(${BROTLI_DIR})

##### zlib start

# Prefer the system zlib on macOS/Linux; on Windows always build from source
# to avoid a runtime dependency on zlib1.dll (MSYS2's shared zlib).
if (WIN32)
    set(ZLIB_FOUND FALSE)
else ()
    find_package(ZLIB QUIET)
endif ()

if (NOT ZLIB_FOUND)
    message(STATUS "System zlib not found, downloading and building from source...")

    set(ZLIB_DIR ${PROJECT_SOURCE_DIR}/third_party/zlib)

    clone_third_party_pinned(zlib https://github.com/madler/zlib.git ${ZLIB_GIT_REF} ${ZLIB_DIR})

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

clone_third_party_pinned(Jansson https://github.com/akheron/jansson.git ${JANSSON_GIT_REF} ${JANSSON_DIR})

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

clone_third_party_pinned(Zstd https://github.com/facebook/zstd.git ${ZSTD_GIT_REF} ${ZSTD_DIR})

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