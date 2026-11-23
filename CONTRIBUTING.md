# Contributing

The public C API is a hand-written header backed by the Rust FFI. Keep the two
in the same change, and keep the C17 baseline in `include/lancedb.h` free of C++
constructs.

## Build and test

The build prerequisites are listed in the [README](README.md#prerequisites).
A local build with the C++ tests and examples enabled:

```bash
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_EXAMPLES=ON \
  -DBUILD_TESTS=ON
cmake --build build
ctest --test-dir build -j --output-on-failure
```

`BUILD_EXAMPLES` and `BUILD_TESTS` default to `OFF`. The optional Boost.Asio
example needs `-DBUILD_ASIO_COROUTINE_EXAMPLE=ON`.

## Checks used by CI

These targets match the commands in `.github/workflows/ci.yml`:

```bash
ninja -C build check          # cargo fmt --check and cargo clippy -D warnings
ninja -C build check-c17      # strict C17 consumer and C17 example
ninja -C build check-abi      # cbindgen plus clang, host and MSVC LLP64
ninja -C build check-install  # installed CMake and pkg-config consumers
```

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

## Public header changes

- Change the Rust FFI, the public header and the C/C++ coverage together.
- Treat C17 as the public header baseline. `check-c17` compiles the header both
  through the generated include directory and directly from `include/`.
- Run `check-abi` after any declaration, struct layout or enum change. It
  compares the header with cbindgen output for the host data model and the
  Windows MSVC LLP64 model.
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

The packaging layout and release rules are documented in
[docs/versioning.rst](docs/versioning.rst) and the README. Release publication
is limited to preview tags and is driven by the `Shared Library Packages`
workflow; do not publish a stable release as part of an ordinary change.
