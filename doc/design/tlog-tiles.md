# Design: Tiled storage and tile-backed proofs for merklecpp

Status: Proposal / Draft
Audience: merklecpp maintainers and contributors
Scope: Extend the header-only library to (a) persist the Merkle tree
progressively (on compaction) using the
[tlog-tiles](https://c2sp.org/tlog-tiles) SHA-256 format and an explicitly
namespaced extension for other SHA output sizes, and (b) serve **inclusion**
and **consistency** proofs from those tiles, from the in-memory tree, or from a
combination of the two.

---

## 1. Goals and non-goals

### Goals

1. Write the tree to disk as a set of immutable, cacheable **tile files** using
   the [tlog-tiles](https://c2sp.org/tlog-tiles) payload, path encoding, and
   geometry (256 hashes per tile and 8 tree levels per tile). SHA-256 follows
   the C2SP format; SHA-384 and SHA-512 retain the same 256-hash width as
   merklecpp extensions. Only **full, balanced** tiles are produced. The
   remaining frontier stays in the in-memory tree until it crosses the next
   full-tile boundary.
2. Produce tiles **progressively**: each batch/flush writes only the newly
   completed tiles, and the write integrates with the existing `flush_to()`
   "compaction" so that flushed (evicted) subtrees are durably persisted before
   their nodes are freed from memory.
3. Provide an API to retrieve, **after compaction**, an **inclusion path** and a
   **consistency path**:
   - from the in-memory tree alone (existing behaviour),
   - from full tiles alone, for the tiled (full-tile-covered) prefix, or
   - from a combination of full tiles (old, flushed part) and the in-memory
     tree (recent frontier) — the normal case.
4. Keep everything **header-only** and templated, matching the existing
   `TreeT<HASH_SIZE, HASH_FUNCTION>` style.

### Full-tile-only policy

Partial tiles are out of scope: merklecpp never emits or reads them. During a
flush, a tile becomes eligible only after all 256 entries are complete and
final. Entries beyond that boundary remain in memory. Once published, a full
tile is immutable and is never rewritten.

### Thread-safety policy

The tiled-storage API provides no internal synchronization. Every object is
single-threaded unless its caller serializes all access. This requirement also
applies to separate objects that share a store prefix and to methods declared
`const`, because proof reads update the `TileHashSourceT` LRU cache. Locking and
reader/writer coordination are deliberately left to the application.

### Hard constraints (from the request)

- **Hashing logic is unchanged.** No modification to `HASH_FUNCTION`, to node
  hashing, or to leaf handling.
- **Proofs remain compatible with the existing library.** A proof produced from
  tiles MUST be identical to (and verify against the same root as) the proof the
  existing library would produce via `Tree::path()` / `Tree::past_path()` /
  `Tree::root()` / `Tree::past_root()`.

### Non-goals

- Making the SHA-384 or SHA-512 extension a C2SP standard. C2SP currently
  specifies SHA-256 only; the additional namespaces are merklecpp formats.
- Supplying RFC 6962 leaf hashing or domain separation. Exact external tlog
  interoperability requires the consumer to instantiate `TreeT` with
  compatible leaf and node hashing; this design does not alter hashing.
- An HTTP server. We define the on-disk layout and the read/write/proof APIs;
  serving the static files is an application concern.

---

## 2. Background

### 2.1 tlog-tiles, the parts we adopt

A tiled log exposes the Merkle tree as a set of static resources:

- **Tiles** at `<prefix>/tile/<L>/<N>`, `application/octet-stream`.
  - `<L>` = level, decimal `0..63`, no leading zeros.
  - `<N>` = tile index within the level, encoded as zero-padded 3-digit path
    elements where **all but the last element are prefixed with `x`**. Example:
    `1234067` → `x001/x234/067`.
- A **full tile** is exactly **256 SHA-256 hashes**, or **8,192 bytes**. C2SP
  fixes both the algorithm and width. merklecpp keeps the width at 256 and
  makes the hash size a template parameter.
- Level 0 hashes are **leaf hashes**. At level `L ≥ 1`, each hash is the Merkle
  Tree Hash of a *full* tile at level `L-1`. A tile spans **8 tree levels**: its
  256 entries are the leaves of a height-8 perfect subtree, and intra-tile
  internal nodes are reconstructed by hashing entries.
- The `n`-th tile at level `l` contains, for `i = 0..255`:
  `MTH(D[(n*256+i)*256^l : (n*256+i+1)*256^l])`.
  Its **start index** is `n*256^(l+1)`, its **end index** `(n+1)*256^(l+1)`.
- The frontier beyond the last full tile is held in the in-memory tree, so the
  on-disk tile set is always a full-tile prefix.
- **Entry bundles** at `<prefix>/tile/entries/<N>`: big-endian `uint16`
  length-prefixed raw entries, 256 per full bundle. (See
  [section 6.8](#68-entry-bundles-optional).)
- **Pruning**: a log keeps a *minimum index*; tiles/bundles whose end index is
  `≤ minimum index` may be denied. This maps cleanly onto merklecpp's
  `flush_to()` / `min_index()`.

### 2.2 The merklecpp model

- `TreeT<HASH_SIZE, HASH_FUNCTION>` is a **left-balanced binary Merkle tree**.
  Its shape is identical to RFC 6962: a node's left child is the largest perfect
  subtree of `2^k` leaves with `2^k < n`. A *full* node of height `h` covers
  exactly `2^(h-1)` aligned leaves, and its hash is the root of that perfect
  subtree (verified from `walk_to`, `is_full`, `update_sizes`).
- Internal nodes are combined with `HASH_FUNCTION(left, right, out)` over exactly
  two child hashes (`merklecpp.h` node hashing). There is **no** domain
  separation; the combiner is the only cryptographic operation.
- `flush_to(index)` is the existing **compaction** primitive: it conflates the
  left part of the tree into single hash-only nodes and drops `num_flushed`
  leaves from memory, raising `min_index()` to `index`. After a flush, paths for
  leaves `< index` can no longer be produced from memory.
- Existing proof APIs we must stay compatible with:
  - `root()` / `past_root(i)` — current / historical root.
  - `path(i)` — inclusion path for leaf `i` in the current tree.
  - `past_path(i, as_of)` — inclusion path for leaf `i` as of size `as_of+1`.
  - `PathT::verify(root)` / `PathT::root()` — verification via `HASH_FUNCTION`.
  - merklecpp has **no** classic two-size consistency proof today; this design
    adds one (built from the same combiner, so it reconciles `past_root`s).

### 2.3 Compatibility statement

C2SP `tlog-tiles` normatively specifies SHA-256, 32-byte hashes, and 256 hashes
per full tile. The SHA-256 merklecpp layout adopts that tile payload and the
`tile/<L>/<N>` resource tree. Exact proof interoperability additionally requires
RFC 6962-compatible leaf and node hashing; merklecpp deliberately continues to
use the caller's existing `HASH_FUNCTION`.

For SHA-384 and SHA-512, merklecpp extends the payload by concatenating 48-byte
or 64-byte hashes respectively. **The tile width remains 256 hashes for every
algorithm**, so full tiles are 12,288 bytes for SHA-384 and 16,384 bytes for
SHA-512. These wider-hash payloads are not currently defined by C2SP.

Each format gets a separate root named
`<hash-algorithm-short-name>-<tile-width>w`: `sha256-256w`, `sha384-256w`, or
`sha512-256w`. Beneath that root, the C2SP `tile/...` paths are unchanged. This
prevents files with different hash sizes from sharing a tile namespace while
preserving the C2SP layout for each format. Built-in SHA functions select their
short name automatically; explicit custom names must be lowercase path-safe
short names, and recognized SHA names are checked against `HASH_SIZE`.

## 3. Tile ↔ merklecpp mapping (the math)

Let `TILE_HEIGHT = 8` and `TILE_WIDTH = 256 = 2^8`. All combiners below are the
tree's `HASH_FUNCTION`; `MTH` denotes the Merkle Tree Hash computed with it.

- **Tile entry = perfect subtree root.** Entry `i` of tile `(L, N)` (global
  entry index `g = N·256 + i`) is `MTH(D[g·256^L : (g+1)·256^L])`, i.e. the root
  of a perfect subtree of `2^(8L)` leaves — the in-memory full node of height
  `8L + 1`.
- **Level roll-up.** Because a full level-`L` entry is the root of 256 full
  level-`(L-1)` entries, a level-`L` tile can be computed from level-`(L-1)`
  tiles alone: `tile(L,N)[i] = perfect_root( tile(L-1, N·256+i)[0..255] )`. So
  the **write path needs no tree internals above level 0** — only leaf hashes
  plus roll-ups.
- **Intra-tile internal nodes.** A tile's 256 entries are the leaves of a
  height-8 perfect subtree. The root of `2^r` (`0 ≤ r ≤ 8`) consecutive,
  aligned entries is `perfect_root` of that run. Hence any RFC 6962 subtree root
  at tree level `k = 8L + r` is obtained from **one** level-`L` tile by hashing
  `2^r` consecutive entries (a single entry when `r = 0`).
- **Full-tile prefix.** Only complete level-`L` tiles are written:
  `full_tiles_L = floor( floor(s / 256^L) / 256 )`, together covering the leaf
  prefix `covered = floor(s / 256) * 256`. The incomplete frontier `[covered, s)`
  is not tiled; it is served from the in-memory tree. When no higher-level full
  tile contains a subtree, it is resolved by **descending to the highest
  available full tile and rolling up** (ultimately from full level-0 tiles).

This yields a single read primitive that both proof types are built on:

```
subtree_root(level k, index j) = MTH(D[j·2^k : (j+1)·2^k])   # within `covered`
   L = k / 8 ; r = k % 8
   first = j << r ; n = first / 256 ; off = first % 256
   if a full level-L tile n exists (first + 2^r within full_tiles_L · 256):
       return perfect_root( tile(L, n)[off : off + 2^r] )     # single entry if r==0
   else:                              # higher-level tile unavailable: descend
       return HASH_FUNCTION( subtree_root(k-1, 2j), subtree_root(k-1, 2j+1) )
```

and a range primitive for non-perfect (right-frontier) subtrees:

```
mth_range(a, b):                         // MTH(D[a:b]); a aligned to 2^ceil(log2(b-a))
   w = b - a
   if w == 2^k and source can resolve subtree_root(k, a>>k): return it
   k = largest power of two < w          // RFC 6962 split
   return HASH_FUNCTION( mth_range(a, a+k), mth_range(a+k, b) )
```

`mth_range` falls back to splitting when a perfect subtree is not directly
resolvable from the chosen source; the recursion always bottoms out at resolvable
pieces (see the [combined-source invariant](#7-progressive-production--compaction)).

---

## 4. File and directory layout

Rooted below a configurable `prefix` directory on local disk:

```
<prefix>/
  sha256-256w/                    # algorithm + fixed 256-hash width
    tile/
      0/                          # level 0 (leaf hashes), full tiles only
        000  001 ... 255          # 8192 B per SHA-256 full tile
        x001/
          000 ... 255
      1/                          # level 1 (roll-ups of level-0 full tiles)
        000 ...
      .../
      entries/                    # optional raw entry bundles (full only)
        000  001 ...
  sha384-256w/                    # optional; same width, 12288 B full tiles
    tile/
      ...
```

### Index encoding (`encode_tile_index`)

```
encode_tile_index(N):
   parts = []
   do { parts.push_front(printf("%03d", N % 1000)); N /= 1000 } while (N > 0)
   for each part except the last: prepend 'x'
   return join(parts, "/")
```

Examples: `5 → "005"`, `255 → "255"`, `1000 → "x001/000"`,
`1234067 → "x001/x234/067"`.

Resource paths:

- Full tile:
  `<prefix>/<algorithm>-256w/tile/<L>/<encode_tile_index(N)>` (the only tiles
  this implementation writes)
- Entry bundle:
  `<prefix>/<algorithm>-256w/tile/entries/<encode_tile_index(N)>` (full bundles
  only)

Tile byte format: the 256 entries concatenated, each `HASH_SIZE` raw bytes
(`HashT::bytes`); a full tile is `256 * HASH_SIZE` bytes. The `256w` suffix
counts hashes, not bytes, and therefore does not change for SHA-384 or SHA-512.

---

## 5. Architecture overview

```
            append(leaf hash)            flush()
   caller ───────────────▶  TiledTreeT ───────────────▶  TileStoreT (disk I/O)
                              │  owns Tree                   ▲
                              │                              │ read tiles
              inclusion/consistency proof requests           │
                              ▼                              │
                       ProofEngine  ──▶  HashSource ◀────────┘
                                          ├─ MemoryHashSource (in-memory Tree)
                                          ├─ TileHashSource   (TileStore + size)
                                          └─ CombinedHashSource
```

New, all in a companion header `merklecpp_tiles.h` (includes `merklecpp.h`),
namespace `merkle::tiles`, every type templated on `<HASH_SIZE, HASH_FUNCTION>`
with default aliases mirroring the bottom of `merklecpp.h`:

| Component            | Responsibility                                                        |
|----------------------|-----------------------------------------------------------------------|
| `TileCoord`          | pure index math + path encoding (no I/O)                              |
| `TileStoreT`         | read/write tile files on a local filesystem                          |
| `TileWriterT`        | compute & persist newly-complete full tiles (write path)             |
| `HashSource` impls   | resolve `subtree_root` from tiles, memory, or both                   |
| `ProofEngineT`       | `mth_range`, `inclusion_proof`, `consistency_proof`, verifiers       |
| `TiledTreeT`         | convenience wrapper: `append` / `flush` / `prove*` / compaction      |

The **only** (optional) change to the core header is a small, read-only,
**non-hashing** accessor (`subtree_root`) used by `MemoryHashSource`; see
[§6.2](#62-optional-core-accessor-non-hashing).

---

## 6. Public API design

### 6.1 Header, namespace, aliases

```cpp
// merklecpp_tiles.h
#include "merklecpp.h"
#include <filesystem>
#include <fstream>

namespace merkle {
namespace tiles {

static constexpr uint16_t TILE_HEIGHT = 8;
static constexpr uint16_t TILE_WIDTH =
  uint16_t{1U << TILE_HEIGHT};

template <size_t HASH_SIZE,
          void HASH_FUNCTION(const HashT<HASH_SIZE>&,
                             const HashT<HASH_SIZE>&,
                             HashT<HASH_SIZE>&)>
class TileStoreT { /* ... */ };
// ... TileWriterT, ProofEngineT, TiledTreeT ...

} // namespace tiles

// Convenience aliases (mirror merklecpp.h)
using TileStore = tiles::TileStoreT<32, sha256_compress>;
using TiledTree = tiles::TiledTreeT<32, sha256_compress>;
// + 384/512 variants
} // namespace merkle
```

Note the roll-up/proof combiner is the **same** `HASH_FUNCTION` the tree uses,
and it only ever combines two `HASH_SIZE`-byte hashes — so the default
`sha256_compress` single-block path is sufficient and **no OpenSSL dependency is
introduced**.

### 6.3 `TileStoreT` — disk I/O

```cpp
struct TileRef { uint8_t level; uint64_t index; }; // full tiles only

class TileStoreT {
public:
  // Built-in SHA functions select sha256, sha384, or sha512.
  explicit TileStoreT(std::filesystem::path prefix);
  TileStoreT(std::filesystem::path prefix,
             const std::string& hash_algorithm_short_name);

  // Path helpers (pure)
  static std::string storage_directory_name(const std::string& algorithm);
  static std::string encode_index(uint64_t n);
  std::filesystem::path tile_path(const TileRef&) const;
  std::filesystem::path entries_path(uint64_t n) const;

  // Tiles
  bool has_full_tile(uint8_t level, uint64_t index) const;
  std::vector<Hash> read_tile(const TileRef&) const;           // 256 entries
  void write_tile(const TileRef&, const std::vector<Hash>&);   // synced atomic replace
};
```

- Writes use a unique temporary file, sync the file contents, publish with an
  atomic replace, and on POSIX sync every newly created directory link plus the
  destination directory after the rename. Before reusing a visible file, the
  writer re-confirms the directory chain and destination directory, so a retry
  repeats a failed sync even if directory creation or rename is already
  visible. A wrong-size tile is not treated as a durable full tile and is
  rewritten by the writer.
- Full tiles are written once and never rewritten (immutability), so every file
  under `<algorithm>-256w/tile/<L>/` is write-once.
- A small in-process cache of recently read tiles avoids repeated I/O during
  proof generation.
- `TileStoreT` does not synchronize access. Callers must serialize all
  operations on a store and across stores that share a prefix.

### 6.8 Entry bundles (optional)

merklecpp never sees raw entries (callers insert pre-computed leaf hashes), so
entry bundles are an **application-owned** add-on, included for completeness:

- `<algorithm>-256w/tile/entries/<N>` stores big-endian `uint16`
  length-prefixed entries, 256 per full bundle (full bundles only).
- The application is responsible for the leaf-hash derivation it uses (e.g.
  `leaf_hash = H(entry)`); merklecpp stores whatever leaf hash is inserted.
## 10. Implementation plan

Each phase is independently testable; phases 1–4 require **no** core changes.

**Phase 0 — Scaffolding.** Add `merklecpp_tiles.h` (includes `merklecpp.h`),
`merkle::tiles` namespace, geometry constants, default aliases. Wire a new test
group in `test/CMakeLists.txt` following `add_merklecpp_test`.

**Phase 1 — Coordinates & store.** `TileCoord`/`encode_index`, `TileRef`,
`TileStoreT` (path building, atomic `write_tile`, `read_tile`). *Tests:*
index-encoding vectors; algorithm-qualified roots; SHA-256 and SHA-384 both use
256-hash tiles; tile byte round-trip.

**Phase 2 — Write path.** `TileWriterT::write_up_to` (level-0 from `leaf_at`,
roll-ups, incremental cursor; full tiles only — the frontier is never tiled).
*Tests:* sizes 256 & 70 000 produce exactly the expected full-tile set;
full-tile immutability (re-running writes nothing new).

**Phase 3 — Hash sources & proof engine.** `TileHashSource`, `mth_range`,
`ProofEngineT::root/inclusion_proof/consistency_proof/verify_consistency`.
*Tests:* `root(size)` from tiles == `tree.root()`; `inclusion_proof(i,size)`
**equals** `tree.path(i)` (operator==) and verifies; `inclusion_proof(i, m)`
equals `tree.past_path(i, m-1)`; consistency proof reconciles
`tree.past_root(m-1)`/`tree.past_root(n-1)`.

**Phase 4 — Combination & wrapper.** Optional core `subtree_root` accessor;
`MemoryHashSource`, `CombinedHashSource`, `TiledTreeT` (append/flush/prove
with `flush_to`). *Tests:* build N leaves, flush, then prove
inclusion for a **flushed** index and a **resident** index against a non-flushed
reference tree's root; consistency across a flush boundary.

**Phase 5 — Optional entry bundles** (`BundleWriter`) and docs (README "Usage"
snippet, link this design doc, add `merklecpp_tiles.h` to Doxygen inputs).

Deliverables: `merklecpp_tiles.h`; `test/tiles_*.cpp`; CMake wiring; optional
one-method core addition; README/docs updates.

---
