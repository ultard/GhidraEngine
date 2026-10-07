# Scanning files and directories

`scan_media` walks local file and directory roots, computes exact content digests, and generates
the enabled image and video fingerprints. It returns files and per-path issues in a deterministic
result. Exact duplicate groups are available from the resulting full BLAKE3 digests.

```cpp
#include <GhidraEngine/scan/scanner.hpp>

#include <array>

const std::array roots{std::filesystem::path{"./library"}};
GhidraEngine::ScanOptions options;
options.workers = 4;
options.fingerprints.kinds = GhidraEngine::MediaKinds::Both;

const auto result = GhidraEngine::scan_media(roots, options);
const auto duplicates = GhidraEngine::exact_duplicate_groups(result.files);
```

`workers` controls the bounded worker count. `MediaKinds` selects images, videos, or both.
`max_files` bounds stored results. Ordinary path, media, and cache failures are reported as
`ScanIssue` entries with a stage and message; invalid configuration, cancellation, allocation
failure, or thread creation failure aborts the scan.

## Fingerprint cache

Pass a `FingerprintCache` pointer to reuse fingerprints for matching content and generation
settings:

```cpp
const GhidraEngine::FingerprintCache cache{"./fingerprints"};
const auto result = GhidraEngine::scan_media(roots, options, &cache);
```

A cache hit still reads and hashes every file byte, so the cache avoids decoding and fingerprint
generation but not exact hashing. Cache settings include the generation parameters. See
[scanner and cache contracts](api.md#scanning) for path rules, per-process constraints, keys,
serialization, and failure guarantees.
