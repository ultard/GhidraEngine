# GhidraEngine

GhidraEngine is a C++20 library for finding duplicate and perceptually similar images and videos.

## Features

**Exact duplicate detection**  
Hash complete files with BLAKE3 and group files with identical 256-bit digests.

**Perceptual image matching**  
Generate PDQ fingerprints and compare images by Hamming distance. Optional rotation and reflection variants allow transformed copies to be found as well.

**Perceptual video matching**  
Generate vPDQ signatures from sampled video frames and compare videos using directional fingerprint coverage.

**Fast collection search**  
Search PDQ fingerprints using either a simple flat index or a multi-index hashing (MIH) index. Both provide complete Hamming-radius searches.

**Filesystem scanning**  
Scan files and directory trees with bounded concurrency, cancellation, deterministic results, per-file error reporting, and an optional persistent fingerprint cache.


## Quick example

```cpp
#include <GhidraEngine/image/decode.hpp>
#include <GhidraEngine/image/fingerprint.hpp>

#include <iostream>

int main(int argc, char** argv) {
    if (argc != 3)
        return 1;

    using namespace GhidraEngine;

    const auto a = fingerprint_image(decode_image(argv[1]));
    const auto b = fingerprint_image(decode_image(argv[2]));

    const auto distance =
        pdq_distance(a.variants.front().hash, b.variants.front().hash);

    std::cout << "PDQ distance: " << distance << '\n';
}
```

PDQ represents an image as a 256-bit perceptual fingerprint. Similar images have a smaller Hamming distance between their fingerprints; identical fingerprints have distance 0.

GhidraEngine can also generate fingerprints for rotated or reflected versions of an image when those transformations should be considered equivalent.


## Requirements

GhidraEngine requires:

- C++20
- CMake 3.25+
- Conan 2.33+
- Ninja
- Python 3.10+

Dependencies, including libvips, FFmpeg, BLAKE3 and PDQ, are managed through Conan.

## Building

PDQ is not available in Conan Center, so export the bundled recipe before configuring the project:

```sh
conan export third_party/pdq
```

```sh
cmake -S . -B build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PROJECT_TOP_LEVEL_INCLUDES=conan_provider.cmake
  
cmake --build build
```

Run the test suite with:

```sh
ctest --test-dir build --output-on-failure
```

If Conan does not have a default profile yet:

```sh
conan profile detect
```

## Using GhidraEngine

After installation:

```cmake
find_package(GhidraEngine CONFIG REQUIRED)

add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE GhidraEngine::GhidraEngine)
```

A source checkout can also be integrated with `add_subdirectory()`.

## Documentation

- [Image matching](docs/image-matching.md)
- [Video matching](docs/video-matching.md)
- [Scanning files and directories](docs/scanning.md)
- [Collection indexing](docs/indexing.md)
- [API reference](docs/api.md)
- [Style guide](docs/style.md)

## License

GhidraEngine is licensed under the [Apache License 2.0](LICENSE).

Third-party license notices are available in [`third_party/licenses`](third_party/licenses).
