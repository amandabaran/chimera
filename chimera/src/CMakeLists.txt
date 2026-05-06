cmake_minimum_required(VERSION 3.9)
project(chimera CXX)

include(${CMAKE_BINARY_DIR}/setup.cmake)
dory_setup_cmake()

add_executable(chimera ${HEADER_TIDER} main.cpp)

target_link_libraries(chimera ${CONAN_LIBS})
