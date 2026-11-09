cmake_minimum_required(VERSION 3.21)
project(UbiFsExtract VERSION 1.0.0 LANGUAGES CXX)

# FarManager/far/plugin.hpp is an internal template: this is the generated public header.
set(FAR_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../FarManager/plugins/common/unicode" CACHE PATH "Directory containing FAR's plugin.hpp")

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")

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
target_compile_options(fscore PUBLIC /W4 /permissive- /utf-8 /EHsc)

add_executable(ubifsx src/ToolMain.cpp)
target_link_libraries(ubifsx PRIVATE fscore)

add_executable(test_decompress tests/test_decompress.cpp)
target_link_libraries(test_decompress PRIVATE fscore)

if(EXISTS "${FAR_SDK_DIR}/plugin.hpp")
    add_library(UbiFsExtract MODULE src/Plugin.cpp src/Plugin.def)
    target_include_directories(UbiFsExtract PRIVATE "${FAR_SDK_DIR}")
    target_link_libraries(UbiFsExtract PRIVATE fscore)
else()
    message(WARNING "plugin.hpp was not found in ${FAR_SDK_DIR}: skipping the FAR plugin")
endif()
