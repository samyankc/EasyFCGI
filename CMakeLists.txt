cmake_minimum_required(VERSION 3.30)
project(EasyFCGI
        VERSION 1.1
        LANGUAGES CXX)

add_library(EasyFCGI OBJECT src/EasyFCGI.cpp)
add_library(EasyFCGI::EasyFCGI ALIAS EasyFCGI)

# include(FetchContent)
# FetchContent_Declare(
#   glaze
#   GIT_REPOSITORY https://github.com/stephenberry/glaze.git
#   GIT_TAG main
#   GIT_SHALLOW TRUE
# )
# FetchContent_MakeAvailable(glaze)
# add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../glaze ${CMAKE_CURRENT_BINARY_DIR}/../glaze)
# target_compile_definitions(glaze_glaze INTERFACE GLZ_NULL_TERMINATED=false)

# target_link_libraries(${PROJECT_NAME} PUBLIC glaze::glaze)
target_link_libraries(${PROJECT_NAME} PUBLIC -lfcgi)
# target_include_directories(${PROJECT_NAME} INTERFACE "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>")
# target_include_directories(${PROJECT_NAME} PRIVATE "${PROJECT_SOURCE_DIR}/include")
target_include_directories(${PROJECT_NAME} PUBLIC "${PROJECT_SOURCE_DIR}/include")
target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_26)
target_compile_options(${PROJECT_NAME} PRIVATE -fPIC)