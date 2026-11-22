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
- CMake (3.22 or later)
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

The Boost.Asio coroutine example is optional because it needs Boost. Enable it
with `-DBUILD_ASIO_COROUTINE_EXAMPLE=ON` in addition to `-DBUILD_EXAMPLES=ON`.

### Installing and downstream discovery

Install into a prefix of your choice. `GNUInstallDirs` controls the layout, so
the library, header, pkg-config file and CMake package all follow
`CMAKE_INSTALL_LIBDIR` and `CMAKE_INSTALL_INCLUDEDIR`:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/opt/lancedb -DCMAKE_INSTALL_LIBDIR=lib64
cmake --build build
cmake --install build
```

A CMake consumer then uses the installed `lancedb::lancedb` target:

```cmake
find_package(lancedb CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE lancedb::lancedb)
```

CMake cannot express a prerelease in a version request, so a numeric
requirement such as `find_package(lancedb 0.1.0)` is rejected for a preview
package. Use unversioned discovery for previews.

The package config and version file are installed under
`${CMAKE_INSTALL_LIBDIR}/cmake/lancedb` and use paths relative to the installed
prefix. For Unix-like and GNU-style toolchains, pkg-config works from
`${CMAKE_INSTALL_LIBDIR}/pkgconfig`:

```bash
PKG_CONFIG_PATH=/opt/lancedb/lib64/pkgconfig pkg-config --cflags --libs lancedb
```

MSVC shared consumers should use the `lancedb::lancedb` CMake target. The MSVC
import library is named `lancedb.dll.lib`, which the conventional `-llancedb`
pkg-config spelling does not select.

On Linux the installed shared library records a SONAME derived from the binding
version: before 1.0 the major.minor pair is the ABI boundary, and from 1.0 the
major version is. The unversioned `liblancedb.so` linker name remains available.
On macOS the library records `@rpath/liblancedb.dylib`. Prebuilt packages keep
the same metadata and package the unversioned library path used by consumers.

### Manual Build Process

If you prefer to build manually:

1. **Build the export-controlled shared library:**
   ```bash
   cargo rustc --release -- -C linker=./cmake/export-linker.sh
   ```

   On Windows use `-C linker=.\cmake\export-linker.cmd` from a Visual Studio
   developer shell. Plain `cargo build` remains the normal path for static,
   rlib and external-provider development, but it does not apply the
   shared-library export boundary; CMake, the package workflow and this
   command are the supported shared build entries.

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

### Cloud Storage and Native Features

`lancedb-c` declares the SDK features it builds against and forwards them, so a
build states what it asks the SDK for instead of inheriting it. These are
forwarding declarations: the public header and the error codes are the same for
every combination, and a feature only changes what the built library can reach.

| C feature | Forwarded to | SDK-side wiring |
|---|---|---|
| `remote` | `lancedb/remote` | The `db://` client and the REST namespace |
| `aws` | `lancedb/aws` | The SDK's AWS support and the `dir-aws` namespace |
| `gcs` | `lancedb/gcs` | The SDK's GCS support and the `dir-gcp` namespace |
| `azure` | `lancedb/azure` | The `dir-azure` namespace and Azure namespace credential vending |
| `dynamodb` | `lancedb/dynamodb` | The DynamoDB commit handler for `s3+ddb://`; implies the SDK's `aws` |
| `oss` | `lancedb/oss` | The `dir-oss` namespace |
| `geo` | `lancedb/geo` | Lance's `geo` feature |
| `fp16kernels` | `lancedb/fp16kernels` | Native f16/bf16 kernels in `lance-linalg` |

Most of these backends are already reachable without the switches. An embedded
connection opens a generic `dir` namespace whose root is resolved through the
connection's object-store registry, and the direct `lance` and `lance-io`
dependencies enable the Lance-side providers for S3, GCS, Azure, OSS, COS, TOS,
GooseFS and Hugging Face as well as Lance's `geo`. What the forwarding changes is
the SDK's own side of that wiring: the `dir-aws`/`dir-gcp`/`dir-azure`
implementations, the SDK's `object_store` edges, Azure namespace credential
vending and the DynamoDB commit handler. The last two are the capabilities a
default build did not have before, and the `s3+ddb://` commit path with them.

On the default S3 backend and the DynamoDB commit handler, `aws_profile` is a
storage option that selects the AWS shared profile per store, so one process
can open connections under different profiles without changing `AWS_PROFILE`:

```c
LanceDBConnectBuilder* builder = lancedb_connect("s3+ddb://bucket/db?ddbTableName=commits");
builder = lancedb_connect_builder_storage_option(builder, "aws_profile", "analytics");
builder = lancedb_connect_builder_storage_option(builder, "aws_region", "eu-west-1");
```

On those two paths the option takes effect from LanceDB SDK `v0.42.0-beta.22`
with its Lance `v15.0.0-beta.26`. A build pinned to the earlier
`v0.42.0-beta.21`/`v15.0.0-beta.25` pair ignores `aws_profile` there and keeps
both on the ambient chains; `Cargo.toml` records which pair a source build
uses. The `v0.1.0-preview.1` release predates the `dynamodb` forwarding and the
profile routing: it was built without the `dynamodb` feature, so `s3+ddb://` is
unavailable there.

