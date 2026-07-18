# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this project is

SQL2RDF++ converts relational database tables to RDF using [R2RML](https://www.w3.org/TR/r2rml/) mappings, with [YARRRML](https://rml.io/yarrrml/spec/) (YAML) accepted as a friendlier front-end. The long-term goal is to lift the core library into a DuckDB extension, so the core must stay free of DuckDB and YAML dependencies (see layering below).

The repo also includes `sql2rdf_sparql`, a standalone SPARQL 1.1 Query grammar parser (`namespace sparql::`). It has no relationship to the R2RML/YARRRML mapping pipeline — it doesn't depend on `sql2rdf_r2rml`/`sql2rdf_yarrrml`/DuckDB/yaml-cpp/Serd, and nothing in the mapping pipeline depends on it. It exists to parse and inspect `.rq` query files; the CLI exposes it via a separate `-Q <file.rq>` flag that parses-and-prints an AST, bypassing the mapping/DB/output pipeline entirely.

`sql2rdf_sparql2sql` (`namespace sparql2sql::`) is the one deliberate bridge between the two: it uses an R2RML mapping *in reverse* to translate a SPARQL query into a SQL query that a relational engine can run directly, without ever materializing the RDF graph. `sql2rdf_sparql` and `sql2rdf_r2rml` themselves remain mutually independent — only `sql2rdf_sparql2sql` depends on both. The CLI exposes it via `-T <file.rq>` (see "Architecture" and `doc/api.md` for the supported SPARQL subset and known limitations).

Language standard is C++11 (`CMAKE_CXX_STANDARD 11` in CMakeLists.txt). Do not use C++14+ features in library or test code.

## Setup and build

One-time after cloning (Serd is a git submodule):

```sh
git submodule update --init --recursive
cmake -B build
```

Common targets:

```sh
cmake --build build --target sql2rdf_r2rml     # core library (no DuckDB, no yaml-cpp)
cmake --build build --target sql2rdf_yarrrml   # YARRRML→R2RML translator library
cmake --build build --target sql2rdf_sparql    # standalone SPARQL query parser library
cmake --build build --target sql2rdf_sparql2sql # SPARQL-to-SQL translator (no DuckDB, no yaml-cpp)
cmake --build build --target test_runner       # tests (no DuckDB needed)
cmake --build build --target SQL2RDF++         # CLI (requires DuckDB installed, e.g. `brew install duckdb`)
cmake --build build                            # everything
```

If DuckDB is not installed, CMake prints a warning and skips the CLI; the libraries and tests still build. `-DUSE_EMBEDDED_DUCKDB=ON` builds DuckDB from source instead (slow — avoid unless needed).

## Tests

Catch2 (fetched via FetchContent). All tests use a mock SQL backend (`tests/MockSQL.h`) — DuckDB is never required to run them.

```sh
cmake --build build --target tests           # build + run all tests
ctest --test-dir build                       # alternative: run via CTest
./build/test_runner "<test case name>"       # run a single Catch2 test case (build test_runner first)
ctest --test-dir build -R "<regex>"          # or filter by name via CTest
```

Test fixtures: R2RML Turtle mappings in `tests/sourceR2RML/`, YARRRML equivalents plus feature/error fixtures in `tests/sourceYARRRML/`, SPARQL query fixtures (valid queries and `invalid_*.rq` error cases) in `tests/sourceSPARQL/`, SPARQL-to-SQL translator query fixtures in `tests/sourceSPARQL2SQL/` (translated against the R2RML fixtures above, mainly `example_emp_dept.ttl` and a few dedicated toy mappings prefixed `sparql2sql_*.ttl`). The directories are passed to the test binary as compile definitions `SOURCE_R2RML_DIR` / `SOURCE_YARRRML_DIR` / `SOURCE_SPARQL_DIR` / `SOURCE_SPARQL2SQL_DIR`, so tests reference fixtures by bare filename. R2RML/YARRRML fixtures are based on the examples in the W3C R2RML spec.

`test_runner` can only assert on the *structural* shape of translator-generated SQL (string content, e.g. "contains a LEFT OUTER JOIN"), since it deliberately never links DuckDB. Real execution-correctness validation for the SPARQL-to-SQL translator lives in a separate, gated CTest target, `sparql2sql_duckdb_tests` (`tests/duckdb/`), built only `if(SQL2RDF_BUILD_TESTS AND SQL2RDF_BUILD_CLI AND (DUCKDB_FOUND OR USE_EMBEDDED_DUCKDB))` — it translates each fixture query, executes the SQL against a real in-memory DuckDB database seeded with toy data, and asserts on actual result rows. This is the one other place besides the CLI that requires DuckDB; it's kept as its own target specifically so `test_runner`'s "no DuckDB needed" guarantee above stays true. Run it directly (`./build/sparql2sql_duckdb_tests`) once built; its cases are also registered with CTest alongside `test_runner`'s (`ctest --test-dir build` runs both).

## Lint / format

Requires `clang-format` and `clang-tidy` on PATH:

```sh
cmake --build build --target format        # apply clang-format in-place
cmake --build build --target format-check  # CI-style check, non-zero exit on violations
cmake --build build --target tidy          # clang-tidy (WarningsAsErrors: '*')
```

Run `format` and `tidy` before committing — CI enforces both. Style: LLVM base, tabs for indentation, 120-column limit (`.clang-format`). Naming: classes/functions CamelCase, members/parameters lower_case (`.clang-tidy`).

## Architecture

Strict dependency layering — this is the most important constraint when adding code:

1. **`sql2rdf_r2rml`** (`src/r2rml/`, headers in `include/r2rml/`, namespace `r2rml::`) — the core R2RML engine. Links only Serd (vendored C library, `external/serd` submodule). Must never depend on DuckDB or yaml-cpp.
2. **`sql2rdf_yarrrml`** (`src/yarrrml/YARRRMLParser.cpp`) — translates YARRRML YAML into R2RML Turtle **text**, which is then fed to the same `R2RMLParser` used for `.ttl` files, so both formats produce identical output. Links yaml-cpp PRIVATE so it doesn't leak to consumers.
3. **`SQL2RDF++` CLI** (`src/main.cpp`, `src/DuckDBConnection.*`) — the only code that touches DuckDB. `DuckDBConnection.h` deliberately lives in `src/`, not `include/`.

**`sql2rdf_sparql`** (`src/sparql-parser/`, headers in `include/sparql-parser/`, namespace `sparql::`) sits outside this layering entirely — a standalone SPARQL 1.1 Query grammar parser (recursive descent, one-token lookahead) with no dependency on `sql2rdf_r2rml`/`sql2rdf_yarrrml`/DuckDB/yaml-cpp/Serd, only the C++ standard library. `Lexer`/`Parser` produce an AST (`include/sparql-parser/ast/`); `PrettyPrinter` renders it back to text; `ParseError` reports failures. SPARQL *Update* grammar is explicitly out of scope — only the *Query* grammar is supported. Keep it standalone: do not introduce a dependency from `sql2rdf_sparql` on the R2RML/YARRRML code, or vice versa, unless a future task explicitly wires SPARQL into the mapping pipeline.

**`sql2rdf_sparql2sql`** (`src/sparql2sql/`, headers in `include/sparql2sql/`, namespace `sparql2sql::`) is that future task: it translates a parsed SPARQL query (`sparql::ast::Query`) against a parsed R2RML mapping (`r2rml::R2RMLMapping`) into a SQL string for a given `SqlDialect`, by using the mapping's `TriplesMap`/`PredicateObjectMap`/`TermMap` structure in reverse (enumerate which mapping sources could produce a triple matching a given triple pattern, then compose per-pattern SQL relations via the SPARQL algebra — AND→inner join, OPTIONAL→left outer join, UNION→schema-extending union, MINUS→anti-join). It depends on **both** `sql2rdf_r2rml` and `sql2rdf_sparql`; neither of those may depend back on it, and it has no DuckDB/yaml-cpp dependency of its own (so it links safely into the DuckDB-free `test_runner`). Entry point: `sparql2sql::translateQuery()` (`include/sparql2sql/Translator.h`). Only SELECT/ASK query forms and a constant-IRI-or-variable predicate position (no property path operators) are supported; every SPARQL variable is represented as a plain SQL VARCHAR of the term's lexical form (no term-kind/datatype/language tracking) — see `doc/api.md`'s "Supported SPARQL subset / Known limitations" for the full, current list. The CLI exposes it via `-T <file.rq> [--dialect <name>]`, paired with the mapping-file positional argument; if the database-file positional is also given, the translated SQL is additionally executed and its result rows printed (see `-h`).

Key abstractions in the core:

- Database access goes through the abstract `SQLConnection` / `SQLResultSet` / `SQLRow` / `SQLValue` interfaces (`include/r2rml/`). New backends implement `SQLConnection`; tests use `MockSQL.h`.
- The R2RML object model mirrors the spec: `R2RMLMapping` → `TriplesMap` (with a `LogicalTable`: `BaseTableOrView` or `R2RMLView`) → `SubjectMap` + `PredicateObjectMap`s → `TermMap` subclasses (`ConstantTermMap`, `ColumnTermMap`, `TemplateTermMap`, `ReferencingObjectMap` for joins).
- Output is written through Serd (`SerdWriter`); `R2RMLMapping::processDatabase(db, writer)` drives generation.
- "Inside-out" mode (`isValidInsideOut()`) validates mappings for a future SQL-export scenario where row data comes from outside rather than a query: `rr:logicalTable`, `rr:sqlQuery`, and referencing object maps / join conditions are prohibited.

Error-handling convention: the YARRRML parser collects non-fatal issues (unsupported keys, skipped graphs, etc.) into `R2RMLMapping::parseErrors` in lenient mode (default) or throws `std::runtime_error` in strict mode (`ignoreNonFatalErrors=false`); fatal problems (unreadable file, YAML syntax error, missing `mappings` key) always throw. Preserve this split when extending the parser.

`doc/api.md` is a hand-written API reference for the core library — update it when changing public headers. The README documents the supported YARRRML subset in detail; keep it in sync when extending YARRRML support.

## FetchContent consumption

Downstream projects consume this repo via CMake FetchContent (`sql2rdf::r2rml` / `sql2rdf::yarrrml` alias targets). When not top-level, the CLI, tests, and format/tidy targets are all skipped (`SQL2RDF_BUILD_CLI` / `SQL2RDF_BUILD_TESTS` default OFF), and an existing `serd` target in the consumer is reused instead of vendoring. Don't add unconditional targets or dependencies to CMakeLists.txt that would leak into downstream builds — gate dev-only things behind `SQL2RDF_IS_TOP_LEVEL`.
