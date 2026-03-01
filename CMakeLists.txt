cmake_minimum_required(VERSION 3.5...3.29)
project(SQL2RDF++ LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
include(FetchContent)

FetchContent_Declare(
  Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG v3.13.0 # or latest release
)

FetchContent_MakeAvailable(Catch2)

# ----------------------------------------------------------------------------
# DuckDB dependency - prefer system-installed library; fall back to embedded
# ----------------------------------------------------------------------------
# By default we assume a system-installed libduckdb and duckdb.hpp are
# available (for faster CI builds). Set `-DUSE_EMBEDDED_DUCKDB=ON` to force
# building DuckDB from source (amalgamation / FetchContent).
option(USE_EMBEDDED_DUCKDB "Build DuckDB from source (amalgamation). If OFF, find system libduckdb and duckdb.hpp" OFF)

find_package(Threads REQUIRED)

if(USE_EMBEDDED_DUCKDB)
  FetchContent_Declare(
    duckdb
    GIT_REPOSITORY https://github.com/duckdb/duckdb.git
    GIT_TAG        v1.4.4 # Use the latest stable tag
  )
  FetchContent_GetProperties(duckdb)
  if(NOT duckdb_POPULATED)
    FetchContent_MakeAvailable(duckdb)
  endif()

  # when building embedded DuckDB, make sure the target links to threads/dl
  target_link_libraries(duckdb PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
else()
  # Try to find system-installed DuckDB (header: duckdb.hpp, library: libduckdb)
  find_path(DUCKDB_INCLUDE_DIR NAMES duckdb.h HINTS /opt/homebrew/include )
  find_library(DUCKDB_LIBRARY NAMES duckdb)

  if(NOT DUCKDB_INCLUDE_DIR OR NOT DUCKDB_LIBRARY)
    message(STATUS "Debug: DUCKDB_INCLUDE_DIR=${DUCKDB_INCLUDE_DIR}")
    message(STATUS "Debug: DUCKDB_LIBRARY=${DUCKDB_LIBRARY}")
    message(FATAL_ERROR "DuckDB not found...")
  endif()

  # Create an imported target so the rest of the CMakeLists can link to 'duckdb'
  add_library(duckdb UNKNOWN IMPORTED)
  set_target_properties(duckdb PROPERTIES
    IMPORTED_LOCATION "${DUCKDB_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${DUCKDB_INCLUDE_DIR}"
  )
endif()

enable_testing()
include(Catch)

# Create your test executable from all tests in the `tests/` folder
file(GLOB TEST_SOURCES "${CMAKE_SOURCE_DIR}/tests/*.cpp")
add_executable(test_runner ${TEST_SOURCES})

# Include headers for tests and link to Catch2 and serd
target_include_directories(test_runner PRIVATE include ${CMAKE_SOURCE_DIR}/external/serd/include ${CMAKE_SOURCE_DIR}/tests)
if(UNIX)
  target_link_libraries(test_runner PRIVATE Catch2::Catch2WithMain sql2rdf_r2rml serd m)
else()
  target_link_libraries(test_runner PRIVATE Catch2::Catch2WithMain sql2rdf_r2rml serd)
endif()
target_compile_definitions(test_runner PRIVATE SOURCE_R2RML_DIR="${CMAKE_SOURCE_DIR}/tests/sourceR2RML/")

# Register individual Catch2 test cases with CTest
catch_discover_tests(test_runner)

# `make tests` builds and runs the test suite
add_custom_target(tests
  COMMAND $<TARGET_FILE:test_runner>
  DEPENDS test_runner
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
  COMMENT "Running tests..."
)

# Build r2rml implementation as a static library so tests can link it
set(R2RML_SOURCES
  src/r2rml/R2RMLMapping.cpp
  src/r2rml/LogicalTable.cpp
  src/r2rml/BaseTableOrView.cpp
  src/r2rml/R2RMLView.cpp
  src/r2rml/TriplesMap.cpp
  src/r2rml/PredicateObjectMap.cpp
  src/r2rml/TermMap.cpp
  src/r2rml/ConstantTermMap.cpp
  src/r2rml/ColumnTermMap.cpp
  src/r2rml/TemplateTermMap.cpp
  src/r2rml/SubjectMap.cpp
  src/r2rml/PredicateMap.cpp
  src/r2rml/ObjectMap.cpp
  src/r2rml/GraphMap.cpp
  src/r2rml/JoinCondition.cpp
  src/r2rml/ReferencingObjectMap.cpp
  src/r2rml/SQLRow.cpp
  src/r2rml/SQLValue.cpp
  src/r2rml/R2RMLParser.cpp
)
add_library(sql2rdf_r2rml STATIC ${R2RML_SOURCES})
target_include_directories(sql2rdf_r2rml PUBLIC include ${CMAKE_SOURCE_DIR}/external/serd/include)
if(UNIX)
  target_link_libraries(sql2rdf_r2rml PUBLIC serd m)
else()
  target_link_libraries(sql2rdf_r2rml PUBLIC serd)
endif()

# Specify main executable sources and link to the library
add_executable(${PROJECT_NAME} src/main.cpp src/DuckDBConnection.cpp)
target_link_libraries(${PROJECT_NAME} PRIVATE sql2rdf_r2rml duckdb)
target_include_directories(${PROJECT_NAME} PRIVATE include src)

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
if(UNIX)
  target_link_libraries(${PROJECT_NAME} PRIVATE serd m)
else()
  target_link_libraries(${PROJECT_NAME} PRIVATE serd)
endif()

# ----------------------------------------------------------------------------
# clang-format targets
# ----------------------------------------------------------------------------
file(GLOB_RECURSE FORMAT_SOURCES
  "${CMAKE_SOURCE_DIR}/src/*.cpp"
  "${CMAKE_SOURCE_DIR}/src/*.h"
  "${CMAKE_SOURCE_DIR}/include/*.h"
  "${CMAKE_SOURCE_DIR}/tests/*.cpp"
  "${CMAKE_SOURCE_DIR}/tests/*.h"
)

find_program(CLANG_FORMAT_EXE NAMES clang-format)
if(CLANG_FORMAT_EXE)
  add_custom_target(format
    COMMAND ${CLANG_FORMAT_EXE} -i ${FORMAT_SOURCES}
    COMMENT "Formatting C++ sources with clang-format..."
  )
  add_custom_target(format-check
    COMMAND ${CLANG_FORMAT_EXE} --dry-run --Werror ${FORMAT_SOURCES}
    COMMENT "Checking C++ source formatting..."
  )
else()
  message(STATUS "clang-format not found; 'format' and 'format-check' targets unavailable")
endif()

# ----------------------------------------------------------------------------
# clang-tidy target
# ----------------------------------------------------------------------------
find_program(CLANG_TIDY_EXE NAMES clang-tidy)

if(CLANG_TIDY_EXE)
  file(GLOB_RECURSE TIDY_SOURCES
    "${CMAKE_SOURCE_DIR}/src/r2rml/*.cpp"
    "${CMAKE_SOURCE_DIR}/include/r2rml/*.h"
    "${CMAKE_SOURCE_DIR}/tests/*.cpp"
  )
  add_custom_target(tidy
    COMMAND ${CLANG_TIDY_EXE} -p "${CMAKE_BINARY_DIR}" ${TIDY_SOURCES}
    COMMENT "Running clang-tidy..."
    DEPENDS sql2rdf_r2rml
  )
else()
  message(STATUS "clang-tidy not found; 'tidy' target unavailable")
endif()
