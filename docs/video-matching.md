# Video matching

A vPDQ signature is an ordered collection of sampled video frames. Each frame stores a PDQ hash,
quality score, and signed presentation timestamp. Timestamps are retained as metadata; vPDQ
matching compares hashes and does not align timelines.

## Generate signatures

```cpp
#include <GhidraEngine/video/fingerprint.hpp>

// Minimal usage fragment.
const auto first = GhidraEngine::fingerprint_video("first.mp4");
const auto second = GhidraEngine::fingerprint_video("second.mp4");
```

By default, generation selects a frame at least one second after the previously selected frame.
Quality filtering and optional pruning affect which frames participate in matching; pruning is
disabled by default. See [video API contracts](api.md#videos) for decoding and sampling details.

## Compare signatures

`compare_vpdq` returns coverage in each direction. Query coverage is the fraction of eligible
unique query hashes that found a candidate hash. Candidate coverage uses the candidate's eligible
unique hashes as its denominator. These values can differ, for example when one video contains
only a portion of another.

```cpp
#include <GhidraEngine/match/video.hpp>

// Comparison fragment; first and second are generated signatures.
const GhidraEngine::PdqMatchPolicy frame_policy{
    .max_distance = {31},
    .min_quality = {50}
};
const auto comparison = GhidraEngine::compare_vpdq(first, second, frame_policy);
const auto match = GhidraEngine::match_vpdq(
    GhidraEngine::VideoId{1}, first,
    GhidraEngine::VideoId{2}, second
);
```

`compare_vpdq` reports both coverage fractions and eligible frame counts. `match_vpdq` applies
the ID and coverage policy and returns an optional `VideoMatch`.

## Search an indexed collection

Build a `VideoFingerprintCatalog` from signatures and index its `index_entries()` in a
`FlatPdqIndex` or `MihPdqIndex`. `find_video_candidates` retrieves videos with potentially
matching frames; `verify_video_candidates` loads each full signature and recomputes directional
coverage. Retrieval is a candidate stage, not the final coverage decision.

See [video matching contracts](api.md#vpdq-matching) for duplicate hash, many-to-one, empty-side,
indexing, and complexity rules. For image collections, see [Indexing](indexing.md).
