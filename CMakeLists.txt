cmake_minimum_required(VERSION 3.25)
project(ZZZ_DX12_AO LANGUAGES CXX)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "This project targets 64-bit Windows.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(RESHADE_INCLUDE_DIR "" CACHE PATH "Directory containing ReShade 6.7.3 reshade.hpp")
if(NOT RESHADE_INCLUDE_DIR)
    include(FetchContent)
    FetchContent_Declare(reshade
        URL https://codeload.github.com/crosire/reshade/tar.gz/4a50d1eddace85734871d91792ff214f13f66c01
        URL_HASH SHA256=a9353b0766274ab9fba1c8aa440d88ab8ec4ab2f363530b71ca4ccdd01a06e85
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        SOURCE_SUBDIR unused)
    FetchContent_MakeAvailable(reshade)
    set(RESHADE_INCLUDE_DIR "${reshade_SOURCE_DIR}/include")
endif()

add_library(zzz_ao_capture MODULE src/capture.cpp)
target_include_directories(zzz_ao_capture SYSTEM PRIVATE "${RESHADE_INCLUDE_DIR}")
target_compile_definitions(zzz_ao_capture PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(zzz_ao_capture PRIVATE user32)
set_target_properties(zzz_ao_capture PROPERTIES
    PREFIX "" SUFFIX ".addon64"
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

option(ZZZ_AO_BUILD_COMPUTE "Build the XeGTAO backend and its DXIL shaders" ON)
if(NOT ZZZ_AO_BUILD_COMPUTE)
    return()
endif()

find_program(DXC_EXECUTABLE NAMES dxc HINTS "$ENV{WindowsSdkVerBinPath}/x64" REQUIRED)
set(shader_outputs)
foreach(entry IN ITEMS Prefilter HorizonLow HorizonMedium HorizonHigh HorizonUltra Denoise DenoiseFinal)
    set(output "${CMAKE_CURRENT_BINARY_DIR}/shaders/${entry}.dxil")
    add_custom_command(OUTPUT "${output}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/shaders"
        COMMAND "${DXC_EXECUTABLE}" -T cs_6_0 -E "${entry}" -O3 -Ges -WX
            -Fo "${output}" "${CMAKE_CURRENT_SOURCE_DIR}/shaders/xegtao.hlsl"
        DEPENDS shaders/xegtao.hlsl third_party/xegtao/XeGTAO.h third_party/xegtao/XeGTAO.hlsli
        VERBATIM)
    list(APPEND shader_outputs "${output}")
endforeach()
add_custom_target(zzz_ao_shaders ALL DEPENDS ${shader_outputs})

add_library(zzz_ao_dx12 STATIC src/xegtao_dx12.cpp)
target_include_directories(zzz_ao_dx12 PUBLIC src PRIVATE third_party/xegtao)
target_compile_definitions(zzz_ao_dx12 PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(zzz_ao_dx12 PUBLIC d3d12)
add_dependencies(zzz_ao_dx12 zzz_ao_shaders)
