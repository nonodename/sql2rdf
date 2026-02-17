cmake_minimum_required(VERSION 3.5)
project(SQL2RDF++ LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Specify source files
set(SOURCES
    src/main.cpp
    src/r2rml/r2rml_scaffolding.cpp
    src/r2rml/SQLValue.cpp
)

add_executable(${PROJECT_NAME} ${SOURCES})

# Include directories
target_include_directories(${PROJECT_NAME} PRIVATE include)

# ----------------------------------------------------------------------------
# serd dependency - build as a static library from source tree
# ----------------------------------------------------------------------------
# grab all C source files from the git submodule
file(GLOB SERD_C_SOURCES "${CMAKE_SOURCE_DIR}/external/serd/src/*.c")
add_library(serd STATIC ${SERD_C_SOURCES})
# Make sure the library is treated as C (not C++)
set_target_properties(serd PROPERTIES LINKER_LANGUAGE C)
# public include directory for users of the library
target_include_directories(serd PUBLIC
    ${CMAKE_SOURCE_DIR}/external/serd/include
)
# Build as a static library (no shared/dll)
target_compile_definitions(serd PUBLIC SERD_STATIC)

# link the serd static library into our executable
add_dependencies(${PROJECT_NAME} serd)
target_link_libraries(${PROJECT_NAME} PRIVATE serd m)
