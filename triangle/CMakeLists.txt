cmake_minimum_required(VERSION 3.20)
project(triangle LANGUAGES CXX)

find_package(Vulkan REQUIRED)
find_package(SDL3 CONFIG REQUIRED)

add_executable(triangle src/main.cxx)
target_compile_features(triangle PRIVATE cxx_std_20)
set_target_properties(triangle PROPERTIES CXX_EXTENSIONS OFF)
target_link_libraries(triangle PRIVATE Vulkan::Vulkan SDL3::SDL3)

find_program(GLSLC_EXECUTABLE glslc REQUIRED)
set(SHADER_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders")
foreach(shader IN ITEMS triangle.vert triangle.frag)
    set(source "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${shader}")
    set(binary "${SHADER_OUTPUT_DIR}/${shader}.spv")
    add_custom_command(
        OUTPUT "${binary}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${SHADER_OUTPUT_DIR}"
        COMMAND "${GLSLC_EXECUTABLE}" --target-env=vulkan1.3 "${source}" -o "${binary}"
        DEPENDS "${source}"
        VERBATIM
    )
    list(APPEND shader_binaries "${binary}")
endforeach()
add_custom_target(triangle-shaders DEPENDS ${shader_binaries})
add_dependencies(triangle triangle-shaders)
target_compile_definitions(triangle PRIVATE
    VERTEX_SHADER_PATH="${SHADER_OUTPUT_DIR}/triangle.vert.spv"
    FRAGMENT_SHADER_PATH="${SHADER_OUTPUT_DIR}/triangle.frag.spv"
)
