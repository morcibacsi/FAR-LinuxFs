cmake_minimum_required(VERSION 3.21)

# Release builds pass the version from the Git tag, for example -DLINUXFS_VERSION=1.2.3.
set(LINUXFS_VERSION "1.0.0" CACHE STRING "LinuxFS release version")
if(NOT LINUXFS_VERSION MATCHES "^(0|[1-9][0-9]*)[.](0|[1-9][0-9]*)[.](0|[1-9][0-9]*)$")
    message(FATAL_ERROR "LINUXFS_VERSION must have the form MAJOR.MINOR.PATCH")
endif()
project(LinuxFS VERSION "${LINUXFS_VERSION}" LANGUAGES CXX RC)

# FarManager/far/plugin.hpp is an internal template: this is the generated public header.
set(FAR_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../FarManager/plugins/common/unicode" CACHE PATH "Directory containing FAR's plugin.hpp")

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(output_dir "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${output_dir}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${output_dir}")

file(GENERATE OUTPUT "${output_dir}/README.md" INPUT README.md NEWLINE_STYLE UNIX)

# Stamps the version into the file's Windows version resource.
function(add_version_resource target file_name description file_type)
    set(RC_FILE_NAME "${file_name}")
    set(RC_DESCRIPTION "${description}")
    set(RC_FILE_TYPE "${file_type}")
    configure_file(src/Version.rc.in "${CMAKE_CURRENT_BINARY_DIR}/${target}_version.rc" @ONLY)
    target_sources(${target} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/${target}_version.rc")
endfunction()

add_library(fscore STATIC
    src/CachedFile.cpp
    src/Decompress.cpp
    src/Ext.cpp
    src/Extract.cpp
    src/FsImage.cpp
    src/Ubifs.cpp
)
target_include_directories(fscore PUBLIC src)
target_compile_features(fscore PUBLIC cxx_std_20)
target_compile_definitions(fscore PUBLIC UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(fscore PUBLIC $<$<COMPILE_LANGUAGE:CXX>:/W4 /permissive- /utf-8 /EHsc>)

add_executable(linuxfs_cli src/ToolMain.cpp)
target_link_libraries(linuxfs_cli PRIVATE fscore)
set_target_properties(linuxfs_cli PROPERTIES OUTPUT_NAME linuxfs)
add_version_resource(linuxfs_cli linuxfs.exe "LinuxFS command-line image reader" VFT_APP)
install(TARGETS linuxfs_cli RUNTIME DESTINATION .)

add_executable(test_decompress tests/test_decompress.cpp)
target_link_libraries(test_decompress PRIVATE fscore)

if(EXISTS "${FAR_SDK_DIR}/plugin.hpp")
    add_library(LinuxFS MODULE src/Plugin.cpp src/Plugin.def)
    target_include_directories(LinuxFS PRIVATE "${FAR_SDK_DIR}")
    target_link_libraries(LinuxFS PRIVATE fscore)
    target_compile_definitions(LinuxFS PRIVATE
        LINUXFS_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
        LINUXFS_VERSION_MINOR=${PROJECT_VERSION_MINOR}
        LINUXFS_VERSION_PATCH=${PROJECT_VERSION_PATCH}
    )
    add_version_resource(LinuxFS LinuxFS.dll "LinuxFS plugin for FAR Manager" VFT_DLL)
    install(TARGETS LinuxFS LIBRARY DESTINATION .)
else()
    message(WARNING "plugin.hpp was not found in ${FAR_SDK_DIR}: skipping the FAR plugin")
endif()

install(FILES README.md RenewalCustom.xml DESTINATION .)

set(package_arch "${CMAKE_CXX_COMPILER_ARCHITECTURE_ID}")
if(package_arch STREQUAL "X86")
    set(package_arch x86)
endif()
set(CPACK_GENERATOR ZIP)
set(CPACK_PACKAGE_NAME LinuxFS)
set(CPACK_PACKAGE_FILE_NAME "LinuxFS-${PROJECT_VERSION}-${package_arch}")
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY OFF)
include(CPack)
