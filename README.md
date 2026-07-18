# SQL2RDF++

A command line utility to convert relational database tables to RDF using R2RML syntax, with support for [YARRRML](https://rml.io/yarrrml/spec/) as a friendlier YAML front-end to R2RML.

Ultimately this is intended to prove out implementation of R2RML in C++ so that the code can be lifted into a DuckDB extension to support a `COPY TO` export from DuckDB.

The repo also includes a standalone [SPARQL 1.1 Query](https://www.w3.org/TR/sparql11-query/) grammar parser, and a SPARQL-to-SQL translator that runs an R2RML mapping *in reverse* to turn a SPARQL query into SQL that a relational engine can execute directly, without ever materializing the RDF graph. See [SPARQL query parsing](#sparql-query-parsing) and [SPARQL-to-SQL translation](#sparql-to-sql-translation) below.

## Build targets

The project is structured as a reusable library plus a thin CLI application:

| Target | Type | DuckDB dependency | Description |
|--------|------|-------------------|-------------|
| `sql2rdf_r2rml` | static library | none | Core R2RML implementation. Links only [Serd](https://drobilla.net/software/serd/) (embedded). Suitable for use in other projects, including a DuckDB extension. |
| `sql2rdf_yarrrml` | static library | none | YARRRML → R2RML translator. Publicly links `sql2rdf_r2rml` and privately links [yaml-cpp](https://github.com/jbeder/yaml-cpp) (fetched via CMake `FetchContent`), so consumers of `sql2rdf_r2rml` alone stay free of the YAML dependency. |
| `sql2rdf_sparql` | static library | none | Standalone SPARQL 1.1 Query grammar parser. No dependency on `sql2rdf_r2rml`, `sql2rdf_yarrrml`, DuckDB, yaml-cpp, or Serd — only the C++ standard library. |
| `sql2rdf_sparql2sql` | static library | none | SPARQL-to-SQL translator. Publicly links both `sql2rdf_r2rml` and `sql2rdf_sparql` (translates a parsed SPARQL query against a parsed R2RML mapping into SQL); no DuckDB/yaml-cpp dependency of its own, so it links into the DuckDB-free `test_runner`. |
| `SQL2RDF++` | executable | required | CLI application. Compiles the DuckDB adapter (`DuckDBConnection`) and links the system or embedded DuckDB library. Gated by `SQL2RDF_BUILD_CLI` (default: ON when building standalone, OFF when consumed via `FetchContent`). |
| `test_runner` | executable | none | Test suite using [Catch2](https://github.com/catchorg/Catch2). All tests run against a mock SQL backend — no DuckDB required. Gated by `SQL2RDF_BUILD_TESTS` (default: ON when building standalone, OFF when consumed via `FetchContent`). |
| `sparql2sql_duckdb_tests` | executable | required | Execution-correctness tests for the SPARQL-to-SQL translator: translates each fixture query and runs the resulting SQL against a real in-memory DuckDB database, asserting on actual result rows. Kept separate from `test_runner` specifically so that target stays DuckDB-free. Gated by `SQL2RDF_BUILD_TESTS AND SQL2RDF_BUILD_CLI` plus DuckDB availability; its cases also register with CTest. |
| `format` | utility | none | Apply `clang-format` to all project C++ sources in-place. Only defined when building standalone. |
| `format-check` | utility | none | Check formatting with `clang-format --dry-run --Werror`; exits non-zero if any file would change. Used in CI. Only defined when building standalone. |
| `tidy` | utility | none | Run `clang-tidy` static analysis using `.clang-tidy`. Builds `sql2rdf_r2rml` first to ensure a fresh compilation database. Only defined when building standalone. |

The [Serd](https://drobilla.net/software/serd/) RDF syntax library is included as a git submodule under `external/serd` and compiled from source into the `sql2rdf_r2rml` library.

## Dependency Verification

This project uses SHA256 checksum verification for all external dependencies to ensure build reproducibility and security:

- **FetchContent dependencies** (Catch2, yaml-cpp, DuckDB): CMake's built-in `URL_HASH` parameter verifies the downloaded content matches expected SHA256 hashes. The build will fail if any dependency's content doesn't match.
- **Git submodules** (Serd): A custom CMake verification function checks that the Serd submodule's commit hash matches the expected value. The build will fail if the submodule is at an unexpected commit.

Checksums are stored in `cmake/Dependencies.cmake`. The GitHub Actions workflow also verifies the Serd submodule commit hash before building.

### Updating Checksums

When updating a dependency version, you must update the corresponding checksum:

1. **FetchContent dependencies**: Download the new release tarball and compute its SHA256:
   ```sh
   curl -L https://github.com/user/repo/archive/refs/tags/vX.Y.Z.tar.gz | shasum -a 256
   ```
   Update the corresponding variable in `cmake/Dependencies.cmake`.

2. **Git submodules**: After updating the submodule, get the new commit hash:
   ```sh
   git -C external/serd rev-parse HEAD
   ```
   Update `SERD_EXPECTED_COMMIT` in `cmake/Dependencies.cmake` and the GitHub Actions workflow.

## Consuming via FetchContent

Downstream CMake projects can pull in the library targets directly:

```cmake
include(FetchContent)
FetchContent_Declare(
  sql2rdf
  GIT_REPOSITORY https://github.com/nonodename/sql2rdf.git
  GIT_TAG        <tag-or-commit>
)
FetchContent_MakeAvailable(sql2rdf)

target_link_libraries(myapp PRIVATE sql2rdf::r2rml)   # or sql2rdf::yarrrml which will give you both that and r2rml
# target_link_libraries(myapp PRIVATE sql2rdf::sparql)     # standalone SPARQL query parser, unrelated to r2rml/yarrrml
# target_link_libraries(myapp PRIVATE sql2rdf::sparql2sql) # SPARQL-to-SQL translator, pulls in both r2rml and sparql
```

By default this gets you only `sql2rdf_r2rml`/`sql2rdf_yarrrml`/`sql2rdf_sparql`/`sql2rdf_sparql2sql` (exposed under the namespaced `sql2rdf::r2rml`/`sql2rdf::yarrrml`/`sql2rdf::sparql`/`sql2rdf::sparql2sql` ALIAS targets, plus their `serd`/`yaml-cpp` dependencies) — no `test_runner`, no `SQL2RDF++` CLI, no Catch2 fetch, and no DuckDB probing or fetch. The `format`/`format-check`/`tidy` dev-utility targets are also skipped, avoiding a target-name collision with any identically-named targets in the consuming project.

If your own project already defines a `serd` target (e.g. from its own `FetchContent`/`find_package` of Serd), sql2rdf will reuse it instead of vendoring a second copy — just make sure that target exists before `FetchContent_MakeAvailable(sql2rdf)` runs.

If you do want sql2rdf's tests or CLI built as part of your own build, set the corresponding option to `ON` *before* `FetchContent_MakeAvailable`:

```cmake
set(SQL2RDF_BUILD_TESTS ON CACHE BOOL "" FORCE)
set(SQL2RDF_BUILD_CLI ON CACHE BOOL "" FORCE)
```

Installed `find_package()` consumption is not supported — FetchContent/`add_subdirectory`-style source consumption is the only supported integration path.

## Building

**Requires a C++11-compliant compiler** (GCC 5+, Clang 3.4+, MSVC 2015+).

To build the extension, first clone this repo. Then in the repo base locally run:

```sh
git submodule update --init --recursive
```
Then (assuming you already have DuckDB installed on your system)
```sh
cmake -B build
cmake --build build --target sql2rdf_r2rml      # library only
cmake --build build --target sql2rdf_sparql     # SPARQL query parser library only
cmake --build build --target sql2rdf_sparql2sql # SPARQL-to-SQL translator library only
cmake --build build --target SQL2RDF++          # CLI app (requires DuckDB)
cmake --build build --target test_runner        # tests (no DuckDB needed)
cmake --build build                             # all of the above
```

Run the tests:
```sh
cmake --build build --target tests   # build + run
# or
ctest --test-dir build
```

If you have Ninja installed you can generate a Ninja build instead of Make:
```sh
cmake -B build -G Ninja
cmake --build build
```

Standalone builds get the CLI and test suite by default. Pass `-DSQL2RDF_BUILD_TESTS=OFF` and/or `-DSQL2RDF_BUILD_CLI=OFF` to suppress either even in a standalone checkout (see [Consuming via FetchContent](#consuming-via-fetchcontent) for the downstream-consumer defaults).

### Code quality

Requires `clang-format` and `clang-tidy` on `PATH` (e.g. `brew install llvm` or `apt install clang-format clang-tidy`):

```sh
cmake --build build --target format        # apply formatting in-place
cmake --build build --target format-check  # check only (non-zero exit if any file would change)
cmake --build build --target tidy          # run static analysis
```

### DuckDB dependency

The `SQL2RDF++` executable (and the gated `sparql2sql_duckdb_tests` target) requires DuckDB headers and a shared library; the core libraries and `test_runner` do not. For faster CI builds a system-installed DuckDB is assumed by default:

- **macOS**: `brew install duckdb`
- **Debian/Ubuntu**: install `libduckdb-dev`
- **Windows**: download the C/C++ SDK from the [DuckDB install page](https://duckdb.org/install/?platform=windows&environment=c)

To build DuckDB from source instead, pass `-DUSE_EMBEDDED_DUCKDB=ON` at configure time:
```sh
cmake -B build -DUSE_EMBEDDED_DUCKDB=ON
```

When embedding DuckDB, its own shell/CLI and unittest targets are disabled (`BUILD_SHELL`/`BUILD_UNITTESTS` forced `OFF`) so embedding DuckDB doesn't also pull in DuckDB's own test suite or shell binary.

See the GitHub Actions workflow for the exact install steps used in CI for each platform.

## Testing

Tests are based on the example tables and mapping configurations from the [W3C R2RML specification](https://www.w3.org/TR/r2rml/). Example mapping files are in `tests/sourceR2RML/`. YARRRML equivalents of the same examples, plus feature/error-handling fixtures, are in `tests/sourceYARRRML/`. SPARQL query fixtures (valid queries and `invalid_*.rq` error cases) are in `tests/sourceSPARQL/`. SPARQL-to-SQL translator query fixtures, translated against the R2RML fixtures above, are in `tests/sourceSPARQL2SQL/`.

`test_runner` (no DuckDB required) asserts only on the *structural* shape of translator-generated SQL, since it deliberately never links DuckDB. Execution-correctness testing — actually running the translated SQL and checking result rows — lives in the separate `sparql2sql_duckdb_tests` target (`tests/duckdb/`), which requires DuckDB and is built only when the CLI and tests are both enabled and DuckDB is available; its cases also register with CTest, so `ctest --test-dir build` runs both suites together when it's built.

All other tests run against a mock SQL backend (`MockSQL.h`) — no DuckDB installation is required to run them.

## YARRRML support

[YARRRML](https://rml.io/yarrrml/spec/) mapping files (`.yml`/`.yaml`/`.yarrrml`) are translated internally into R2RML Turtle and then parsed by the same R2RML engine used for `.ttl` mappings, so both formats produce identical output for equivalent mappings. Example:

```yaml
prefixes:
  ex: http://example.com/ns#

mappings:
  employee:
    sources:
      - table: EMP
    s: http://data.example.com/employee/$(EMPNO)
    po:
      - [a, ex:Employee]
      - [ex:name, $(ENAME)]
      - [ex:count, $(COUNT), xsd:integer]
      - [ex:nickname, $(NICKNAME), en~lang]
      - [ex:homepage, $(HOMEPAGE)~iri]
```

Supported subset:

- `prefixes`, `base`, `mappings`/`mapping`.
- `sources`/`source` (per-mapping, single entry or list — the first is used) with `table`/`query`; a top-level `sources` map of named sources referenced by name. `access`/`type`/`credentials`/`queryFormulation`/`referenceFormulation` are ignored.
- `subjects`/`subject`/`s` (single entry or list — the first is used): `$(COL)` → column, mixed text → template, otherwise a constant IRI.
- `po`/`predicateobjects`, in shortcut array form (`[predicates, objects]` or `[predicates, objects, datatype-or-language]`) or map form (`predicates`/`predicate`/`p`, `objects`/`object`/`o`). The `a` predicate with a constant class object is folded into `rr:class` on the subject map.
- Object values: `$(COL)` → column (literal by default; `~iri` forces an IRI), mixed text → template, a CURIE/absolute IRI → constant IRI, any other plain string → constant literal. Per-object `{value:|v:, datatype:|language:}` maps and `[value, datatype-or-language]` pairs are supported.
- Mapping references (joins): `{mapping: OTHER, condition(s): {function: equal, parameters: [[str1, $(CHILD)], [str2, $(PARENT)]]}}`.
- `graphs`/`graph`, unknown per-mapping keys, and unknown top-level keys (e.g. `functions`, `targets`) are not supported and are reported as non-fatal warnings (`authors` is ignored silently).

Non-fatal issues (unsupported keys, a mapping with no/multiple sources, an unresolved join-condition function, skipped `graphs`, ...) are collected into `R2RMLMapping::parseErrors` in the default lenient mode, or raised as a `std::runtime_error` when parsing in strict mode (`ignoreNonFatalErrors=false`). Fatal problems (unreadable file, YAML syntax errors, a missing `mappings` key) always throw.

## SPARQL query parsing

`sql2rdf_sparql` (namespace `sparql::`) is a standalone recursive-descent parser for the [SPARQL 1.1 Query grammar](https://www.w3.org/TR/sparql11-query/#sparqlGrammar). It has no dependency on the R2RML/YARRRML/DuckDB code and is not currently used by the mapping-to-RDF conversion pipeline — it exists to parse and inspect `.rq` query files.

Supported: all four query forms (`SELECT`/`CONSTRUCT`/`DESCRIBE`/`ASK`), prologue (`BASE`/`PREFIX`), dataset clauses, group graph patterns (`OPTIONAL`/`MINUS`/`UNION`/`GRAPH`/`SERVICE`/`FILTER`/`BIND`/`VALUES`), subqueries, solution modifiers (`GROUP BY`/`HAVING`/`ORDER BY`/`LIMIT`/`OFFSET`), the full property path algebra (alternative/sequence/inverse/negated property sets), triples including collections and blank-node property lists, the full expression grammar with aggregates (`COUNT`/`SUM`/`MIN`/`MAX`/`AVG`/`SAMPLE`/`GROUP_CONCAT`) and builtin functions, and `EXISTS`. SPARQL *Update* is out of scope.

`sparql::Parser::parseFile`/`parseString` produce an AST (`include/sparql-parser/ast/`); `sparql::print` (`PrettyPrinter.h`) renders it back to text. Parse errors are reported via `sparql::ParseError`.

The CLI exposes this parser directly via `-Q <file.rq>`, which parses the query and prints its AST to stdout, then exits without touching the mapping/database/output pipeline. See [Usage](#usage) below.

## SPARQL-to-SQL translation

`sql2rdf_sparql2sql` (namespace `sparql2sql::`) is the one deliberate bridge between the R2RML/YARRRML mapping pipeline and the standalone SPARQL parser: it translates a parsed SPARQL query against a parsed R2RML mapping into a SQL query, by using the mapping's `TriplesMap`/`PredicateObjectMap`/`TermMap` structure *in reverse*. For each SPARQL triple pattern it enumerates every mapping source that could produce a matching triple and composes the per-pattern SQL relations via the SPARQL algebra (`AND`→inner join, `OPTIONAL`→left outer join, `UNION`→schema-extending union, `MINUS`→anti-join). This lets a SPARQL query run directly against the relational data — no materialized RDF graph in between.

The approach to conversion is based on the work of A. Chebotko et al., Semantics preserving SPARQL-to-SQL translation, Data Knowl. Eng. (2009), `doi:10.1016/j.datak.2009.04.001`

```cpp
#include "sparql-parser/Parser.h"
#include "r2rml/R2RMLParser.h"
#include "sparql2sql/Translator.h"
#include "sparql2sql/DuckDbDialect.h"

sparql::Parser sparqlParser;
std::unique_ptr<sparql::ast::Query> query = sparqlParser.parseFile("query.rq");

r2rml::R2RMLParser mappingParser;
r2rml::R2RMLMapping mapping = mappingParser.parse("mapping.ttl");

sparql2sql::DuckDbDialect dialect;
std::string sql = sparql2sql::translateQuery(*query, mapping, dialect);
// sql is a single "SELECT ..." (or, for ASK, "SELECT EXISTS(...) AS ask") statement,
// ready to hand to r2rml::DuckDBConnection::execute() or any other SQLConnection.
```

Only `SELECT`/`ASK` query forms and a constant-IRI-or-variable predicate position (no property path operators) are supported; every SPARQL variable is represented as a plain SQL `VARCHAR` of the term's lexical form (no term-kind/datatype/language tracking). See `doc/api.md`'s "Supported SPARQL subset / Known limitations" for the full, current list of what is and isn't translated.

The CLI exposes this via `-T <file.rq> [--dialect <name>]` (default and currently only dialect: `duckdb`), paired with the mapping-file positional argument; if the database-file positional is also given, the translated SQL is additionally executed and its result rows printed. See [Usage](#usage) below.

## Usage

```sh
Usage: ./SQL2RDF++ [options] <mapping.ttl|mapping.yml> <database.db> <output.nt>

Arguments:
  mapping.ttl|mapping.yml   R2RML mapping file (Turtle) or YARRRML mapping
                            file (YAML); the format is chosen from the file
                            extension (.ttl -> R2RML, .yml/.yaml/.yarrrml ->
                            YARRRML) unless overridden with -y.
  database.db               DuckDB database file
  output.nt                 Output RDF file

Options:
  -f ntriples|turtle   Output format (default: ntriples); ignored with -T
  -y                   Force the mapping file to be parsed as YARRRML,
                       regardless of its extension
  -P                   Print the parsed mapping to stderr
  -Q <file.rq>         Parse a SPARQL query file and print its AST to
                       stdout, then exit (bypasses the mapping/database/
                       output pipeline entirely)
  -T <file.rq>         Translate a SPARQL query file into SQL against the
                       given R2RML/YARRRML mapping (uses an R2RML mapping in
                       reverse), then exit. Requires the mapping file
                       positional argument; mutually exclusive with -Q.
                       If the database.db positional argument is also given,
                       the translated SQL is additionally executed against it
                       (result rows to stdout, SQL echoed to stderr);
                       otherwise the SQL alone is printed to stdout.
  --dialect <name>     SQL dialect to translate for with -T (default: duckdb;
                       currently the only supported dialect)
  -h                   Show this help message
```

`-Q` and `-T` are independent, mutually-exclusive entry points that bypass the mapping/database/output pipeline used by the default R2RML/YARRRML→RDF conversion above:

```sh
./SQL2RDF++ -Q <query.rq>
# Parses a SPARQL query file and prints its AST to stdout.

./SQL2RDF++ -T <query.rq> <mapping.ttl|mapping.yml> [database.db] [--dialect duckdb]
# Translates the SPARQL query into SQL against the given mapping and prints
# it to stdout; if database.db is also given, executes it and prints the
# result rows instead.
```