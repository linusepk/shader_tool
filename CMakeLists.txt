cmake_minimum_required(VERSION 3.29)

project(arkin_shader)

# Arkin
set(ARKIN_DEBUG true)
add_subdirectory("${CMAKE_SOURCE_DIR}/libs/arkin")

# Glslang
set(GLSLANG_ENABLE_INSTALL false)
set(ENABLE_GLSLANG_BINARIES false)
add_subdirectory("${CMAKE_SOURCE_DIR}/libs/glslang")

# SPIRV-Cross
set(SPIRV_CROSS_CLI false)
set(SPIRV_CROSS_ENABLE_TESTS false)
add_subdirectory("${CMAKE_SOURCE_DIR}/libs/spirv_cross")

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED true)
set(CMAKE_EXPORT_COMPILE_COMMANDS true)
add_compile_options("-Wall" "-Wextra")

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin")
set(CMAKE_BUILD_TYPE Debug)

set(SOURCES
    src/main.c
    src/utils.c
    src/parser.c
    src/reflection.c
    src/compiler.c
)

add_executable(${CMAKE_PROJECT_NAME} ${SOURCES})
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE "${CMAKE_SOURCE_DIR}/include")
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE "${CMAKE_SOURCE_DIR}/src")
target_link_libraries(${CMAKE_PROJECT_NAME} arkin glslang glslang-default-resource-limits SPIRV spirv-cross-c)
