cmake_minimum_required(VERSION 3.24)
project(RetooledXeSS LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

# Only the Vulkan headers are used. Unity supplies the loader at runtime. They come from the Vulkan SDK, or from a
# checkout of KhronosGroup/Vulkan-Headers whose include folder is passed as Vulkan_INCLUDE_DIR.
find_path(Vulkan_INCLUDE_DIR vulkan/vulkan.h HINTS "$ENV{VULKAN_SDK}/Include" REQUIRED)

add_library(RetooledXeSS SHARED
    src/Plugin.cpp
    src/XessLibrary.cpp
    src/BackendD3D12.cpp
    src/BackendVulkan.cpp
    src/BackendD3D11.cpp
    src/BackendD3D11On12.cpp
)

# The SDK runtime is loaded at runtime, so only its headers are needed here.
target_include_directories(RetooledXeSS PRIVATE
    unity
    xess/inc
    ${Vulkan_INCLUDE_DIR}
)
target_compile_definitions(RetooledXeSS PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX VK_NO_PROTOTYPES)
set_target_properties(RetooledXeSS PROPERTIES OUTPUT_NAME Retooled.XeSS.Native)

install(TARGETS RetooledXeSS RUNTIME DESTINATION .)
