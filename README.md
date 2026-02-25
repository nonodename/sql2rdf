# SQL2RDF++

A command line utility to convert relational database tables to RDF using R2RML syntax.

Utlimately this is intended to prove out implementation of R2RML in C++ so that the code can be lifted into a DuckDB extension to support a `COPY TO` export from Duck.

## Building

**Requires a C++11-compliant compiler** (GCC 4.8+, Clang 3.3+, MSVC 2015+).

The repository now includes the [Serd](https://drobilla.net/software/serd/) RDF syntax
library as a git submodule under `external/serd`. The CMake build system compiles Serd
from source and links a static `serd` library into the `SQL2RDF++` executable.

To build:

```sh
mkdir build && cd build
cmake ..
make
make tests
./SQL2RDF++
```

If you have ninja installed you can also make by saying
```sh
GEN=ninja make
```

Notes:
- This project links to DuckDB at runtime. For faster CI builds we assume a
	system-installed DuckDB (headers + library) is available. On Debian/Ubuntu
	install the dev package `libduckdb-dev` (or `duckdb`) and on macOS use
	`brew install duckdb`.
- If you prefer building an embedded DuckDB from source, pass the CMake option
	`-DUSE_EMBEDDED_DUCKDB=ON` when configuring the build.

See the GitHub actions for how to configure Duck libraries for your OS and/or the DuckDB (site)[https://duckdb.org/install/?platform=macos&environment=c].

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