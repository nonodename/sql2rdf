cmake_minimum_required(VERSION 3.5...3.29)
project(SQL2RDF++ LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
include(FetchContent)

FetchContent_Declare(
  Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG v3.13.0 # or latest release
)

FetchContent_MakeAvailable(Catch2)

# ----------------------------------------------------------------------------
# DuckDB dependency - built from the single-file amalgamation
# ----------------------------------------------------------------------------
FetchContent_Declare(
  duckdb
  GIT_REPOSITORY https://github.com/duckdb/duckdb.git
  GIT_TAG        v1.4.4 # Use the latest stable tag
)
FetchContent_GetProperties(duckdb)
if(NOT duckdbn_POPULATED)
  FetchContent_MakeAvailable(duckdb) 
endif()

#add_library(duckdb_static STATIC "${duckdb_amalgamation_SOURCE_DIR}/duckdb.cpp")
#target_include_directories(duckdb_static PUBLIC "${duckdb_amalgamation_SOURCE_DIR}")
find_package(Threads REQUIRED)
target_link_libraries(duckdb PUBLIC Threads::Threads ${CMAKE_DL_LIBS})

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
  src/r2rml/DuckDBConnection.cpp
)
add_library(sql2rdf_r2rml STATIC ${R2RML_SOURCES})
target_include_directories(sql2rdf_r2rml PUBLIC include ${CMAKE_SOURCE_DIR}/external/serd/include)
if(UNIX)
  target_link_libraries(sql2rdf_r2rml PUBLIC serd m duckdb)
else()
  target_link_libraries(sql2rdf_r2rml PUBLIC serd dduckdbc)
endif()

# Specify main executable sources and link to the library
add_executable(${PROJECT_NAME} src/main.cpp)
target_link_libraries(${PROJECT_NAME} PRIVATE sql2rdf_r2rml)
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
if(UNIX)
  target_link_libraries(${PROJECT_NAME} PRIVATE serd m)
else()
  target_link_libraries(${PROJECT_NAME} PRIVATE serd)
endif()
