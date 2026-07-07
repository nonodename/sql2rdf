# SQL2RDF++

A command line utility to convert relational database tables to RDF using R2RML syntax, with support for [YARRRML](https://rml.io/yarrrml/spec/) as a friendlier YAML front-end to R2RML.

Ultimately this is intended to prove out implementation of R2RML in C++ so that the code can be lifted into a DuckDB extension to support a `COPY TO` export from DuckDB.

## Build targets

The project is structured as a reusable library plus a thin CLI application:

| Target | Type | DuckDB dependency | Description |
|--------|------|-------------------|-------------|
| `sql2rdf_r2rml` | static library | none | Core R2RML implementation. Links only [Serd](https://drobilla.net/software/serd/) (embedded). Suitable for use in other projects, including a DuckDB extension. |
| `sql2rdf_yarrrml` | static library | none | YARRRML → R2RML translator. Publicly links `sql2rdf_r2rml` and privately links [yaml-cpp](https://github.com/jbeder/yaml-cpp) (fetched via CMake `FetchContent`), so consumers of `sql2rdf_r2rml` alone stay free of the YAML dependency. |
| `SQL2RDF++` | executable | required | CLI application. Compiles the DuckDB adapter (`DuckDBConnection`) and links the system or embedded DuckDB library. Gated by `SQL2RDF_BUILD_CLI` (default: ON when building standalone, OFF when consumed via `FetchContent`). |
| `test_runner` | executable | none | Test suite using [Catch2](https://github.com/catchorg/Catch2). All tests run against a mock SQL backend — no DuckDB required. Gated by `SQL2RDF_BUILD_TESTS` (default: ON when building standalone, OFF when consumed via `FetchContent`). |
| `format` | utility | none | Apply `clang-format` to all project C++ sources in-place. Only defined when building standalone. |
| `format-check` | utility | none | Check formatting with `clang-format --dry-run --Werror`; exits non-zero if any file would change. Used in CI. Only defined when building standalone. |
| `tidy` | utility | none | Run `clang-tidy` static analysis using `.clang-tidy`. Builds `sql2rdf_r2rml` first to ensure a fresh compilation database. Only defined when building standalone. |

The [Serd](https://drobilla.net/software/serd/) RDF syntax library is included as a git submodule under `external/serd` and compiled from source into the `sql2rdf_r2rml` library.

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

target_link_libraries(myapp PRIVATE sql2rdf_r2rml)   # or sql2rdf_yarrrml which will give you both that and r2rml
```

By default this gets you only `sql2rdf_r2rml`/`sql2rdf_yarrrml` (and their `serd`/`yaml-cpp` dependencies) — no `test_runner`, no `SQL2RDF++` CLI, no Catch2 fetch, and no DuckDB probing or fetch. The `format`/`format-check`/`tidy` dev-utility targets are also skipped, avoiding a target-name collision with any identically-named targets in the consuming project.

If you do want sql2rdf's tests or CLI built as part of your own build, set the corresponding option to `ON` *before* `FetchContent_MakeAvailable`:

```cmake
set(SQL2RDF_BUILD_TESTS ON CACHE BOOL "" FORCE)
set(SQL2RDF_BUILD_CLI ON CACHE BOOL "" FORCE)
```

Installed `find_package()` consumption is not supported — FetchContent/`add_subdirectory`-style source consumption is the only supported integration path.

## Building

**Requires a C++14-compliant compiler** (GCC 5+, Clang 3.4+, MSVC 2015+).

To build the extension, first clone this repo. Then in the repo base locally run:

```sh
git submodule update --init --recursive
```
Then (assuming you already have DuckDB installed on your system)
```sh
cmake -B build
cmake --build build --target sql2rdf_r2rml   # library only
cmake --build build --target SQL2RDF++        # CLI app (requires DuckDB)
cmake --build build --target test_runner      # tests (no DuckDB needed)
cmake --build build                           # all of the above
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

The `SQL2RDF++` executable (but not the library or tests) requires DuckDB headers and a shared library. For faster CI builds a system-installed DuckDB is assumed by default:

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

Tests are based on the example tables and mapping configurations from the [W3C R2RML specification](https://www.w3.org/TR/r2rml/). Example mapping files are in `tests/sourceR2RML/`. YARRRML equivalents of the same examples, plus feature/error-handling fixtures, are in `tests/sourceYARRRML/`.

All tests run against a mock SQL backend (`MockSQL.h`) — no DuckDB installation is required to run them.

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
  -f ntriples|turtle   Output format (default: ntriples)
  -y                   Force the mapping file to be parsed as YARRRML,
                       regardless of its extension
  -P                   Print the parsed mapping to stderr
  -h                   Show this help message
```