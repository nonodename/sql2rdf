# SQL2RDF++

A C++11 project built with CMake. This project will eventually convert SQL data to RDF.

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
