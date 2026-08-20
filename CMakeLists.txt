cmake_minimum_required(VERSION 3.20)

project(logscope VERSION 0.2.0 LANGUAGES CXX)

option(LOGSCOPE_BUILD_TESTS "Build the test suite" ON)
option(LOGSCOPE_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)
find_package(simdjson CONFIG QUIET)
if(NOT simdjson_FOUND)
  find_path(SIMDJSON_INCLUDE_DIR simdjson.h)
  find_library(SIMDJSON_LIBRARY NAMES simdjson simdjson_static)
  if(NOT SIMDJSON_INCLUDE_DIR OR NOT SIMDJSON_LIBRARY)
    message(FATAL_ERROR "simdjson was not found. Install it (for example: brew install simdjson or apt install libsimdjson-dev).")
  endif()
  add_library(simdjson_fallback UNKNOWN IMPORTED)
  set_target_properties(simdjson_fallback PROPERTIES
    IMPORTED_LOCATION "${SIMDJSON_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${SIMDJSON_INCLUDE_DIR}")
  set(LOGSCOPE_SIMDJSON_TARGET simdjson_fallback)
elseif(TARGET simdjson::simdjson)
  set(LOGSCOPE_SIMDJSON_TARGET simdjson::simdjson)
elseif(TARGET simdjson)
  set(LOGSCOPE_SIMDJSON_TARGET simdjson)
else()
  message(FATAL_ERROR "The simdjson package was found but did not export a known target")
endif()

add_library(logscope_core
  src/analyzer.cpp
  src/config.cpp
  src/formats.cpp
  src/input.cpp
  src/normalize.cpp
  src/output.cpp)
target_include_directories(logscope_core PUBLIC include)
target_link_libraries(logscope_core PUBLIC ${LOGSCOPE_SIMDJSON_TARGET} Threads::Threads)
target_compile_options(logscope_core PRIVATE -Wall -Wextra -Wpedantic)

add_executable(logscope src/main.cpp)
target_link_libraries(logscope PRIVATE logscope_core)

if(LOGSCOPE_ENABLE_SANITIZERS AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  foreach(target logscope_core logscope)
    target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(${target} PRIVATE -fsanitize=address,undefined)
  endforeach()
endif()

include(CTest)
if(BUILD_TESTING AND LOGSCOPE_BUILD_TESTS)
  add_executable(logscope_tests tests/test_main.cpp)
  target_link_libraries(logscope_tests PRIVATE logscope_core)
  target_compile_options(logscope_tests PRIVATE -Wall -Wextra -Wpedantic)
  if(LOGSCOPE_ENABLE_SANITIZERS AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(logscope_tests PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(logscope_tests PRIVATE -fsanitize=address,undefined)
  endif()
  add_test(NAME logscope_tests COMMAND logscope_tests)
endif()

install(TARGETS logscope RUNTIME DESTINATION bin)
