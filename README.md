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
