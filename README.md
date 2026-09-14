# mmaplib

`mmaplib` is a C++20 RAII wrapper for memory-mapped files. It provides zero-copy access to file contents through `std::span` while keeping operating-system-specific mapping details inside the library implementation.

The public API is defined in `mmap.hpp` and uses the `mmaplib` namespace.

## Features

- RAII ownership of the file and memory mapping
- Move-only `MmapFile` objects
- Zero-copy read access through `bytes()` and `view()`, with explicit writable views through `mutable_bytes()` and `mutable_view()`
- Optional copying helpers through `read()` and `write()`
- Insertion of text or binary data at any valid mapping position
- Appending text or binary data at the end of the mapped file
- Read-only and read-write mappings
- Shared and private copy-on-write mappings
- File seeking, resizing, syncing, and access-pattern hints
- Optional Linux mapping prefaulting for latency-sensitive workloads
- Platform-specific implementation hidden behind the public API
- CMake library target named `linuxfs_mmap`

## Requirements

- CMake 3.20 or newer
- A C++20 compiler
- Linux for the fully functional implementation currently included

On non-Linux systems, CMake builds the unsupported-platform backend. The public API remains available, but constructing `MmapFile` reports that memory mapping is not supported until a native backend is added for that operating system.

## Files

```text
mmap.hpp                 Public, platform-neutral API
mmap.cpp                 Linux implementation
mmap_unsupported.cpp    Fallback implementation for unsupported systems
mmap_test.cpp            Example-style test application
CMakeLists.txt           Library and test build configuration
```

## Build with CMake

