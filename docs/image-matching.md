# Image matching

GhidraEngine decodes each image to luma and computes a PDQ fingerprint. PDQ is a 256-bit
perceptual hash. Compare hashes with Hamming distance: a smaller distance means fewer differing
bits. `PdqDistance` accepts thresholds from 0 through 256, and match thresholds are inclusive.
The exact input and output rules are in [Image API contracts](api.md#images).

## Decode and fingerprint

```cpp
#include <GhidraEngine/image/decode.hpp>
#include <GhidraEngine/image/fingerprint.hpp>

// Minimal usage fragment.
const auto image = GhidraEngine::decode_image("photo.jpg");
const auto signature = GhidraEngine::fingerprint_image(image);
```

The default `TransformPolicy::OriginalOnly` produces one fingerprint. `Rotations` produces
variants for the four rotations; `Dihedral` also includes reflected orientations. These variants
allow transformed copies to be found without guessing the orientation in advance. They are
computed from physically transformed pixels, so their hashes are not bit-string rotations of the
original hash.

The quality score describes PDQ's confidence for one fingerprint. For a pairwise match, both
qualities must meet the configured minimum.

## Compare two images

```cpp
#include <GhidraEngine/hash/pdq.hpp>

// Minimal usage fragment; other_signature is produced the same way.
const auto distance = GhidraEngine::pdq_distance(
    signature.variants.front().hash,
    other_signature.variants.front().hash
);
const bool similar = distance.value() <= 31;
```

For transformed matching, compare every variant pair or use an index followed by candidate
verification. The image candidate pipeline keeps retrieval and policy decisions separate:

```cpp
#include <GhidraEngine/match/image.hpp>

// Candidate-pipeline fragment; index, query, query_id, and catalog are application data.
const auto candidates = GhidraEngine::find_image_candidates(index, query, {31});
const auto matches = GhidraEngine::verify_image_candidates(
    query_id,
    query,
    candidates,
    catalog,
    GhidraEngine::PdqMatchPolicy{.max_distance = {31}, .min_quality = {50}}
);
```

Candidate retrieval queries the index for each query variant. Verification applies distance and
quality thresholds, associates fingerprint records with media IDs, and excludes the query's own
media ID. See [image candidate contracts](api.md#image-matching) for ordering, duplicate handling,
and missing-ID behavior.

## Choosing a transform policy

Use `OriginalOnly` when stored images have a consistent orientation. `Rotations` covers rotation
changes. `Dihedral` also covers reflections, at the cost of storing and querying more variants.
An index should contain one entry for each stored fingerprint variant, with distinct
`FingerprintId` values that point back to the owning `MediaId` in the catalog.

See [Indexing](indexing.md) for complete collection search and [API reference](api.md) for exact
decoding, alpha, color, HDR, and PDQ generation behavior.
