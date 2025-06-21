cmake_minimum_required(VERSION 3.30)
project(EasyFCGI
        VERSION 1.3
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
target_include_directories(${PROJECT_NAME} PUBLIC "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>")
set_target_properties(
        ${PROJECT_NAME} PROPERTIES
        POSITION_INDEPENDENT_CODE ON
        CXX_STANDARD 26
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS OFF
)

