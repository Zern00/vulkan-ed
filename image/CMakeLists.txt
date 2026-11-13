cmake_minimum_required(VERSION 3.20)
project(image LANGUAGES CXX)

find_package(Vulkan REQUIRED)

add_executable(image src/main.cxx)
target_compile_features(image PRIVATE cxx_std_20)
set_target_properties(image PROPERTIES CXX_EXTENSIONS OFF)
target_link_libraries(image PRIVATE Vulkan::Vulkan)

find_program(GLSLC_EXECUTABLE glslc REQUIRED)
set(SHADER_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders")
foreach(shader IN ITEMS test.comp)
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
add_custom_target(image-shaders DEPENDS ${shader_binaries})
add_dependencies(image image-shaders)
target_compile_definitions(image PRIVATE
    IMAGE_SHADER_PATH="${SHADER_OUTPUT_DIR}/test.comp.spv"
)
