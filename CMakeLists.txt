cmake_minimum_required(VERSION 3.14)
project(vNPUSimulator VERSION 1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

# 加入安全編譯選項
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Werror -Wpedantic -fno-omit-frame-pointer")

include(FetchContent)
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG        release-1.12.1
)
FetchContent_MakeAvailable(googletest)

# 修正路徑 src/ -> common/
add_library(vnpu_utils common/vnpu_logger.cpp)
target_include_directories(vnpu_utils PUBLIC ${CMAKE_SOURCE_DIR}/include)

add_executable(firmware firmware/firmware.cpp)
target_include_directories(firmware PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(firmware vnpu_utils pthread)

add_executable(driver client/driver_client.cpp)
target_include_directories(driver PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(driver vnpu_utils)