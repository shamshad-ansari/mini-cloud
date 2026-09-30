cmake_minimum_required(VERSION 3.20)

project(mini_cloud LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(mini_cloud_resources
  src/resources.cpp
  src/scheduler.cpp
)
target_include_directories(mini_cloud_resources PUBLIC include)
target_compile_features(mini_cloud_resources PUBLIC cxx_std_23)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(mini_cloud_resources PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

add_executable(resource_admission_tracer src/main.cpp)
target_link_libraries(resource_admission_tracer PRIVATE mini_cloud_resources)

add_library(mini_cloud_runtime src/process_supervisor.cpp)
target_include_directories(mini_cloud_runtime PUBLIC include)
target_compile_features(mini_cloud_runtime PUBLIC cxx_std_23)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(mini_cloud_runtime PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

add_executable(process_supervision_tracer src/process_supervision_main.cpp)
target_link_libraries(process_supervision_tracer PRIVATE mini_cloud_runtime)

add_library(mini_cloud_protocol src/worker_protocol.cpp)
target_include_directories(mini_cloud_protocol PUBLIC include)
target_compile_features(mini_cloud_protocol PUBLIC cxx_std_23)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(mini_cloud_protocol PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

add_executable(mini_cloud_controller src/controller_main.cpp)
target_link_libraries(mini_cloud_controller PRIVATE mini_cloud_protocol mini_cloud_resources)
add_executable(mini_cloud_worker src/worker_agent_main.cpp)
target_link_libraries(mini_cloud_worker PRIVATE mini_cloud_protocol mini_cloud_runtime)

enable_testing()
add_executable(resource_tests tests/resources_test.cpp)
target_link_libraries(resource_tests PRIVATE mini_cloud_resources)
add_test(NAME resource_tests COMMAND resource_tests)

add_executable(scheduler_tests tests/scheduler_test.cpp)
target_link_libraries(scheduler_tests PRIVATE mini_cloud_resources)
add_test(NAME scheduler_tests COMMAND scheduler_tests)

add_executable(runtime_fixture tests/runtime_fixture.cpp)
add_executable(process_supervisor_tests tests/process_supervisor_test.cpp)
target_link_libraries(process_supervisor_tests PRIVATE mini_cloud_runtime)
add_test(NAME process_supervisor_tests COMMAND process_supervisor_tests $<TARGET_FILE:runtime_fixture>)

add_executable(worker_protocol_tests tests/worker_protocol_test.cpp)
target_link_libraries(worker_protocol_tests PRIVATE mini_cloud_protocol)
add_test(NAME worker_protocol_tests COMMAND worker_protocol_tests)

add_executable(controller_worker_integration_tests tests/controller_worker_integration_test.cpp)
target_link_libraries(controller_worker_integration_tests PRIVATE mini_cloud_protocol)
add_test(NAME controller_worker_integration_tests COMMAND controller_worker_integration_tests
  $<TARGET_FILE:mini_cloud_controller> $<TARGET_FILE:mini_cloud_worker>)
