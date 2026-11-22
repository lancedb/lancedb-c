# LanceDB C Bindings

This repository contains comprehensive C FFI bindings for LanceDB, allowing you to use all LanceDB functionality from C and C++ applications.

## Project Structure

```
├── src/                    # Rust FFI implementation
│   ├── lib.rs              # Main library entry point
│   ├── connection.rs       # Connection management
│   ├── table.rs            # Table operations and data manipulation
│   ├── query.rs            # Complete query API implementation
│   ├── index.rs            # Index management
│   ├── error.rs            # Error handling and reporting
│   └── types.rs            # Type definitions and conversions
├── include/
│   └── lancedb.h           # Complete C header file with Arrow C ABI
├── examples/
│   ├── full.cpp            # C++ example using Arrow. Covering most of the API
│   └── simple.cpp          # C++ example using Arrow. Similar to rust/examples/simple.rs
├── tests/                  # C++ unit tests using Catch2
├── docs/                   # Documentation definitions
├── Cargo.toml              # Rust crate configuration
├── CMakeLists.txt          # CMake build configuration
└── README.md               # This file
```

## Building

### Prerequisites

- Rust 1.95.0 or newer (`rustc` and `cargo`; the minimum declared by the locked dependency graph)
- CMake (3.15 or later)
- C++ compiler with C++20 support (gcc, clang)
- Apache Arrow C++ library
- pkg-config

### Quick Start

1. **Build everything using CMake:**
   ```bash
   mkdir -p build
   cd build
   cmake ..
   make
   ```

2. **Run the examples:**
   ```bash
   ./simple
   ./full
   ```

### Manual Build Process

If you prefer to build manually:

1. **Build the Rust library:**
   ```bash
   cargo build --release
   ```

2. **Compile the C++ simple example with Arrow:**
   ```bash
   g++ -std=c++20 -Wall -Wextra -O2 \
       -I./include \
       $(pkg-config --cflags arrow) \
       -o examples/simple examples/simple.cpp \
       -L./target/release -llancedb \
       $(pkg-config --libs arrow) \
       -Wl,-rpath,./target/release
   ```

3. **Run the example:**
   ```bash
   ./examples/simple
   ```

## Prebuilt Shared-Library Packages

The `Shared Library Packages` workflow builds and tests five native targets:
`linux-amd64`, `linux-arm64`, `darwin-arm64` (Apple Silicon), `windows-amd64`
and `windows-arm64`. macOS packages are Apple Silicon only; Intel macOS is not
supported. The workflow runs for `v*` release tags and on explicit dispatch;
the fast packaging fixture checks still run on pull requests through `CI`.
Each package is named
`lancedb-c-<commit>-<target>.tar.gz` and contains the shared library, the
public header, the license, the Windows import library when the platform needs
one, and a `manifest.json` describing the paths. A `SHA256SUMS` file covers the
package contents; the archive has a sibling `.sha256` file.

The C binding has its own version line, independent of the Cargo package and
the pinned SDK. The Cargo package version is fixed at `0.0.0`; the release
version comes from the Git tag. Pull-request and untagged local builds report
`0.0.0-dev` in CMake, pkg-config, documentation and `manifest.json`.

Tags use strict SemVer with a `v` prefix. If several valid tags point at the
same commit, local builds prefer the last stable tag in Git's version sort,
otherwise the last prerelease; a tag dispatch always uses the explicitly
selected tag. `v0.0.0-dev` is reserved for the untagged fallback.

To prepare a release, create an annotated or lightweight tag such as
`v0.1.0-preview.1` on the exact commit and dispatch `Shared Library Packages`
on that tag. The workflow fetches the tag, requires its peeled commit to be the
checked-out commit, and uses the tag version for the build, package manifest and
consumer verification. A tag on a parent or another branch is rejected. CMake
reads the tag when it configures, so after creating or moving a tag in an
existing build directory, rerun the CMake configuration step before building.
CMake passes its configure-time version to the docs target through a private
variable, so a different build-time `LANCEDB_VERSION` does not change an already
configured docs build.

