cmake_minimum_required(VERSION 3.16)
project(cache_server LANGUAGES C CXX ASM)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(CACHE_SERVER_USE_JEMALLOC "Link bundled jemalloc when available" OFF)

add_subdirectory(thirdparty/libco)

file(GLOB_RECURSE CACHE_CORE_SRCS CONFIGURE_DEPENDS src/*.cpp)
list(FILTER CACHE_CORE_SRCS EXCLUDE REGEX ".*/main\\.cpp$")
add_library(cache_core STATIC ${CACHE_CORE_SRCS})

target_include_directories(cache_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/cpp_util/redis/include
  ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/cpp_util/immutable_container/include
  ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/cpp_util/conn_util/include
  ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/libco
)

target_link_libraries(cache_core PUBLIC colib_static pthread dl)

if(CACHE_SERVER_USE_JEMALLOC)
  set(JEMALLOC_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/jemalloc)
  set(JEMALLOC_INCLUDE_DIR ${JEMALLOC_ROOT}/obj/include)
  set(JEMALLOC_STATIC_LIB ${JEMALLOC_ROOT}/obj/lib/libjemalloc.a)
  if(NOT EXISTS "${JEMALLOC_STATIC_LIB}")
    message(FATAL_ERROR
      "CACHE_SERVER_USE_JEMALLOC=ON requires ${JEMALLOC_STATIC_LIB}. "
      "Build jemalloc first or configure with CACHE_SERVER_USE_JEMALLOC=OFF.")
  endif()
  target_include_directories(cache_core PUBLIC ${JEMALLOC_INCLUDE_DIR})
  target_link_libraries(cache_core PUBLIC
    -Wl,--whole-archive ${JEMALLOC_STATIC_LIB} -Wl,--no-whole-archive)
endif()

file(GLOB CACHE_TEST_SRCS CONFIGURE_DEPENDS tests/*_test.cpp)
add_executable(cache_tests tests/test_harness.cpp ${CACHE_TEST_SRCS})
target_link_libraries(cache_tests PRIVATE cache_core)
target_include_directories(cache_tests PRIVATE tests)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp")
  add_executable(cache_server src/main.cpp)
  target_link_libraries(cache_server PRIVATE cache_core)
endif()

enable_testing()
add_test(NAME cache_tests COMMAND cache_tests)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/redis/run_unit_tests.sh")
  add_test(NAME redis_unit_tests
           COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/redis/run_unit_tests.sh")
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/replication_integration_test.sh")
  add_test(NAME replication_integration
           COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/replication_integration_test.sh")
endif()
