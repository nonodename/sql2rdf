cmake_minimum_required(VERSION 3.5...3.29)
project(SQL2RDF++ LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
include(FetchContent)

FetchContent_Declare(
  Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG v3.13.0 # or latest release
)

FetchContent_MakeAvailable(Catch2)

# Create your test executable from all tests in the `tests/` folder
file(GLOB TEST_SOURCES "${CMAKE_SOURCE_DIR}/tests/*.cpp")
add_executable(tests ${TEST_SOURCES})

# Include headers for tests and link to Catch2 and serd
target_include_directories(tests PRIVATE include ${CMAKE_SOURCE_DIR}/external/serd/include)
target_link_libraries(tests PRIVATE Catch2::Catch2WithMain sql2rdf_r2rml serd m)
target_compile_definitions(tests PRIVATE SOURCE_R2RML_DIR="${CMAKE_SOURCE_DIR}/tests/sourceR2RML/")

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
  src/r2rml/SQLConnection.cpp
  src/r2rml/SQLResultSet.cpp
  src/r2rml/SQLRow.cpp
  src/r2rml/SQLValue.cpp
  src/r2rml/R2RMLParser.cpp
)
add_library(sql2rdf_r2rml STATIC ${R2RML_SOURCES})
target_include_directories(sql2rdf_r2rml PUBLIC include ${CMAKE_SOURCE_DIR}/external/serd/include)
target_link_libraries(sql2rdf_r2rml PUBLIC serd m)

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
target_link_libraries(${PROJECT_NAME} PRIVATE serd m)
