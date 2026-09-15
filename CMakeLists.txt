cmake_minimum_required(VERSION 3.20)

project(mini_cloud LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(mini_cloud_resources src/resources.cpp)
target_include_directories(mini_cloud_resources PUBLIC include)
target_compile_features(mini_cloud_resources PUBLIC cxx_std_23)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(mini_cloud_resources PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

add_executable(resource_admission_tracer src/main.cpp)
target_link_libraries(resource_admission_tracer PRIVATE mini_cloud_resources)

enable_testing()
add_executable(resource_tests tests/resources_test.cpp)
target_link_libraries(resource_tests PRIVATE mini_cloud_resources)
add_test(NAME resource_tests COMMAND resource_tests)
