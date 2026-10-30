cmake_minimum_required(VERSION 3.20)

project(OC_Lab3 C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

add_executable(parent parent.c shared.h)
add_executable(child child.c shared.h)

target_link_libraries(parent PRIVATE Threads::Threads rt)
target_link_libraries(child PRIVATE Threads::Threads rt)

add_dependencies(parent child)

set_target_properties(parent child PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
)
