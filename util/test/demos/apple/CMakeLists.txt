cmake_minimum_required(VERSION 3.20)

project(renderdoc_metal_trace_fixture LANGUAGES CXX OBJCXX)

add_executable(metal_trace_fixture metal_trace_fixture.mm)
target_compile_features(metal_trace_fixture PRIVATE cxx_std_17)
target_compile_options(metal_trace_fixture PRIVATE -Wall -Wextra -Werror)
target_link_libraries(metal_trace_fixture PRIVATE
  "-framework AppKit"
  "-framework Metal"
  "-framework QuartzCore")

set_target_properties(metal_trace_fixture PROPERTIES
  MACOSX_BUNDLE FALSE
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
