cmake_minimum_required(VERSION 3.5)
project(RN-Praxis)
set(CMAKE_C_STANDARD 11)

# Enable compile commands for clang tooling
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Add include directory
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/includes)

# Define source files
set(SOURCES
    src/webserver.c
    src/http.c
    src/util.c
    src/data.c
    src/dht.c
)

# Create executable
add_executable(webserver ${SOURCES})
target_compile_options(webserver PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(webserver PRIVATE -lm)

# Packaging
set(CPACK_SOURCE_GENERATOR "TGZ")
set(CPACK_SOURCE_IGNORE_FILES ${CMAKE_BINARY_DIR} /\\..*$)
set(CPACK_VERBATIM_VARIABLES YES)
include(CPack)