From this directory:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build --parallel
```

This builds:

```text
build/liblinuxfs_mmap.a
build/linuxfs_mmap_tests
```

Run the tests with CTest:

```bash
ctest --test-dir build --output-on-failure
```

Or run the test executable directly:

```bash
./build/linuxfs_mmap_tests
```

Expected output:

```text
mmap tests passed
```

## Build with Make

The project also provides a Makefile for Linux users who prefer Make over CMake. From this directory:

```bash
make
```

This builds:

```text
liblinuxfs_mmap.a
mmap_test
```

Available targets:

```bash
make library  # Build only the static library
make test     # Build the test executable
make check    # Run the test executable
make clean    # Remove generated build artifacts
make help     # List available targets
```

Install the library and header under `/usr/local`:

```bash
make install
```

Use another installation prefix when needed:

```bash
make install PREFIX="$HOME/.local"
```

The Makefile uses `mmap.cpp` and builds the functional test on Linux. On other operating systems it selects `mmap_unsupported.cpp` and reports that tests are unavailable until a native backend is implemented.

## Use from another CMake application

If this directory is included in another project, add it with `add_subdirectory()` and link the library target:

```cmake
cmake_minimum_required(VERSION 3.20)
project(mmap_example LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_subdirectory(path/to/linuxfilesystem mmaplib-build)

add_executable(mmap_example main.cpp)
target_link_libraries(mmap_example PRIVATE linuxfs_mmap)
```

The `PUBLIC` include directory configured by the library makes `mmap.hpp` available to the application.

## Example application

The following program creates a file, maps it for shared read-write access, changes its first byte without copying the file into a class-owned buffer, flushes the change, and then exits. The mapping and file resource are released automatically by the destructor.

```cpp
#include "mmap.hpp"

#include <cstddef>
#include <fstream>
#include <iostream>

int main() {
    const char* path = "example.bin";
    {
        std::ofstream output(path, std::ios::binary);
        output << "hello mmap";
    }

    mmaplib::Config config;
    config.access = mmaplib::Access::read_write;
    config.sharing = mmaplib::Sharing::shared;

    mmaplib::MmapFile file(path, config);

    auto mapped = file.mutable_view(0, file.mapping_length());
    if (!mapped.empty()) {
        mapped[0] = std::byte{'H'};
    }

    file.sync();

    std::cout << "mapped " << file.mapping_length() << " bytes\n";
}
```

    To insert data rather than overwrite existing bytes, use `insert()`. It grows the file, shifts the existing suffix toward the end, and places the new bytes at the requested position:

    ```cpp
    mmaplib::MmapFile file("data.bin", config);

    file.insert("inserted text", 6);
    file.sync();
    ```

    `insert()` also accepts `std::span<const std::byte>` for binary data. The mapping must be writable and use the default variable-length configuration (`Config::length == 0`). Existing `std::span` views become invalid after insertion because the file is resized and remapped.

    To append data at the end of the file, use `append()`:

    ```cpp
    file.append("a new line\n");
    file.sync();
    ```

    `append()` is equivalent to inserting at `mapping_length()`. It grows the file, does not shift an existing suffix, and invalidates existing spans because the mapping is recreated.

For a file that already exists and should be read only, the default configuration is sufficient:

```cpp
mmaplib::MmapFile file("data.bin");

for (const std::byte value : file.bytes()) {
    // Process value without copying the mapped file into another buffer.
}
```

For a file that may not exist, use `create_if_missing` with read-write access:

```cpp
mmaplib::Config config;
config.access = mmaplib::Access::read_write;
config.sharing = mmaplib::Sharing::shared;
config.create_if_missing = true;
config.length = 4096;

mmaplib::MmapFile file("new-data.bin", config);
```

## Direct compilation with g++

CMake is recommended, but the Linux implementation can also be compiled directly:

```bash
g++ -std=c++20 \
    -Wall -Wextra -Wpedantic -Wconversion -Wshadow \
    -I. \
    mmap.cpp mmap_test.cpp \
    -o mmap_test
```

Run it:

```bash
./mmap_test
```

To build a static library manually:

```bash
g++ -std=c++20 \
    -Wall -Wextra -Wpedantic -Wconversion -Wshadow \
    -I. -c mmap.cpp -o mmap.o

ar rcs liblinuxfs_mmap.a mmap.o
```

Then compile an application and link it against the library:

```bash
g++ -std=c++20 \
    -Wall -Wextra -Wpedantic -Wconversion -Wshadow \
    -I. mmap_test.cpp -L. -llinuxfs_mmap -o mmap_test
```

## Important usage rules

### Zero-copy views

`bytes()` and `view()` return `std::span` objects pointing directly into the operating-system mapping. They do not copy file contents.

Use `mutable_bytes()` or `mutable_view()` for direct writable access. The separate names prevent a non-const read-only `MmapFile` from accidentally selecting a writable API.

Do not keep a span after:

- `MmapFile::close()`
- `MmapFile::resize()`
- Moving from the owning `MmapFile`
- Destruction of the owning `MmapFile`

A span can also become invalid if another process truncates or otherwise changes the mapped file externally.

### Concurrency and process coordination

`MmapFile` does not provide synchronization for bytes accessed through returned spans. Callers must ensure that no thread accesses a span while another thread calls `close()`, `resize()`, `insert()`, `append()`, move-assignment, or destruction on the same object. Concurrent writes to the same bytes require application-level synchronization. Other processes must not truncate or replace an actively mapped file: Linux can raise `SIGBUS` for that case, which cannot be converted into a normal C++ range-check error.

For cooperating processes, request an advisory lock when opening the file:

```cpp
config.locking = mmaplib::LockMode::exclusive;
```

`LockMode::exclusive` is appropriate for a process that resizes or truncates
the file; readers can use `LockMode::shared`. Lock acquisition is non-blocking
and a conflicting lock causes construction to throw `std::system_error`.
These are advisory locks: every process that can change the file must use the
same protocol. They cannot make an mmap safe against unrelated or malicious
external truncation.

Structural operations (`resize()`, `insert()`, and `append()`) are rejected
when the object holds `LockMode::shared`; use `LockMode::exclusive` for a
cooperatively locked writer. `LockMode::none` remains available when ownership
is enforced outside this library.

For automatically sized mappings (`Config::length == 0`), `insert()`,
`append()`, and `resize()` verify that the backing-file size still matches the
mapped extent before changing data. If another process has changed the size,
they throw rather than overwrite at a stale offset. After coordinating with
that process, call `remap()` to recreate the mapping for the current file size;
this invalidates all existing spans. Writable private mappings reject
`remap()` because recreating a `MAP_PRIVATE` mapping would discard uncommitted
copy-on-write modifications.

### Growth, persistence, and throughput

`resize()`, `insert()`, and `append()` are supported only for writable shared mappings. They are rejected for private copy-on-write mappings because remapping would otherwise silently discard private dirty pages. `insert()` and `append()` remap and invalidate all spans; use a pre-sized shared mapping and write records directly through `mutable_view()` for sustained low-latency writes.

`sync()` flushes the complete mapping. `sync_range(position, count)` flushes a smaller dirty range; its position must be page aligned. A successful `sync()` requests synchronous mapped-page writeback but does not make multiple writes atomic or supply a crash-recovery protocol. For durable data formats, use record framing/checksums and a commit/recovery design appropriate to the application.

`Advice::will_need` and `Config::prefault` can reduce first-touch faults at the cost of open latency and RAM pressure; they do not guarantee resident pages or bounded latency. `Advice::dont_need` is rejected for writable private mappings because Linux may discard their copy-on-write changes. Benchmark warm and cold cache behavior, random and sequential access, memory pressure, and p99 latency on the target storage before setting production SLOs.

### Shared versus private mappings

`Sharing::shared` allows modifications to be written back to the file after synchronization:

```cpp
config.sharing = mmaplib::Sharing::shared;
```

`Sharing::private_copy` uses copy-on-write. Changes are visible to the current process but are not written to the file:

```cpp
config.sharing = mmaplib::Sharing::private_copy;
```

### Persistence

For shared writable mappings, call:

```cpp
file.sync();
```

This requests synchronous writeback of modified pages. It does not prevent another process from truncating the file while it is mapped.

`sync()` is valid only for `Sharing::shared`; private copy-on-write mappings have no file-backed changes to persist.

### Concurrency and external changes

Concurrent const access is safe while the mapping remains unchanged. Concurrent writes require synchronization supplied by the caller when readers need a defined consistency or memory-ordering guarantee. `close()`, `resize()`, `insert()`, and `append()` must not run concurrently with any access and invalidate existing spans.

If another process truncates the file below the mapped range, a later access to the invalid portion can raise `SIGBUS`. Coordinate file ownership or prevent truncation while mappings are active.

For latency-sensitive workloads, set `Config::prefault` to request Linux `MAP_POPULATE`. This shifts page-fault work into construction and increases startup latency and memory pressure; it is not enabled by default.

### Exceptions

Typical failures are reported through exceptions:

- `std::system_error`: operating-system operation failed
- `std::invalid_argument`: invalid mapping configuration or size
- `std::out_of_range`: requested view is outside the mapping
- `std::logic_error`: operation requires a writable or open mapping
- `std::length_error`: requested mapping is too large

## Installation

Install the library and public header after configuring and building:

```bash
cmake --install build --prefix "$PWD/install"
```

The installed files are placed under:

```text
install/lib/liblinuxfs_mmap.a
install/include/linuxfs/mmap.hpp
```

## Future platform backends

The public API is intentionally platform-neutral. Future implementations can map the same operations to native APIs:

- macOS: POSIX `mmap`, `munmap`, `msync`, and related calls
- Windows: `CreateFile`, `CreateFileMapping`, `MapViewOfFile`, and related calls

Applications should use `mmaplib::Config` and the `mmaplib` enums rather than native operating-system flags. This allows a platform backend to be added without changing application code.
