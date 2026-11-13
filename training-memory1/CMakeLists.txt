cmake_minimum_required(VERSION 3.20)
project(training-memory1 LANGUAGES CXX)

find_package(Vulkan REQUIRED)

add_executable(training-memory1 src/main.cxx)
target_compile_features(training-memory1 PRIVATE cxx_std_20)
set_target_properties(training-memory1 PROPERTIES CXX_EXTENSIONS OFF)
target_link_libraries(training-memory1 PRIVATE Vulkan::Vulkan)
