cmake_minimum_required(VERSION 3.0)
project(LIPS VERSION 0.1)

add_library(lips STATIC "lips.c" "lips.h")

target_include_directories(lips PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>)

set_target_properties(lips PROPERTIES
  C_STANDARD 99
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)

set(ENABLE_ASAN 1)

if (ENABLE_ASAN)
  # enable all warnings from compiler
  target_compile_options(lips PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX>                         # MSVC
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra -Wpedantic -fsanitize=address -fno-omit-frame-pointer> # g++ or clang
  )

  target_link_options(lips PUBLIC
    -fsanitize=address -fno-omit-frame-pointer -lrt)
else ()
  # enable all warnings from compiler
  target_compile_options(lips PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX>                         # MSVC
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra -Wpedantic> # g++ or clang
  )
endif()

add_executable(test main.cpp)
target_link_libraries(test PUBLIC lips)

add_executable(repl repl.cpp)
target_link_libraries(repl PUBLIC lips)
