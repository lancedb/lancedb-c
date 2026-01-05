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

- Rust toolchain (rustc, cargo)
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