`aws_region` is the explicit region and wins over both the environment and the
profile. Without it, the default S3 backend takes `AWS_REGION` when it is set
and then falls back to ordinary bucket-region discovery. Custom S3 endpoints
and the DynamoDB client instead use the SDK-resolved region, or the applicable
default chain. An explicit `aws_profile` still follows the AWS SDK's environment
precedence: `AWS_REGION` and the standard credential variables take precedence
over the profile's own region and keys.

The default set is `remote,aws,gcs,azure,dynamodb`, and packages built from
this revision with the default flags contain it. `oss`, `geo` and
`fp16kernels` are opt-in for source builds:

```bash
cargo rustc --release --locked -p lancedb-c -- -C linker=./cmake/export-linker.sh
cargo rustc --release --locked -p lancedb-c --features geo,fp16kernels -- -C linker=./cmake/export-linker.sh
cargo rustc --release --locked -p lancedb-c --no-default-features --features remote,gcs -- -C linker=./cmake/export-linker.sh
```

Feature selection applies to the packages Cargo selects. A workspace build also
selects `examples/external_provider`, which depends on this package with its
default features, and feature unification then turns those defaults back on even
when `--no-default-features` was passed. Use `-p lancedb-c` when the narrowing is
meant to apply to the library.

`--no-default-features` turns off these forwarding declarations only; the Lance
defaults above still apply, so the Lance-side providers and `geo` remain. Without
`remote` the library cannot produce `LANCEDB_HTTP` or `LANCEDB_RETRY`, because
the SDK does not define those error variants; both codes keep their values for
source compatibility.

`geo` is what builds an RTree index natively; the C index-type enumeration has
no RTree entry yet, so enabling `geo` alone does not expose that index type to C
callers.

`fp16kernels` compiles C kernels with the `cc` crate, per target:

- Windows: no kernels. The Lance build script warns and skips them.
- arm64: NEON only (`-march=armv8.2-a+fp16` on Linux, Apple Silicon tuning on
  macOS).
- Non-Windows x86_64: AVX2 always, plus AVX-512 and AMX-FP16 when the compiler
  supports them. The AVX2 object needs `_Float16`, so the compiler baseline is
  Clang 6 or GCC 12 and newer. GCC 11 rejects the AVX2 object with `'_Float16'
  is not supported on this target` and does not know `-mavx512fp16`. Point `CC`
  at a supported compiler when the default is older, for example
  `CC=clang cargo rustc --release --locked -p lancedb-c --features fp16kernels -- -C linker=./cmake/export-linker.sh`.

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

The shared library exports exactly the functions declared in `include/lancedb.h`.
Earlier preview packages also exposed incidental symbols from statically linked
dependencies (CRC, BLAKE3, and AWS-LC jitterentropy). Those were never part of
the C API and are no longer exported. No documented C function is removed and
the public header is unchanged; a caller that resolved an implementation symbol
by name must provide its own copy. The next preview notes should mention this
export-surface change.

The C binding has its own version line, independent of the Cargo package and
the pinned SDK. The Cargo package version is fixed at `0.0.0`; the release
version comes from the Git tag. Pull-request and untagged local builds report
`0.0.0-dev` in CMake, pkg-config, documentation and `manifest.json`.

The public header exposes `LANCEDB_VERSION_MAJOR`, `LANCEDB_VERSION_MINOR`,
`LANCEDB_VERSION_PATCH` and `LANCEDB_VERSION_STRING` (`LANCEDB_VERSION` is an
alias for the string). The checked-in header reports `0.0.0-dev` so it remains
usable directly without a generated file.
CMake renders the installed header with the configure-time version, and the
package builder renders the same block for release archives. `lancedb_version()`
returns the built version from the loaded library as a static, NUL-terminated
string owned by the library; the caller must not free it. Header, runtime string
and package manifest therefore agree for a package built from the same source.
A direct `cargo build` follows the same `LANCEDB_VERSION` override or exact-tag
resolution for the runtime string, with `0.0.0-dev` when no tag is present. Use
the CMake-installed or package-rendered header when a tagged direct Cargo build
needs compile-time macros that match the runtime; the checked-in header
deliberately stays at the development default.

Tags use strict SemVer with a `v` prefix. If several valid tags point at the
same commit, local builds prefer the last stable tag in Git's version sort,
otherwise the last prerelease; a tag dispatch always uses the explicitly
selected tag. `v0.0.0-dev` is reserved for the untagged fallback.

To prepare a release, create an annotated or lightweight tag such as
`v0.1.0-preview.1` on the exact commit and dispatch `Shared Library Packages`
on that tag. The workflow fetches the tag, requires its peeled commit to be the
checked-out commit, and uses the tag version for the build, package manifest and
consumer verification. A tag on a parent or another branch is rejected. CMake
reads the tag when it configures and records HEAD and tag refs as configure-time
dependencies, so existing build directories reconfigure when they change. An
environment override is not a file input; rerun the CMake configuration step
after changing `LANCEDB_VERSION`. CMake passes its configure-time version to
the docs target through a private variable, so a different build-time
`LANCEDB_VERSION` does not change an already configured docs build.

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

