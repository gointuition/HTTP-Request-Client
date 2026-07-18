cmake_minimum_required(VERSION ${CMAKE_MINIMUM_REQUIRED_VERSION})
project(src)

# find source files
file(GLOB SRC ./*.c)
# find include headers files
include_directories(${HEADER_PATH})
# link libs directories
link_directories(${LIB_PATH})
# link libs
link_libraries(${HTTP2_CLIENT})

set(EXECUTABLE_OUTPUT_PATH ${EXEC_PATH})

file(GLOB JSONFILES ./*.json)
foreach (json_file ${JSONFILES})
    get_filename_component(filename ${json_file} NAME)
    add_custom_command(
        OUTPUT ${EXECUTABLE_OUTPUT_PATH}/${filename}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${EXECUTABLE_OUTPUT_PATH}
        COMMAND ${CMAKE_COMMAND} -E copy ${json_file} ${EXECUTABLE_OUTPUT_PATH}/${filename}
        DEPENDS ${json_file}
        COMMENT "Copying ${filename} to ${EXECUTABLE_OUTPUT_PATH}"
    )
    # Collect all json output files
    list(APPEND JSON_OUTPUT_FILES ${EXECUTABLE_OUTPUT_PATH}/${filename})
endforeach ()
add_custom_target(copy_json_files ALL DEPENDS ${JSON_OUTPUT_FILES})

foreach (source_file ${SRC})
    get_filename_component(target_name ${source_file} NAME_WE)
    add_executable(${target_name} ${source_file})
    add_dependencies(${target_name} copy_json_files)
    # include third parties headers
    target_include_directories(${target_name} PRIVATE
            ${BORINGSSL_DIR}/include
            ${BROTLI_DIR}/c/include
            ${JANSSON_DIR}/src
    )
    # linked against third parties
    target_link_libraries(${target_name} PRIVATE
            crypto
            ssl
            brotlicommon
            brotlidec
            brotlienc
            ZLIB::ZLIB
            jansson
    )
endforeach ()



#set(APP_NAME http2test)
#add_executable(${APP_NAME} ${SRC})
## include BoringSSL headers
#target_include_directories(${APP_NAME} PRIVATE ${BORINGSSL_DIR}/include)
## linked against BoringSSL
#target_link_libraries(${APP_NAME} PRIVATE crypto ssl)