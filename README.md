# LanceDB C bindings

[![CI](https://github.com/lancedb/lancedb-c/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/lancedb/lancedb-c/actions/workflows/ci.yml)
[![Documentation](https://github.com/lancedb/lancedb-c/actions/workflows/docs.yml/badge.svg?branch=main)](https://lancedb.github.io/lancedb-c/)
[![Release](https://img.shields.io/github/v/release/lancedb/lancedb-c?include_prereleases&label=release)](https://github.com/lancedb/lancedb-c/releases)
[![License](https://img.shields.io/badge/license-Apache--2.0-blue.svg)](LICENSE)

C17 FFI bindings for [LanceDB](https://github.com/lancedb/lancedb), usable from
C, C++, and other languages through the C ABI. The public header includes the
Arrow C Data and C Stream structures it needs, so a C program does not need the
Arrow C++ library, Rust, or CMake to use a prebuilt package.

> **Status:** pre-1.0 preview releases (`v0.1.0-preview.*`). The C binding has
> its own version line, independent of the pinned `lancedb` Rust crate. Before
> 1.0 the C ABI may change; recompile C and C++ callers against the published
> header. See [Versioning and ABI](docs/versioning.rst).

Capabilities include embedded/local and remote/cloud connections, Arrow C Data
Interface and Arrow IPC interchange, vector, full-text and hybrid search, scalar
and vector indexes, CRUD and merge-insert, branches and history, and table
optimization. Cloud storage and compile-time feature flags are covered in
[Cloud storage and native features](docs/cloud-storage.rst).

## Supported platforms

The release workflow builds and tests five native targets:

| Platform | Release archive suffix | Notes |
| --- | --- | --- |
| Linux x86_64 | `linux-amd64` | glibc 2.35 or newer |
| Linux arm64 | `linux-arm64` | glibc 2.35 or newer |
| macOS arm64 | `darwin-arm64` | Apple Silicon only; Intel macOS is not supported |
| Windows x86_64 | `windows-amd64` | MSVC import library is `lancedb.dll.lib` |
| Windows arm64 | `windows-arm64` | MSVC import library is `lancedb.dll.lib` |

Linux packages are built on Ubuntu 22.04. The Windows packages use MSVC; the
import library name matters, see [Windows (MSVC)](#windows-msvc).

## Requirements

### Using a prebuilt package

- A C17 compiler, or a C++ compiler for C++ callers.
- No Rust toolchain, CMake, or Apache Arrow C++ installation is required. The
  public header defines the Arrow C ABI structures used by the C entry points.

### Building from source

- Rust 1.98.0 or newer (`rustc` and `cargo`; the minimum declared by the locked
  dependency graph).
- CMake 3.22 or later.
- A C and C++ compiler. CMake enables both languages even when examples and
  tests are off.
- A Protocol Buffers compiler (`protoc`) for the pinned Lance build.
- `pkg-config` only if you want to consume the installed `.pc` file.
- Apache Arrow C++ development headers and libraries, plus C++20, only for the
  examples and tests.

## Install a prebuilt package

From an empty directory, download an archive and its sibling `.sha256` file
from [GitHub Releases](https://github.com/lancedb/lancedb-c/releases), verify
the checksum, and extract it. For example, for the public
`v0.1.0-preview.2` Linux x86_64 package:

```bash
tag=v0.1.0-preview.2
archive=lancedb-c-250aa95153848c6fc6f4af571af70e979fb89ce3-linux-amd64.tar.gz
base="https://github.com/lancedb/lancedb-c/releases/download/${tag}"

curl -fLO "${base}/${archive}"
curl -fLO "${base}/${archive}.sha256"
sha256sum -c "${archive}.sha256"
tar -xzf "${archive}"
root="${PWD}/${archive%.tar.gz}"
```

On macOS use `shasum -a 256 -c "${archive}.sha256"` instead of `sha256sum`.
Substitute the current release tag and archive name from the Releases page; the
archive name contains the packaged commit and the platform suffix from the
table above. The release also publishes a combined `SHA256SUMS` and an internal
`SHA256SUMS` inside each archive.

The extracted package contains:

```text
lancedb-c-<commit>-<target>/
├── LICENSE
├── include/lancedb.h
├── lib/liblancedb.so        # Linux linker name
├── lib/liblancedb.so.0.1    # Linux SONAME
├── manifest.json
└── SHA256SUMS
```

macOS uses `lib/liblancedb.dylib`; Windows uses `lib/lancedb.dll` and
`lib/lancedb.dll.lib`. The manifest records the version, commit, target and
package-relative paths.

The `v0.1.0-preview.2` archives do not contain a CMake package config or a
pkg-config file. Use the `-I`/`-L` commands below for that archive. A current
source build installs both, as described in
[Build and install from source](#build-and-install-from-source).

Save this complete C17 program as `hello.c` in the same directory. It
includes only `<lancedb.h>`, connects to a local database, prints the runtime
version, and frees every handle. The archive contains the header and library,
but not example sources.

```c
#include <lancedb.h>

#include <stdio.h>

int main(int argc, char** argv) {
  const char* uri = argc > 1 ? argv[1] : "./lancedb-c-example-db";

  printf("lancedb-c %s\n", lancedb_version());

  LanceDBConnectBuilder* builder = lancedb_connect(uri);
  if (builder == NULL) {
    fprintf(stderr, "failed to create a connection builder for %s\n", uri);
    return 1;
  }

  LanceDBConnection* connection = NULL;
  char* error_message = NULL;
  const LanceDBError status =
      lancedb_connect_builder_execute(builder, &connection, &error_message);
  if (status != LANCEDB_SUCCESS) {
    fprintf(stderr, "connect failed: %s\n",
            error_message != NULL ? error_message
                                  : lancedb_error_to_message(status));
    lancedb_free_string(error_message);
    return 1;
  }

  printf("connected to %s\n", lancedb_connection_uri(connection));
  lancedb_connection_free(connection);
  return 0;
}
```

### Linux and macOS

Compile the saved `hello.c` against the extracted `include/` and `lib/`:

```bash
cc -std=c17 -Wall -Wextra -Werror -pedantic hello.c \
  -I"$root/include" -L"$root/lib" -llancedb \
  -Wl,-rpath,"$root/lib" -o hello
./hello ./example-db
```

`-Wl,-rpath` records the package library directory in the executable. Without
it, set `LD_LIBRARY_PATH` on Linux or `DYLD_LIBRARY_PATH` on macOS before
running. The macOS dylib uses an `@rpath/liblancedb.dylib` install name, so the
same `-rpath` form works for a package extracted anywhere.

### Windows (MSVC)

Use the **x64 Native Tools Command Prompt for VS** for `windows-amd64`, or the
**arm64 Native Tools Command Prompt for VS** for `windows-arm64`. The compiler
architecture must match the package. The import library is `lancedb.dll.lib`;
`-llancedb` does not select it under MSVC.

```bat
set "root=%CD%\lancedb-c-250aa95153848c6fc6f4af571af70e979fb89ce3-windows-amd64"
cl /nologo /std:c17 /W4 /I"%root%\include" hello.c ^
  /Fe:hello.exe /link /LIBPATH:"%root%\lib" lancedb.dll.lib
copy "%root%\lib\lancedb.dll" .
hello.exe .\example-db
```

The `windows-arm64` archive extracts to
`lancedb-c-250aa95153848c6fc6f4af571af70e979fb89ce3-windows-arm64`; use that
directory with the ARM64 prompt. For another release, replace the path with the
directory actually extracted.

The loader must find `lancedb.dll` at runtime. Copy it next to the executable
(as above) or add `%root%\lib` to `PATH`. A CMake consumer of an installed
source build links the installed `lancedb::lancedb` target instead; the target
links `lancedb.dll.lib` from the installation `lib` directory and expects the
DLL in the installation `bin` directory.

## Build and install from source

Build the shared library:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

`BUILD_SHARED_LIBS` defaults to `ON`. For a static library:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF
cmake --build build
```

Install into a prefix. `GNUInstallDirs` controls the layout, so the library,
header, pkg-config file and CMake package follow `CMAKE_INSTALL_LIBDIR` and
`CMAKE_INSTALL_INCLUDEDIR`:

```bash
cmake --install build --prefix "$PWD/install"
```

The current source install contains a relocatable CMake package and
`lancedb.pc`. The CMake package is under
`${CMAKE_INSTALL_LIBDIR}/cmake/lancedb`; pkg-config is under
`${CMAKE_INSTALL_LIBDIR}/pkgconfig`.

### CMake consumer

```cmake
cmake_minimum_required(VERSION 3.22)
project(my_app C)

find_package(lancedb CONFIG REQUIRED)
add_executable(my_app main.c)
target_link_libraries(my_app PRIVATE lancedb::lancedb)
```

Point CMake at the prefix with `-DCMAKE_PREFIX_PATH="$PWD/install"` or
`-Dlancedb_DIR="$PWD/install/lib/cmake/lancedb"`. Use unversioned discovery for
preview packages: CMake cannot express a
prerelease in a version request, so `find_package(lancedb 0.1.0)` is rejected
for `0.1.0-preview.*`. The imported target handles include and link paths, but
not the runtime loader path. On Unix, install into a loader path, set an RPATH
in the consumer, or set `LD_LIBRARY_PATH`/`DYLD_LIBRARY_PATH` at runtime. On
Windows, the DLL is in the installation `bin` directory; copy it next to the
executable or add `bin` to `PATH`.

### pkg-config consumer

```bash
export PKG_CONFIG_PATH="$PWD/install/lib/pkgconfig"
cc -std=c17 -Wall -Wextra -Werror -pedantic main.c \
  $(pkg-config --cflags --libs lancedb) \
  -Wl,-rpath,"$(pkg-config --variable=libdir lancedb)" -o my_app
```

If the install used `lib64`, point `PKG_CONFIG_PATH` at
`$PWD/install/lib64/pkgconfig` and use the same `pkg-config` commands. For a
static install, add `--static` so `Libs.private` supplies the native link
dependencies.

## C++ and Apache Arrow

The repository's [`examples/hello.c`](examples/hello.c) is the same C17 program
shown in the prebuilt quick start. It exists only in a source checkout; the
release archives do not include example sources.

[`examples/simple.cpp`](examples/simple.cpp) is a complete C++20 program that
creates a table with a fixed-size-list vector column, inserts rows through the
Arrow C Data Interface, creates an IVF_FLAT vector index, and runs a nearest
neighbor search. Build the examples with Arrow C++ installed:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_EXAMPLES=ON
cmake --build build
./build/simple
```

With a multi-config generator such as Visual Studio or Xcode, build with
`cmake --build build --config Release` and run from the configuration
subdirectory. On Windows the build-tree DLL stays in `build/target/release`; add
that directory to `PATH` before running an example. The Boost.Asio coroutine
example is optional:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_EXAMPLES=ON \
  -DBUILD_ASIO_COROUTINE_EXAMPLE=ON
cmake --build build
```

Arrow C++ is required only to build the examples and tests or to prepare Arrow
data in the caller. The C library itself does not link Arrow C++.

### Examples

| File | What it demonstrates | Extra requirements |
| --- | --- | --- |
| [`examples/hello.c`](examples/hello.c) | Minimal C17 connect, version, error and handle cleanup | C17 compiler |
| [`examples/simple.cpp`](examples/simple.cpp) | Create, insert, vector index and nearest-neighbor search | C++20, Arrow C++ |
| [`examples/full.cpp`](examples/full.cpp) | Broader CRUD, scalar and vector indexes, merge-insert, optimize, stats and index metadata | C++20, Arrow C++ |
| [`examples/s3.cpp`](examples/s3.cpp) | Connect to S3 with storage options | C++20, Arrow C++, S3 endpoint and credentials |
| [`examples/asio_coroutine.cpp`](examples/asio_coroutine.cpp) | Structure upsert, delete and query work as Boost.Asio coroutines | C++20, Arrow C++, Boost coroutine/context, `BUILD_ASIO_COROUTINE_EXAMPLE` |
| [`examples/lancedb_registry.cpp`](examples/lancedb_registry.cpp) | Custom object-store registry and session | C++20, Arrow C++, external provider crate |
| [`examples/external_provider/`](examples/external_provider/) | Rust `ObjectStoreProvider` used by the registry example | Rust |

## API conventions

### Errors

Fallible functions return `LanceDBError`; `LANCEDB_SUCCESS` is zero. Check the
return code before using any output. A function with a `char** error_message`
parameter may return a heap-allocated message; free it with
`lancedb_free_string()`, which accepts `NULL`. Use
`lancedb_error_to_message()` when no detailed message is available. Do not
assume every function clears an existing error slot; follow the comment above
the function.

### Ownership and lifetimes

Opaque handles are freed by their matching `*_free()` function. Builder setters
consume the builder and return the replacement, so assign the return value back
to the same variable. Executing a builder consumes it on success and failure.
`lancedb_connect_builder_execute()` consumes a non-NULL builder on both paths;
the connection output is written only on success.

Record-batch reader ownership is per entry point. A call can consume the reader
and still return an error, so do not free it after a call that takes it.

| Entry point | Reader ownership |
| --- | --- |
| `lancedb_table_create()` | Transfers a non-NULL reader only after the required pointers, UTF-8 table name and schema import succeed. Failures before that leave it caller-owned; after schema import it is consumed even if later validation or creation fails. |
| `lancedb_table_create_with_options()` | Consumes a non-NULL reader as soon as it is called, including when another argument, an option, the schema or the SDK rejects the create. |
| `lancedb_table_add()` | Consumes a non-NULL reader after the table and reader NULL checks. A NULL table or reader leaves it caller-owned. |
| `lancedb_table_add_with_options()` | Consumes a non-NULL reader before validating the table or options. A NULL reader is not consumed. |
| `lancedb_table_merge_insert*()` | Consumes a non-NULL data reader before validating the table, columns, config or options. A NULL data pointer is not consumed. |

After a call consumes a reader, do not call
`lancedb_record_batch_reader_free()` on it, even when the call returned an
error. Set the caller's pointer to NULL after the call when a single cleanup
path needs it. The full rules, including the Arrow array split at schema
import, are in [Memory ownership](docs/ownership.rst) and the header comments.

### Arrow interchange

The public header defines `ArrowSchema`, `ArrowArray` and `ArrowArrayStream`, so
pure C consumers do not need Arrow C++ headers.

- **Arrow C Data Interface** is the zero-copy path for record batches produced by
  Arrow C++, nanoarrow, DuckDB or another Arrow implementation. Import with
  `lancedb_record_batch_reader_from_arrow()`. If schema import fails, the array
  is still caller-owned; once schema import succeeds, the binding owns the
  array even if a later conversion fails. The schema stays caller-owned.
- **Arrow C Stream** is for a producer that supplies batches lazily. Import with
  `lancedb_record_batch_reader_from_arrow_stream()`; the schema is read
  synchronously, and later callbacks may run on SDK worker threads.
- **Arrow IPC** is for bytes already encoded as an IPC file. Use
  `lancedb_record_batch_reader_from_ipc_file()` to read batches and/or a schema.
  Query results can be converted to Arrow arrays or to an IPC buffer with the
  result conversion functions.

See [Arrow interoperability](docs/api/arrow.rst) and
[Memory ownership](docs/ownership.rst).

### Threading and unwinding

The API does not define a general rule for sharing a handle between threads. Do
not share a handle or mutable input unless the function comment permits it.
Exported entry points are non-unwinding `extern "C"` functions; a Rust panic
that reaches the boundary aborts the process. Do not let C++ exceptions or
`longjmp` escape a callback. Arrow C Stream callbacks may run on SDK worker
threads. See [Threading and unwinding](docs/threading.rst).

### Versioning

The release version comes from the Git tag; the Cargo package version stays
`0.0.0`. The installed header exposes `LANCEDB_VERSION_MAJOR`,
`LANCEDB_VERSION_MINOR`, `LANCEDB_VERSION_PATCH` and `LANCEDB_VERSION_STRING`;
`lancedb_version()` returns the runtime version from the loaded library. The
checked-in header reports `0.0.0-dev`. See
[Versioning and ABI](docs/versioning.rst).

## Documentation and resources

- Hosted API documentation: <https://lancedb.github.io/lancedb-c/>
- Public header: [`include/lancedb.h`](include/lancedb.h)
- Ownership: [docs/ownership.rst](docs/ownership.rst)
- Threading: [docs/threading.rst](docs/threading.rst)
- Versioning and ABI: [docs/versioning.rst](docs/versioning.rst)
- Cloud storage and feature flags: [docs/cloud-storage.rst](docs/cloud-storage.rst)
- Issues and feature requests: <https://github.com/lancedb/lancedb-c/issues>

## Development

Build, test and documentation checks are in [CONTRIBUTING.md](CONTRIBUTING.md).
The release workflow, package contract, retry recovery and tag-immutability
rules are in [RELEASING.md](RELEASING.md).

## License

[Apache-2.0](LICENSE).
