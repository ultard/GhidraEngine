# API reference

This document defines the public behavioral contracts and operational limits.

## Types, ownership, and identity

Byte, hash, signature, and decoded-image results own their storage. Input spans borrow caller
memory for the duration of the call. Catalog accessors return borrowed spans or references: keep
the catalog alive and do not assign to it while using those views. Do not retain a view from a
temporary catalog. `FingerprintCache::directory()` borrows from the cache object; `version()`
refers to static storage.

`MediaId`, `VideoId`, and `FingerprintId` are distinct `uint64_t`-backed types. Zero is valid.
Scanner IDs and dense catalog frame IDs belong to one result; rebuilding invalidates them. Equal
hashes do not identify the same media object.

| Type           | Representation or valid range                                       |
|----------------|---------------------------------------------------------------------|
| `Blake3Digest` | 32 bytes.                                                           |
| `PdqHash`      | Four `uint64_t` words; word 0 contains reference bits 0 through 63. |
| `PdqQuality`   | Integer in [0, 100].                                                |
| `PdqDistance`  | Integer in [0, 256].                                                |
| `Coverage`     | Finite fraction in [0, 1].                                          |
| `Timestamp`    | Signed microseconds, including negative presentation times.         |

Invalid quality, distance, or coverage construction throws `std::invalid_argument`.

## Exact hashing

`hash_blake3(span<const byte>)` hashes all supplied bytes without copying and is `noexcept`.
`hash_blake3_stream` reads from the current position to EOF using a 64 KiB buffer. It requires a
good stream and preserves its exception mask. EOF works with exception-enabled streams; other
read failures throw `std::ios_base::failure`. `hash_blake3_file` accepts regular files and file
symlinks.

File hashing rejects empty or NUL-containing paths and reports path and I/O errors as
`std::filesystem_error`. Cancellation throws `std::system_error(operation_canceled)` and returns
no partial digest. Cancellation cannot interrupt a blocking read. Keep input contents stable.

## Images

### Decoding and color

`decode_image` probes file contents rather than extensions, applies EXIF orientation, and converts
embedded ICC color to sRGB. The result is tightly packed owning float luma. Dimension and pixel
limits are checked before pixel evaluation. `ImageDecodeOptions` defaults to maximum dimension
32768, maximum 40,000,000 pixels, white alpha background, and default HDR settings.

| Input                                         | Handling                               |
|-----------------------------------------------|----------------------------------------|
| Unsigned 8/16-bit SDR                         | Supported.                             |
| Linear float/double TIFF and Radiance RGBE    | HDR RGB/gray; 1.0 represents 203 nits. |
| Animated or multi-page images                 | Rejected.                              |
| Other sample formats or color interpretations | Rejected.                              |

