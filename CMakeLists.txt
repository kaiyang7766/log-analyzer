cmake_minimum_required(VERSION 3.20)

project(log-analyzer VERSION 0.2.0 LANGUAGES CXX)

option(LOG_ANALYZER_BUILD_TESTS "Build the test suite" ON)
option(LOG_ANALYZER_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

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
  set(LOG_ANALYZER_SIMDJSON_TARGET simdjson_fallback)
elseif(TARGET simdjson::simdjson)
  set(LOG_ANALYZER_SIMDJSON_TARGET simdjson::simdjson)
elseif(TARGET simdjson)
  set(LOG_ANALYZER_SIMDJSON_TARGET simdjson)
else()
  message(FATAL_ERROR "The simdjson package was found but did not export a known target")
endif()

add_library(log_analyzer_core
  src/analyzer.cpp
  src/config.cpp
  src/formats.cpp
  src/input.cpp
  src/normalize.cpp
  src/output.cpp)
target_include_directories(log_analyzer_core PUBLIC include)
target_link_libraries(log_analyzer_core PUBLIC ${LOG_ANALYZER_SIMDJSON_TARGET} Threads::Threads)
target_compile_options(log_analyzer_core PRIVATE -Wall -Wextra -Wpedantic)

add_executable(log-analyzer src/main.cpp)
target_link_libraries(log-analyzer PRIVATE log_analyzer_core)

if(LOG_ANALYZER_ENABLE_SANITIZERS AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  foreach(target log_analyzer_core log-analyzer)
    target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(${target} PRIVATE -fsanitize=address,undefined)
  endforeach()
endif()

include(CTest)
if(BUILD_TESTING AND LOG_ANALYZER_BUILD_TESTS)
  add_executable(log_analyzer_tests tests/test_main.cpp)
  target_link_libraries(log_analyzer_tests PRIVATE log_analyzer_core)
  target_compile_options(log_analyzer_tests PRIVATE -Wall -Wextra -Wpedantic)
  if(LOG_ANALYZER_ENABLE_SANITIZERS AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(log_analyzer_tests PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(log_analyzer_tests PRIVATE -fsanitize=address,undefined)
  endif()
  add_test(NAME log_analyzer_tests COMMAND log_analyzer_tests)
endif()

install(TARGETS log-analyzer RUNTIME DESTINATION bin)
