cmake_minimum_required(VERSION 3.14)
project(vNPUSimulator VERSION 1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Werror -Wpedantic")

add_executable(firmware firmware.cpp)
target_include_directories(firmware PRIVATE ${CMAKE_SOURCE_DIR}/include)

add_executable(driver client/driver_client.cpp)
target_include_directories(driver PRIVATE ${CMAKE_SOURCE_DIR}/include)

include(FetchContent)
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG        release-1.12.1
)
FetchContent_MakeAvailable(googletest)

add_executable(vnpu_tests tests/test_ringbuffer.cpp)
target_link_libraries(vnpu_tests gtest_main)

# 建立一個共享的 library
add_library(vnpu_utils src/vnpu_logger.cpp)
target_include_directories(vnpu_utils PUBLIC ${CMAKE_SOURCE_DIR}/include)

# Firmware 連結 logger
add_executable(firmware firmware.cpp)
target_link_libraries(firmware vnpu_utils)

# Driver 連結 logger
add_executable(driver client/driver_client.cpp)
target_link_libraries(driver vnpu_utils)