HDR uses the [shared tone-mapping policy](#hdr-tone-mapping). Non-finite samples, alpha outside
[0, 1], and floating ICC profiles fail. Negative linear samples clip to zero. Radiance follows
libvips' native header policy and does not apply nonstandard scanline orientation.

Straight alpha composites onto the configured RGB8 background in encoded sRGB, with nearest
integer rounding. Luma is `0.299f*R + 0.587f*G + 0.114f*B`, without linearization.

### Native resources and cancellation

libvips initializes once with concurrency 1 and its operation cache disabled. Native decoding
and error attribution use a global lock. Conversion of owning pixel buffers to luma and tone
mapping happen outside that lock. Double RGBA may require 32 bytes per pixel, plus output luma
and decoder storage.

Other libvips users must coordinate process-wide settings, error handling, and shutdown. Keep
files stable. Cancellation checks run between native/luma rows and around native calls; they
cannot interrupt a blocking codec operation.

### PDQ computation and encoding

`compute_pdq` accepts finite row-major luma in [0, 255]. Dimensions must be positive, safe for
`int` arithmetic, and match the span. Invalid input throws `std::invalid_argument`; allocation
errors propagate. Dimensions below 5 produce the upstream zero hash and quality 0.

The function preserves input. Working storage uses two full-size float buffers, or one for the
reference 64x64 shortcut. Video generation reuses these buffers within a call and retains
capacity for the largest selected frame until that call ends. Calls and threads share no
workspace.

`encode_pdq` returns 64 lowercase hexadecimal digits, highest bit first. `decode_pdq` accepts
exactly 64 digits in either case and rejects prefixes, whitespace, and embedded NUL.

### Image matching

`fingerprint_image` uses reference DCT transforms to compute variants:

| Transform policy | Variants |
| --- | --- |
| `OriginalOnly` | 1 |
| `Rotations` | 4 |
| `Dihedral` | 8 |

The original variant is independent of policy. Other variants are not bit-string rotations;
rehashing physically transformed pixels can differ because of filtering and float rounding.

Candidate search and verification have separate responsibilities:

- `find_image_candidates` queries every supplied variant and returns ascending unique
  `FingerprintId` values. It applies no quality, self, or media policy.
- `verify_image_candidates` recomputes distances, requires both qualities to meet the inclusive
  minimum, excludes self, and returns the smallest eligible distance per candidate `MediaId` in
  ascending order.

Queries require 1 through 8 variants. Missing catalog IDs throw `std::out_of_range`.

### Distance kernels

`pdq_distance` is `constexpr` and `noexcept`. Constant evaluation uses portable popcount; runtime
evaluation selects a supported kernel when `GHIDRAENGINE_ENABLE_SIMD=ON`. Detection and dispatch
stay outside long inner loops. Inclusive thresholds can reject a pair before remaining words are
computed; returned hit distances are exact. SIMD preserves alpha quantization and ordered
float-luma operations. It does not approximate HDR transfer functions or change generation
revision 2. See [CPU kernel configuration](build.md#cpu-kernels).

## Indexes

### Search and concurrency

Flat and MIH return every hash at Hamming distance `<=` the threshold, sorted by unique
`FingerprintId`. Thresholds above 256 and duplicate IDs throw `std::invalid_argument`. Different
IDs may contain identical hashes.

Const queries are safe concurrently. Mutations, assignment, and copying an object during
mutation require external synchronization.

### Storage and costs

| Index | Storage and operations |
| --- | --- |
| Flat | Sorted contiguous records. Bulk load is O(N log N); increasing-ID insertion is amortized O(1); unordered insertion and search are O(N). |
| MIH | Records plus sixteen 16-bit slot tables. Candidate deduplication precedes full-hash verification. Pigeonhole probing and Flat fallback preserve complete recall. |

MIH uses dense posting positions and supports at most `UINT32_MAX` records. Posting positions
remain stable. Ordered records use binary search to reject duplicate IDs; increasing-ID insertion
is amortized O(1) without an ID set.

The first out-of-order insertion builds an unordered ID set in O(N). Later insertion is
amortized O(1), regardless of ID order. Unordered bulk loads build the set directly. Queries sort
hits only when record IDs are unordered.

A populated MIH index has roughly 8 MiB of fixed tables, plus records, postings, and an ID set
for unordered records. `storage_bytes()` measures vector capacities and estimates ID-set nodes
and buckets. It excludes allocator overhead, process RSS, and query scratch.

## Videos

### Decoder lifetime and input

`VideoDecoder` owns native resources and is move-only. `next()` returns an owning image and
timestamp, or repeatable `nullopt` at EOF. Moved-from and failed instances reject further reads
with `std::logic_error`. Separate decoders support concurrent use; one instance needs external
synchronization.

Decoding selects the first non-attached video stream and uses one software codec thread with
custom local-file I/O. It opens no URL, protocol, or program. Headerless discovery is bounded by
1 MiB and 2500 packets; native metadata/index memory can grow with file size. Defaults for
maximum dimension, pixels, alpha background, sample interval, and HDR are as in
`VideoDecodeOptions` (32768, 40,000,000, white, zero, and default HDR respectively).

Explicit native errors and corrupt flags fail. Ordinary EOF cannot certify completeness.

### Display transforms and timestamps

Decoding applies display rotation and reflection; scaling, shear, and perspective fail.
libswscale performs YUV range/matrix conversion. SDR transfer/primaries conversion,
sample-aspect-ratio stretching, and deinterlacing are not performed.

Missing, overflowing, or backward PTS fail. Negative origins and presentation order remain
intact.

PQ/HLG HDR uses a 16-bit RGBA intermediate, inverse transfer, HDR-to-linear-sRGB primaries
conversion, and tone mapping before alpha composition and luma. BT.2020 and BT.709 HDR primaries
are supported. Unspecified HDR primaries or matrix default to BT.2020.

### HDR tone mapping

`ImageDecodeOptions::hdr` and `VideoDecodeOptions::hdr` share `HdrToneMapOptions`:

| Setting     | Default | Valid range                             |
|-------------|---------|-----------------------------------------|
| `exposure`  | 1       | Finite multiplier in [0.000001, 10000]. |
| `peak_nits` | 1000    | Finite value in [100, 10000] nits.      |

Peak sets the HLG display peak and Reinhard white point. PQ uses the BT.2100 absolute EOTF. HLG
uses the inverse OETF and luminance-dependent display OOTF. A fixed-luminance Reinhard curve maps
linear RGB to SDR sRGB8 with gamut clipping and nearest-integer rounding.

This policy produces deterministic fingerprints. It does not render for display or apply dynamic
HDR10+/Dolby Vision metadata. Generation revision 2 includes both HDR settings in the cache key;
SDR pixel semantics remain unchanged. Transfer equations follow
[ITU-R BT.2100-2](https://www.itu.int/dms_pubrec/itu-r/rec/bt/R-REC-BT.2100-2-201807-S%21%21PDF-E.pdf).

### Sampling and pruning

A zero sampling interval emits every frame. A positive interval emits the first frame, then the
first frame at least that interval after the previously selected timestamp. This measures gaps
between selections rather than using a fixed grid. Skipped frames still decode but avoid RGB/luma
conversion.

`fingerprint_video` defaults to a one-second gap, preserves timestamp and quality, and starts no
worker pool. The retained-frame budget must be positive; exceeding it throws `std::length_error`.
Optional pruning compares each hash to the last retained hash and can reduce recall. It is
disabled by default. `prune_vpdq` preserves order and metadata.

## vPDQ matching

### Coverage rules

`compare_vpdq` follows the pinned ThreatExchange Python matching policy:

1. Keep the first occurrence of each exact hash, then apply the quality filter.
2. Apply inclusive distance and quality thresholds. Allow many-to-one matches.
3. Compute each direction's coverage as matched eligible hashes divided by eligible unique hashes.

Timestamps do not affect matching. Order matters only because it selects the first duplicate's
quality. An empty eligible side gives zero coverage on both sides.

`match_vpdq` excludes self and checks independent inclusive coverage minima. Empty eligible sides
never match, even when both minima are zero. The pinned legacy C++ vPDQ matcher and CFR file
hasher use different policies; arbitrary videos need not reproduce their sampled signatures.

### Catalogs and candidate verification

Video catalogs own sorted signatures and dense frame payloads. Copy assignment is transactional
on allocation failure; moving leaves an empty, reusable source. Index exactly `index_entries()`
under the same generation settings.

Retrieval deduplicates `VideoId` values. Verification loads full signatures and recomputes
coverage. With both coverage minima zero, retrieval returns all catalog IDs, including disjoint
bags; verification still rejects empty bags.

### Comparison costs

Comparison uses O(Q*C + Q log Q + C log C) time and O(Q+C) scratch. At distance 0 it uses linear
intersection of prepared hashes. At distance 256 every nonempty eligible hash matches.

At distances 1 through 255, large sparse inputs may use a temporary exact byte-partition index
with mixed Hamming radii per partition. This preserves complete recall. A cost heuristic selects
the algorithm without changing coverage. Small, dense, or broad-radius inputs use exhaustive
search when probing would cost more.

Long indexed sides use blocks of at most 65536 hashes. A query match bitmap prevents double
counting across blocks. Index postings use at most 8 MiB, with up to 129 KiB for offsets and build
cursors, in addition to prepared-hash storage. Directional indexes are built sequentially. Batch
verification prepares query hashes once per call, without a shared mutable cache or a public
prepared-signature type.

## Scanning

### Paths and limits

`scan_media` accepts files and real directory roots. It recursively ignores directory symlinks
and rejects them as roots. On Windows this includes junctions and other directory reparse
points. It follows file symlinks and deduplicates normalized paths from overlapping roots.

Directory components resolve through the filesystem before normalization. Final file symlink
names remain distinct; hardlinks remain separate paths. Cache directory paths resolve after
creation, and scanning excludes that directory. Sorting assigns deterministic local IDs.

Roots must be non-empty and each root must resolve to a regular file or real directory.
`workers` must be in [1, 256], and `max_files` must be positive. Unique-file and retained-frame
budgets bound stored results. When both media kinds are enabled, image probing precedes video
probing. There is no nested pool.

### Errors and exact groups

| Failure                                                                                   | Result                          |
|-------------------------------------------------------------------------------------------|---------------------------------|
| Traversal, media, or cache error                                                          | A per-path issue.               |
| Invalid roots/configuration, allocation failure, thread creation failure, or cancellation | Abort without a partial result. |

Successful byte hashes can survive unsupported or corrupt-media decoding. Before/after size and
mtime checks detect common file changes, not hostile races. Keep input files stable.
`exact_duplicate_groups` groups full digests, sorts and deduplicates IDs, and returns deterministic
groups.

### Cache keys, serialization, and recovery

Cache keys include full content BLAKE3, all generation settings, and a generation revision. A
cache hit still reads and hashes every input byte. Corrupt entries are reported, regenerated, and
replaced when possible.

Wire version 1 uses explicit little-endian encoding, signed PTS, and a BLAKE3 checksum. It never
serializes raw object layouts or index internals. Invalid versions, fields, lengths, counts, and
checksums fail. Wire versions and generation revisions serve separate purposes.

Entries are staged in exclusive directories and atomically replaced. On Windows, file reads and
publication share a process-wide lock. Checksum validation and deserialization run after closing
the file and releasing the lock. Separate processes must coordinate access.

Keep the cache private. It provides no fsync/crash-durability, eviction, migration, or
authentication guarantee.
