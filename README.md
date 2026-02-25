# SQL2RDF++

A project built with CMake that will eventually convert SQL data to RDF.

**Requires a C++11-compliant compiler** (GCC 4.8+, Clang 3.3+, MSVC 2015+).

The repository now includes the [Serd](https://drobilla.net/software/serd/) RDF syntax
library as a git submodule under `external/serd`. The CMake build system compiles Serd
from source and links a static `serd` library into the `SQL2RDF++` executable.
A simple demonstration of calling `serd_strlen` and `serd_strerror` is used in
`src/main.cpp` to prove the dependency is working.

To build:

```sh
mkdir build && cd build
cmake ..
make
./SQL2RDF++
```

## Building

```bash
mkdir build && cd build
cmake ..
make
```

Notes:
- This project links to DuckDB at runtime. For faster CI builds we assume a
	system-installed DuckDB (headers + library) is available. On Debian/Ubuntu
	install the dev package `libduckdb-dev` (or `duckdb`) and on macOS use
	`brew install duckdb`.
- If you prefer building an embedded DuckDB from source, pass the CMake option
	`-DUSE_EMBEDDED_DUCKDB=ON` when configuring the build.

The project's GitHub Actions workflow now installs DuckDB on runners before
configuring CMake, so CI runs will use the system package by default.
