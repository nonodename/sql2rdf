# SQL2RDF++

A command line utility to convert relational database tables to RDF using R2RML syntax.

Ultimately this is intended to prove out implementation of R2RML in C++ so that the code can be lifted into a DuckDB extension to support a `COPY TO` export from DuckDB.

## Build targets

The project is structured as a reusable library plus a thin CLI application:

| Target | Type | DuckDB dependency | Description |
|--------|------|-------------------|-------------|
| `sql2rdf_r2rml` | static library | none | Core R2RML implementation. Links only [Serd](https://drobilla.net/software/serd/) (embedded). Suitable for use in other projects, including a DuckDB extension. |
| `SQL2RDF++` | executable | required | CLI application. Compiles the DuckDB adapter (`DuckDBConnection`) and links the system or embedded DuckDB library. |
| `test_runner` | executable | none | Test suite using [Catch2](https://github.com/catchorg/Catch2). All tests run against a mock SQL backend — no DuckDB required. |
| `format` | utility | none | Apply `clang-format` to all project C++ sources in-place. |
| `format-check` | utility | none | Check formatting with `clang-format --dry-run --Werror`; exits non-zero if any file would change. Used in CI. |
| `tidy` | utility | none | Run `clang-tidy` static analysis using `.clang-tidy`. Builds `sql2rdf_r2rml` first to ensure a fresh compilation database. |

The [Serd](https://drobilla.net/software/serd/) RDF syntax library is included as a git submodule under `external/serd` and compiled from source into the `sql2rdf_r2rml` library.

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

See the GitHub Actions workflow for the exact install steps used in CI for each platform.

## Testing

Tests are based on the example tables and mapping configurations from the [W3C R2RML specification](https://www.w3.org/TR/r2rml/). Example mapping files are in `tests/sourceR2RML/`.

All tests run against a mock SQL backend (`MockSQL.h`) — no DuckDB installation is required to run them.

## Usage

```sh
Usage: ./SQL2RDF++ [options] <mapping.ttl> <database.db> <output.nt>

Arguments:
  mapping.ttl    R2RML mapping file (Turtle format)
  database.db    DuckDB database file
  output.nt      Output RDF file

Options:
  -f ntriples|turtle   Output format (default: ntriples)
  -P                   Print the parsed mapping to stderr
  -h                   Show this help message
```