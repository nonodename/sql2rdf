cmake_minimum_required(VERSION 3.10...3.29)
project(SQL2RDF++ LANGUAGES C CXX)

# ----------------------------------------------------------------------------
# Top-level detection - lets downstream projects pull this in via
# FetchContent_Declare/FetchContent_MakeAvailable and get only the library
# targets (sql2rdf_r2rml, sql2rdf_yarrrml), not this project's own CLI, tests,
# or dev-utility targets.
# ----------------------------------------------------------------------------
set(SQL2RDF_IS_TOP_LEVEL OFF)
if(CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME)
  set(SQL2RDF_IS_TOP_LEVEL ON)
endif()

option(SQL2RDF_BUILD_TESTS "Build sql2rdf's own test_runner and fetch Catch2" ${SQL2RDF_IS_TOP_LEVEL})
option(SQL2RDF_BUILD_CLI "Build the SQL2RDF++ CLI executable (requires DuckDB)" ${SQL2RDF_IS_TOP_LEVEL})

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
include(FetchContent)

# Include dependency checksums and verification
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/Dependencies.cmake)

if(SQL2RDF_BUILD_TESTS)
  FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG v3.15.1 # or latest release
    URL_HASH SHA256=${CATCH2_SHA256}
  )

  FetchContent_MakeAvailable(Catch2)
endif()

