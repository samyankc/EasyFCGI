cmake_minimum_required(VERSION 3.30)
project(EasyFCGI VERSION 1.0 LANGUAGES CXX)

add_library(EasyFCGI OBJECT src/EasyFCGI.cpp)
add_library(EasyFCGI::EasyFCGI ALIAS EasyFCGI)

target_include_directories(EasyFCGI INTERFACE "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>")
target_include_directories(EasyFCGI PRIVATE "${PROJECT_SOURCE_DIR}/include")
target_link_libraries(EasyFCGI PUBLIC -lfcgi)
target_compile_features(EasyFCGI PRIVATE cxx_std_26)
target_compile_options(EasyFCGI PRIVATE -fPIC)