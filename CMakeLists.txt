cmake_minimum_required(VERSION 3.22.1)

project(AlsaPlugin C CXX)

message("Building ${PROJECT_NAME}")

set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_VERBOSE_MAKEFILE ON)

set (OBOE_DIR ./oboe)
add_subdirectory (${OBOE_DIR} ./oboe)
include_directories (${OBOE_DIR}/include)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -O2")

MESSAGE(STATUS "Compiler options: ${CMAKE_C_FLAGS}")

add_library(asound_module_pcm_android_alsa SHARED module_pcm_android_alsa.cpp)

target_compile_definitions(asound_module_pcm_android_alsa PRIVATE PIC)
target_compile_options(asound_module_pcm_android_alsa PRIVATE -fPIC)
target_link_libraries(asound_module_pcm_android_alsa asound oboe)