Tag pushes and dispatches without `publish_release` only build the CI
artifacts. To publish, dispatch on the exact preview tag with the
`publish_release` Boolean enabled. The workflow waits for all five package jobs,
re-verifies every archive and checksum against the tag, creates a draft release,
uploads the five archives, their `.sha256` files and combined `SHA256SUMS`, and
only then publishes it. It records the tag ref object, peeled commit, locked
LanceDB and Lance revisions and every archive checksum. Publication is limited
to preview tags; stable releases remain a manual decision for Xuanwo. A retry
reuses a draft, and a published release is never replaced.

If an upload is interrupted after GitHub creates an empty `starter` asset, the
workflow fails closed and names the placeholder. Inspect the exact draft with
`gh release view <tag> --json databaseId,isDraft,assets`; confirm the named
asset is the empty `starter` (`size` 0, no digest), then remove only that asset
with `gh release delete-asset <tag> <name> --yes`. Keep every other draft asset
and re-dispatch the same exact tag; the workflow uploads only missing assets.

Tag ref reads and release writes are separate GitHub operations. The workflow
rechecks the exact remote tag before draft writes and again before publication,
but a tag move in the remaining interval cannot be made atomic from the
workflow. An atomic guarantee needs a tag ruleset that blocks updates and
deletions together with release immutability; this repository does not
currently enable those protections.

The `target` names match Go's `GOOS-GOARCH` spelling and the manifest is the
contract the future pure-Go loader will consume. Release notes for a published
preview record the tag ref object and peeled commit, the pinned SDK revision and
the SHA-256 of each archive.

## Tests

### Prerequisites

- Tests use the Catch2 framework. No need to install it as it will be fetched as part of the build process
- If valgrind is installed, some tests will run under valgrind and will fail on memory errors or definite memory leaks

### Building and Running Tests

1. **Build Tests**
   ```bash
   mkdir -p build
   cd build
   cmake .. -DBUILD_TESTS=ON
   make
   ```

2. **Run Tests**
   ```bash
   ctest -j 6
   ```

### C ABI Declarations

The public header is hand-written and must match the Rust FFI. Generate
declarations from the Rust source with the pinned `cbindgen` and compare them
with the header using `clang`:

```bash
cargo install cbindgen --version =0.29.4 --locked
ninja -C build check-abi
```

The check compares the host data model and the Windows MSVC LLP64 model using
Clang target headers; it does not build a Windows Rust library. It also runs
temporary signature, layout and enum drift fixtures to show that a mismatch is
reported with the affected function, argument, struct field or enumerator.
Native C17 consumers remain platform runtime smoke tests rather than full
declaration coverage, and MSVC C17 CI is owned by #107.

### Code Coverage

Generate a Rust code coverage report showing which `src/*.rs` lines are exercised by the C++ tests.

**Dependencies:** `llvm` (provides `llvm-profdata` and `llvm-cov`)
- RHEL/CentOS: `sudo dnf install llvm`
- Ubuntu/Debian: `sudo apt-get install llvm`
- Via rustup: `rustup component add llvm-tools-preview`

**Build and run:**
```bash
mkdir -p build && cd build
cmake .. -G Ninja -DBUILD_TESTS=ON -DBUILD_COVERAGE=ON -DCMAKE_BUILD_TYPE=Debug
ninja coverage
```

This runs all tests, prints a per-file coverage summary, and generates an HTML report at `build/coverage-report/index.html`.

The latest coverage report from the `main` branch is available [here](https://nightly.link/lancedb/lancedb-c/workflows/coverage/main/coverage-report.zip).

## Documentation

The LanceDB C API has comprehensive documentation generated from the header file comments.

### Viewing Documentation Online

The documentation is automatically published to **GitHub Pages** on every push to main:
- **URL**: https://lancedb.github.io/lancedb-c/

### Building Documentation Locally

1. **Install documentation tools:**
   - Install Doxygen (extracts API documentation from `include/lancedb.h`)
   - Install Sphinx and Breathe (generate HTML documentation):
   ```bash
   pip install -r docs/requirements.txt
   ```

2. **Build the documentation:**
   ```bash
   mkdir -p build
   cd build
   cmake .. -DBUILD_DOCS=ON
   make docs
   ```