# ----------------------------------------------------------------------------
# yaml-cpp dependency - used only by the sql2rdf_yarrrml library so that
# sql2rdf_r2rml (and anyone linking only it) stays free of a YAML dependency.
# ----------------------------------------------------------------------------
set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(YAML_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
# Avoid a "format" custom-target name collision with this project's own
# `format` target (below); yaml-cpp's own formatting is irrelevant to us.
set(YAML_CPP_FORMAT_SOURCE OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
  yaml-cpp
  GIT_REPOSITORY https://github.com/jbeder/yaml-cpp.git
  GIT_TAG yaml-cpp-0.9.0
  URL_HASH SHA256=${YAML_CPP_SHA256}
)

FetchContent_MakeAvailable(yaml-cpp)

# ----------------------------------------------------------------------------
# DuckDB dependency - prefer system-installed library; fall back to embedded
# ----------------------------------------------------------------------------
# By default we assume a system-installed libduckdb and duckdb.hpp are
# available (for faster CI builds). Set `-DUSE_EMBEDDED_DUCKDB=ON` to force
# building DuckDB from source (amalgamation / FetchContent).
option(USE_EMBEDDED_DUCKDB "Build DuckDB from source (amalgamation). If OFF, find system libduckdb and duckdb.hpp" OFF)

if(SQL2RDF_BUILD_CLI)
  find_package(Threads REQUIRED)

  if(USE_EMBEDDED_DUCKDB)
    # Disable DuckDB's own CLI shell and unittest suite so embedding DuckDB
    # doesn't pull in targets we don't want (mirrors the yaml-cpp pattern above).
    set(BUILD_SHELL OFF CACHE BOOL "" FORCE)
    set(BUILD_UNITTESTS OFF CACHE BOOL "" FORCE)
    set(BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)

    FetchContent_Declare(
      duckdb
      GIT_REPOSITORY https://github.com/duckdb/duckdb.git
      GIT_TAG        v1.4.4 # Use the latest stable tag
      URL_HASH SHA256=${DUCKDB_SHA256}
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
      message(WARNING "DuckDB not found; the main sql2rdf executable will not be built. Tests can still be built and run.")
      set(DUCKDB_FOUND FALSE)
    else()
      set(DUCKDB_FOUND TRUE)
    endif()

    # Create an imported target so the rest of the CMakeLists can link to 'duckdb'
    add_library(duckdb UNKNOWN IMPORTED)
    set_target_properties(duckdb PROPERTIES
      IMPORTED_LOCATION "${DUCKDB_LIBRARY}"
      INTERFACE_INCLUDE_DIRECTORIES "${DUCKDB_INCLUDE_DIR}"
    )
  endif()
endif()

if(SQL2RDF_BUILD_TESTS)
  enable_testing()
  include(Catch)

  # Create your test executable from all tests in the `tests/` folder
  file(GLOB TEST_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp")
  add_executable(test_runner ${TEST_SOURCES})

  # Include headers for tests and link to Catch2 and serd
  target_include_directories(test_runner PRIVATE include ${CMAKE_CURRENT_SOURCE_DIR}/external/serd/include ${CMAKE_CURRENT_SOURCE_DIR}/tests)
  if(UNIX)
    target_link_libraries(test_runner PRIVATE Catch2::Catch2WithMain sql2rdf_r2rml sql2rdf_yarrrml sql2rdf_sparql sql2rdf_sparql2sql serd m)
  else()
    target_link_libraries(test_runner PRIVATE Catch2::Catch2WithMain sql2rdf_r2rml sql2rdf_yarrrml sql2rdf_sparql sql2rdf_sparql2sql serd)
  endif()
  target_compile_definitions(test_runner PRIVATE SOURCE_R2RML_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/sourceR2RML/")
  target_compile_definitions(test_runner PRIVATE SOURCE_YARRRML_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/sourceYARRRML/")
  target_compile_definitions(test_runner PRIVATE SOURCE_SPARQL_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/sourceSPARQL/")
  target_compile_definitions(test_runner PRIVATE SOURCE_SPARQL2SQL_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/sourceSPARQL2SQL/")

  # Register individual Catch2 test cases with CTest
  catch_discover_tests(test_runner)

  # `make tests` builds and runs the test suite
  add_custom_target(tests
    COMMAND $<TARGET_FILE:test_runner>
    DEPENDS test_runner
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    COMMENT "Running tests..."
  )
endif()

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
  src/r2rml/MapSQLRow.cpp
  src/r2rml/SQLValue.cpp
  src/r2rml/StringSQLValue.cpp
  src/r2rml/R2RMLParser.cpp
)
add_library(sql2rdf_r2rml STATIC ${R2RML_SOURCES})
add_library(sql2rdf::r2rml ALIAS sql2rdf_r2rml)
target_include_directories(sql2rdf_r2rml PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/external/serd/include>
  $<INSTALL_INTERFACE:include>
)
if(UNIX)
  target_link_libraries(sql2rdf_r2rml PUBLIC serd m)
else()
  target_link_libraries(sql2rdf_r2rml PUBLIC serd)
endif()

# ----------------------------------------------------------------------------
# YARRRML support - a separate static library so that sql2rdf_r2rml itself
# stays free of the yaml-cpp dependency.
# ----------------------------------------------------------------------------
add_library(sql2rdf_yarrrml STATIC src/yarrrml/YARRRMLParser.cpp src/yarrrml/MappingParserFactory.cpp)
add_library(sql2rdf::yarrrml ALIAS sql2rdf_yarrrml)
target_include_directories(sql2rdf_yarrrml PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)
target_link_libraries(sql2rdf_yarrrml PUBLIC sql2rdf_r2rml)
target_link_libraries(sql2rdf_yarrrml PRIVATE yaml-cpp::yaml-cpp)

# ----------------------------------------------------------------------------
# SPARQL 1.1 Query grammar parser - a standalone library with no dependency
# on r2rml/yarrrml/DuckDB/yaml-cpp/serd (genuinely independent of the
# mapping-conversion domain; only <string>/<vector>/<memory>/<stdexcept>).
# ----------------------------------------------------------------------------
set(SPARQL_SOURCES
  src/sparql-parser/Lexer.cpp
  src/sparql-parser/ParseError.cpp
  src/sparql-parser/Ast.cpp
  src/sparql-parser/Parser.cpp
  src/sparql-parser/ParserGraphPatterns.cpp
  src/sparql-parser/ParserPaths.cpp
  src/sparql-parser/ParserExpressions.cpp
  src/sparql-parser/PrettyPrinter.cpp
)
add_library(sql2rdf_sparql STATIC ${SPARQL_SOURCES})
add_library(sql2rdf::sparql ALIAS sql2rdf_sparql)
target_include_directories(sql2rdf_sparql PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)

# ----------------------------------------------------------------------------
# SPARQL-to-SQL translator - uses an R2RML mapping in reverse to translate a
# SPARQL query into SQL for a given dialect. The one deliberate bridge between
# sql2rdf_sparql and sql2rdf_r2rml; neither of those depend on this, only the
# other way around. No DuckDB/yaml-cpp dependency of its own, so it's safe to
# link into the plain (DuckDB-free) test_runner.
# ----------------------------------------------------------------------------
set(SPARQL2SQL_SOURCES
  src/sparql2sql/TranslationError.cpp
  src/sparql2sql/SqlDialect.cpp
  src/sparql2sql/DuckDbDialect.cpp
  src/sparql2sql/DialectFactory.cpp
  src/sparql2sql/TemplateUtil.cpp
  src/sparql2sql/TermMapSql.cpp
  src/sparql2sql/TranslatedPattern.cpp
  src/sparql2sql/TriplePatternTranslator.cpp
  src/sparql2sql/ExpressionTranslator.cpp
  src/sparql2sql/PatternFolder.cpp
  src/sparql2sql/Translator.cpp
)
add_library(sql2rdf_sparql2sql STATIC ${SPARQL2SQL_SOURCES})
add_library(sql2rdf::sparql2sql ALIAS sql2rdf_sparql2sql)
target_include_directories(sql2rdf_sparql2sql PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)
target_link_libraries(sql2rdf_sparql2sql PUBLIC sql2rdf_r2rml sql2rdf_sparql)

# ----------------------------------------------------------------------------
# DuckDB-backed SQLConnection - a small static library (requires DuckDB) so
# both the CLI and the DuckDB-execution validation test target (see below)
# can link a real SQLConnection without compiling DuckDBConnection.cpp
# twice. Deliberately lives outside include/ (its public header is under
# src/) so DuckDB never becomes a dependency of anything reachable from the
# plain, DuckDB-free test_runner target.
# ----------------------------------------------------------------------------
if(SQL2RDF_BUILD_CLI AND (DUCKDB_FOUND OR USE_EMBEDDED_DUCKDB))
  add_library(sql2rdf_duckdb STATIC src/DuckDBConnection.cpp)
  target_include_directories(sql2rdf_duckdb PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
  )
  target_link_libraries(sql2rdf_duckdb PUBLIC sql2rdf_r2rml duckdb)
endif()

# Specify main executable sources and link to the library (requires DuckDB)
if(SQL2RDF_BUILD_CLI AND (DUCKDB_FOUND OR USE_EMBEDDED_DUCKDB))
  add_executable(${PROJECT_NAME} src/main.cpp)
  target_link_libraries(${PROJECT_NAME} PRIVATE sql2rdf_r2rml sql2rdf_yarrrml sql2rdf_sparql sql2rdf_sparql2sql sql2rdf_duckdb duckdb)
  target_include_directories(${PROJECT_NAME} PRIVATE include src)
endif()

# ----------------------------------------------------------------------------
# serd dependency - build as a static library from source tree, unless a
# consumer has already defined a `serd` target (e.g. via their own
# find_package/FetchContent of serd) before pulling this project in, in
# which case we reuse theirs instead of vendoring a second copy.
# ----------------------------------------------------------------------------
if(NOT TARGET serd)
  # Verify Serd submodule commit hash
  verify_submodule_commit("${CMAKE_CURRENT_SOURCE_DIR}/external/serd" "${SERD_EXPECTED_COMMIT}")
  
  # grab all C source files from the git submodule
  file(GLOB SERD_C_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/external/serd/src/*.c")
  add_library(serd STATIC ${SERD_C_SOURCES})
  # Make sure the library is treated as C (not C++)
  set_target_properties(serd PROPERTIES LINKER_LANGUAGE C)
  # public include directory for users of the library
  target_include_directories(serd PUBLIC
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/external/serd/include>
      $<INSTALL_INTERFACE:include>
  )
  # Build as a static library (no shared/dll)
  target_compile_definitions(serd PUBLIC SERD_STATIC)
endif()

# link the serd static library into our executable (only when it was created)
if(SQL2RDF_BUILD_CLI AND (DUCKDB_FOUND OR USE_EMBEDDED_DUCKDB))
  add_dependencies(${PROJECT_NAME} serd)
  if(UNIX)
    target_link_libraries(${PROJECT_NAME} PRIVATE serd m)
  else()
    target_link_libraries(${PROJECT_NAME} PRIVATE serd)
  endif()
endif()

# ----------------------------------------------------------------------------
# SPARQL-to-SQL DuckDB-execution validation suite - the one other place
# (besides the CLI) that needs a real DuckDB. Kept as its own gated,
# separately-registered CTest target so the plain test_runner/`ctest` suite
# stays exactly as DuckDB-free as CLAUDE.md promises: this target simply
# doesn't exist (and isn't registered with CTest) when DuckDB isn't
# available, and no DuckDB-touching source file is ever compiled into
# test_runner.
# ----------------------------------------------------------------------------
if(SQL2RDF_BUILD_TESTS AND SQL2RDF_BUILD_CLI AND (DUCKDB_FOUND OR USE_EMBEDDED_DUCKDB))
  file(GLOB SPARQL2SQL_DUCKDB_TEST_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/tests/duckdb/*.cpp")
  add_executable(sparql2sql_duckdb_tests ${SPARQL2SQL_DUCKDB_TEST_SOURCES})
  target_include_directories(sparql2sql_duckdb_tests PRIVATE
    include src ${CMAKE_CURRENT_SOURCE_DIR}/external/serd/include ${CMAKE_CURRENT_SOURCE_DIR}/tests
  )
  if(UNIX)
    target_link_libraries(sparql2sql_duckdb_tests PRIVATE
      Catch2::Catch2WithMain sql2rdf_r2rml sql2rdf_sparql sql2rdf_sparql2sql sql2rdf_duckdb duckdb serd m
    )
  else()
    target_link_libraries(sparql2sql_duckdb_tests PRIVATE
      Catch2::Catch2WithMain sql2rdf_r2rml sql2rdf_sparql sql2rdf_sparql2sql sql2rdf_duckdb duckdb serd
    )
  endif()
  target_compile_definitions(sparql2sql_duckdb_tests PRIVATE
    SOURCE_R2RML_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/sourceR2RML/"
    SOURCE_SPARQL2SQL_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/sourceSPARQL2SQL/"
  )
  add_dependencies(sparql2sql_duckdb_tests serd)
  catch_discover_tests(sparql2sql_duckdb_tests)
endif()

if(SQL2RDF_IS_TOP_LEVEL)
  # ----------------------------------------------------------------------------
  # clang-format targets
  # ----------------------------------------------------------------------------
  file(GLOB_RECURSE FORMAT_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.h"
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
      "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
      "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h"
      "${CMAKE_CURRENT_SOURCE_DIR}/src/yarrrml/*.cpp"
      "${CMAKE_CURRENT_SOURCE_DIR}/include/*.h"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp"
    )
    if(APPLE)
      execute_process(
        COMMAND xcrun --show-sdk-path
        OUTPUT_VARIABLE CLANG_TIDY_SYSROOT
        OUTPUT_STRIP_TRAILING_WHITESPACE
      )
      set(CLANG_TIDY_EXTRA_ARGS "--extra-arg=-isysroot${CLANG_TIDY_SYSROOT}")
    endif()

    add_custom_target(tidy
      COMMAND ${CLANG_TIDY_EXE} -p "${CMAKE_CURRENT_BINARY_DIR}" ${CLANG_TIDY_EXTRA_ARGS} ${TIDY_SOURCES}
      COMMENT "Running clang-tidy..."
      DEPENDS sql2rdf_r2rml
    )
  else()
    message(STATUS "clang-tidy not found; 'tidy' target unavailable")
  endif()
endif()
