# Contributing

The public C API is a hand-written header backed by the Rust FFI. Keep the two
in the same change, and keep the C17 baseline in `include/lancedb.h` free of C++
constructs.

## Repository layout

```text
src/                # Rust FFI implementation
include/lancedb.h   # Public C17 header with Arrow C ABI structures
examples/           # C17 and C++ examples, including the external provider crate
tests/              # Catch2 suites and C17/consumer smoke tests
docs/               # Sphinx site and Doxygen group pages
cmake/              # CMake probe, config and linker helpers
.github/            # CI, packaging, release and ABI scripts
RELEASING.md        # Maintainer release runbook
CMakeLists.txt      # Build, install and check targets
```

## Build and test

### Prerequisites

Building the library and tests requires the source-build prerequisites in the
[README](README.md#build-and-install-from-source). In addition:

- Catch2 is fetched automatically when no suitable package is installed.
- `valgrind` is optional; when installed, several test suites run under it and
  fail on memory errors or definite leaks.
- Boost coroutine and context only for the optional Asio example.
- Doxygen, Sphinx and Breathe for the documentation target.

### Configure, build and test

A local build with the C++ tests and examples enabled:

```bash
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_EXAMPLES=ON \
  -DBUILD_TESTS=ON
cmake --build build
ctest --test-dir build -j 6 --output-on-failure
```

`BUILD_EXAMPLES` and `BUILD_TESTS` default to `OFF`. The optional Boost.Asio
example needs `-DBUILD_ASIO_COROUTINE_EXAMPLE=ON` in addition to
`-DBUILD_EXAMPLES=ON`.

### Manual shared-library build

The supported shared-library entry points are CMake, the package workflow, and
the export-controlled Cargo command. A plain `cargo build` is still the normal
path for static, rlib and external-provider development, but it does not apply
the shared-library export boundary.

On Unix:

```bash
cargo rustc --release -- -C linker=./cmake/export-linker.sh
```

On Windows, run the equivalent from a Visual Studio developer shell with
`-C linker=.\cmake\export-linker.cmd`.

The export boundary and paired import-library verification cover Linux ELF,
macOS Mach-O and native Windows MSVC shared builds. Windows GNU shared builds
and their installed `liblancedb.dll.a` consumers remain supported, but Windows
GNU is not a release target and does not use the MSVC export policy.

### Manual example build

The CMake example targets are the supported build path. To compile the simple
example by hand against the manual shared-library build and Arrow C++:

```bash
g++ $(pkg-config --cflags arrow) -std=c++20 -Wall -Wextra -O2 \
    -I./include \
    -o examples/simple examples/simple.cpp \
    -L./target/release -llancedb \
    $(pkg-config --libs arrow) \
    -Wl,-rpath,./target/release
./examples/simple
```

## Checks used by CI

These targets are the local entry points for the checks CI runs:

```bash
ninja -C build check          # cargo fmt --check and cargo clippy -D warnings
ninja -C build check-c17      # strict C17 consumer and C17 example
ninja -C build check-abi      # cbindgen plus clang, host and MSVC LLP64
ninja -C build check-install  # installed C and C++ find_package consumers
```

The dedicated `Lint and ABI checks` job runs `cargo fmt --check`,
`cargo clippy --all-targets -- -D warnings`, both `check-abi` data models, the
packaging and release fixtures, and a diff-scoped `git clang-format` check on
changed C and C++ files. The native build matrix no longer repeats the Rust
format, lint or ABI checks.

`check-install` stages the current build into a temporary prefix and runs C and
C++ `find_package(lancedb CONFIG)` consumers. It does not exercise pkg-config;
the README documents that consumer path separately.

`check-abi` needs the pinned cbindgen version and clang:

```bash
cargo install cbindgen --version =0.29.4 --locked
```

The packaging and release fixture checks do not build the Rust library:

```bash
python3 .github/scripts/test_package.py
python3 .github/scripts/test_release.py
```

Native Windows uses a Debug shared build with `BUILD_CONSUMER_TEST=ON` and runs
the C17 and package consumers directly. Keep the Windows DLL directory on
`PATH` when running them outside CMake.

### C17 and C ABI coverage

`check-c17` compiles the public header through the generated include directory
and directly from `include/`. It compiles the strict C17 consumer and the
`examples/hello.c` example with `-Wall -Wextra -Werror -pedantic` on GCC/Clang
or `/W4 /WX` on MSVC.

`check-abi` compares the hand-written header with pinned cbindgen output for the
host data model and the Windows MSVC LLP64 model. It does not build a Windows
Rust library. The target also runs temporary signature, layout and enum drift
fixtures to show that a mismatch is reported with the affected function,
argument, struct field or enumerator. Native C17 consumers remain platform
runtime smoke tests rather than full declaration coverage. Native Windows MSVC
runs the C17 consumer and the package consumer in CI.

## Public header changes

- Change the Rust FFI, the public header and the C/C++ coverage together.
- Run `check-abi` after any declaration, struct layout or enum change.
- A change that only adds or edits Doxygen comments must not change the public
  declaration or export manifest. Verify that before opening a documentation
  pull request.
- The checked-in header reports `0.0.0-dev`. Do not replace that block with a
  release version; CMake and the package builder render the tag-derived version.

## Documentation

The Sphinx site is built from `docs/` and the Doxygen groups in the header:

```bash
python3 -m venv /tmp/lancedb-c-docs
/tmp/lancedb-c-docs/bin/pip install -r docs/requirements.txt
export PATH="/tmp/lancedb-c-docs/bin:$PATH"
cmake -S . -B build-docs -DBUILD_DOCS=ON
cmake --build build-docs --target docs
```

Generated HTML is written to `build-docs/docs/html`. Doxygen must be installed.
`docs/api/*.rst` contains one page per Doxygen group; add or rename a group in
the header and the matching page together.

## Packaging and releases

The public versioning, ABI and package layout are documented in
[docs/versioning.rst](docs/versioning.rst). The maintainer release workflow,
package publication, retry recovery and tag-immutability rules are in
[RELEASING.md](RELEASING.md).

Release publication is limited to preview tags and is driven by the
`Shared Library Packages` workflow; do not publish a stable release as part of
an ordinary change.
