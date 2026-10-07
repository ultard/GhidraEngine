# Collection indexing

An index narrows a collection of PDQ fingerprints to entries within a Hamming radius. Both
`FlatPdqIndex` and `MihPdqIndex` return every hit whose distance is less than or equal to the
threshold, with unique IDs in ascending order. Use `FlatPdqIndex` for simpler or smaller
collections; MIH is intended to accelerate larger collections while retaining complete recall.

## Build and query an index

```cpp
#include <GhidraEngine/index/mih_pdq.hpp>

#include <vector>

// Index construction fragment; fingerprint is an ImageSignature.
std::vector<GhidraEngine::PdqIndexEntry> entries;
entries.push_back({GhidraEngine::FingerprintId{10}, fingerprint.variants.front().hash});

GhidraEngine::MihPdqIndex index{entries};
const auto hits = index.search_within(fingerprint.variants.front().hash, 31);
```

Use `insert` to add entries incrementally and `reserve` when the expected size is known. A
`FingerprintId` identifies an index entry; image applications commonly map it through an
`ImageFingerprintCatalog` to a media ID. IDs must be unique inside each index, while different IDs
may contain the same hash.

## Image and video collections

Image search retrieves fingerprint candidates with `find_image_candidates`, then uses
`verify_image_candidates` to apply quality, self-match, and media-level policy. Video search
indexes every catalog frame entry; use `find_video_candidates` followed by
`verify_video_candidates` to recompute full-signature directional coverage. See
[Image matching](image-matching.md) and [Video matching](video-matching.md).

Const queries may run concurrently. Synchronize mutation, assignment, and copying during mutation
externally. For exact complexity, storage accounting, MIH fallback behavior, and catalog ID
lifetime, see [index API contracts](api.md#indexes).
