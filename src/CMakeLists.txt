cmake_minimum_required(VERSION 3.22)
project(android-usb-speaker LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT DEFINED ALSA_ROOT)
  message(FATAL_ERROR "ALSA_ROOT must point to extracted Termux alsa-lib prefix")
endif()

add_library(asound SHARED IMPORTED)
set_target_properties(asound PROPERTIES IMPORTED_LOCATION "${ALSA_ROOT}/lib/libasound.so")

add_executable(android-usb-speaker android-usb-speaker.cpp)
target_include_directories(android-usb-speaker PRIVATE "${ALSA_ROOT}/include")
target_link_libraries(android-usb-speaker PRIVATE asound aaudio log dl)
target_compile_options(android-usb-speaker PRIVATE -O3 -ffast-math -fno-exceptions -fno-rtti -Wall -Wextra)
