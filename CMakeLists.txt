cmake_minimum_required(VERSION 3.20)
project(glfw_vulkan LANGUAGES C CXX)

include(FetchContent)

# GLFW
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG 3.4
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(glfw)

# Vulkan-Headers
FetchContent_Declare(
    vulkan_headers
    GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
    GIT_TAG vulkan-sdk-1.4.350.0
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(vulkan_headers)

# volk
FetchContent_Declare(
    volk
    GIT_REPOSITORY https://github.com/zeux/volk.git
    GIT_TAG vulkan-sdk-1.4.350.0
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(volk)

# GLM
FetchContent_Declare(
    glm
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG 1.0.1
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(glm)

# Dear ImGui
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.91.6
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(imgui)

# glslang — GLSL → SPIR-V compiler, built as a build-time tool
set(BUILD_EXTERNAL OFF CACHE BOOL "" FORCE)
set(ENABLE_SPVREMAPPER OFF CACHE BOOL "" FORCE)
set(ENABLE_GLSLANG_INSTALL OFF CACHE BOOL "" FORCE)
set(SKIP_GLSLANG_INSTALL ON CACHE BOOL "" FORCE)
set(ENABLE_HLSL OFF CACHE BOOL "" FORCE)
set(ENABLE_OPT OFF CACHE BOOL "" FORCE)
set(ENABLE_GLSLANG_BINARIES ON CACHE BOOL "" FORCE)

FetchContent_Declare(
    glslang
    GIT_REPOSITORY https://github.com/KhronosGroup/glslang.git
    GIT_TAG 16.3.0
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(glslang)

# --- Shader compilation ---
set(SHADER_DIR ${CMAKE_SOURCE_DIR}/shaders)
set(SHADER_GEN_DIR ${CMAKE_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${SHADER_GEN_DIR})

set(VERT_H ${SHADER_GEN_DIR}/vert_spv.h)
set(FRAG_H ${SHADER_GEN_DIR}/frag_spv.h)
add_custom_command(
    OUTPUT ${VERT_H} ${FRAG_H}
    DEPENDS ${SHADER_DIR}/shader.vert ${SHADER_DIR}/shader.frag glslang-standalone
    COMMAND $<TARGET_FILE:glslang-standalone> -V ${SHADER_DIR}/shader.vert --variable-name vert_spv -o ${VERT_H}
    COMMAND $<TARGET_FILE:glslang-standalone> -V ${SHADER_DIR}/shader.frag --variable-name frag_spv -o ${FRAG_H}
    COMMENT "Compiling GLSL shaders -> SPIR-V headers"
)

add_custom_target(shaders ALL DEPENDS ${VERT_H} ${FRAG_H})

# --- Executable ---
add_executable(glfw_vulkan
    src/main.cpp
    src/vk_context.cpp
    src/imgui_layer.cpp
    src/renderer.cpp

    ${volk_SOURCE_DIR}/volk.c
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp
)

target_include_directories(glfw_vulkan PRIVATE
    src
    ${SHADER_GEN_DIR}
    ${vulkan_headers_SOURCE_DIR}/include
    ${volk_SOURCE_DIR}
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)
target_compile_definitions(glfw_vulkan PRIVATE
    IMGUI_IMPL_VULKAN_USE_VOLK
    GLM_FORCE_DEPTH_ZERO_TO_ONE # Vulkan clip space, needed for ortho/perspective
)
target_link_libraries(glfw_vulkan PRIVATE glfw glm::glm)
add_dependencies(glfw_vulkan shaders)
