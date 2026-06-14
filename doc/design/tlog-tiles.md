# Design: Tiled storage and tile-backed proofs for merklecpp

Status: Proposal / Draft
Audience: merklecpp maintainers and contributors
Scope: Extend the header-only library to (a) persist the Merkle tree as
[tlog-tiles](https://c2sp.org/tlog-tiles) tile files progressively (on
compaction), and (b) serve **inclusion** and **consistency** proofs from those
tiles, from the in-memory tree, or from a combination of the two.

---

## 1. Goals and non-goals

### Goals

1. Write the tree to disk as a set of immutable, cacheable **tile files** using
   the [tlog-tiles](https://c2sp.org/tlog-tiles) file/directory layout and tile
   geometry (256-wide tiles, 8 tree levels per tile, partial-tile rules, path
   encoding).
2. Produce tiles **progressively**: each batch/flush writes only the newly
   completed tiles, and the write integrates with the existing `flush_to()`
   "compaction" so that flushed (evicted) subtrees are durably persisted before
   their nodes are freed from memory.
3. Provide an API to retrieve, **after compaction**, an **inclusion path** and a
   **consistency path**:
   - purely from tiles,
   - purely from the in-memory tree (existing behaviour), or
   - from a combination of tiles (old, flushed part) and the in-memory tree
     (recent frontier).
4. Keep everything **header-only** and templated, matching the existing
   `TreeT<HASH_SIZE, HASH_FUNCTION>` style.

### Hard constraints (from the request)

- **Hashing logic is unchanged.** No modification to `HASH_FUNCTION`, to node
  hashing, or to leaf handling.
- **Proofs remain compatible with the existing library.** A proof produced from
  tiles MUST be identical to (and verify against the same root as) the proof the
  existing library would produce via `Tree::path()` / `Tree::past_path()` /
  `Tree::root()` / `Tree::past_root()`.

### Non-goals

- Byte-level interoperability with *external* RFC 6962 / Go-ecosystem tlog
  clients. See [§2.3](#23-compatibility-statement): such interop is only
  possible if the consumer instantiates `TreeT` with an RFC 6962 combiner, which
  is **out of scope** here and not required by this design.
- An HTTP server. We define the on-disk layout and the read/write/proof APIs;
  serving the static files is an application concern.

---

## 2. Background

### 2.1 tlog-tiles, the parts we adopt

A tiled log exposes the Merkle tree as a set of static resources:

- **Tiles** at `<prefix>/tile/<L>/<N>[.p/<W>]`, `application/octet-stream`.
  - `<L>` = level, decimal `0..63`, no leading zeros.
  - `<N>` = tile index within the level, encoded as zero-padded 3-digit path
    elements where **all but the last element are prefixed with `x`**. Example:
    `1234067` → `x001/x234/067`.
  - `.p/<W>` is present only for **partial** tiles; `<W>` is the width `1..255`.
- A **full tile** is exactly **256 hashes** wide (`256 * HASH_SIZE` bytes). The
  spec fixes `HASH_SIZE = 32` (SHA-256); here it is the template parameter.
- Level 0 hashes are **leaf hashes**. At level `L ≥ 1`, each hash is the Merkle
  Tree Hash of a *full* tile at level `L-1`. A tile spans **8 tree levels**: its
  256 entries are the leaves of a height-8 perfect subtree, and intra-tile
  internal nodes are reconstructed by hashing entries.
- The `n`-th tile at level `l` contains, for `i = 0..255`:
  `MTH(D[(n*256+i)*256^l : (n*256+i+1)*256^l])`.
  Its **start index** is `n*256^(l+1)`, its **end index** `(n+1)*256^(l+1)`.
- Some rightmost tiles are **partial**. The partial tile at level `l` for tree
  size `s` has `floor(s / 256^l) mod 256` entries. **Empty tiles MUST NOT be
  served.** Partial-tile entries are still complete `256^l`-leaf subtree roots;
  only the *count* is partial.
- **Entry bundles** at `<prefix>/tile/entries/<N>[.p/<W>]`: big-endian `uint16`
  length-prefixed raw entries. (See [§6.8](#68-entry-bundles-optional).)
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

The tile geometry, path encoding, partial-tile rules, and the inclusion /
consistency proof **algorithms** are exactly those of tlog-tiles / RFC 6962.
The **hash values stored in tiles** are produced by the tree's existing
`HASH_FUNCTION`. Because

1. merklecpp's tree shape equals the RFC 6962 left-balanced shape, and
2. tile entries and proof building blocks are *perfect-subtree roots* combined
   with the **same** `HASH_FUNCTION` the tree already uses,

a tile entry at level `L`, index `g` equals the in-memory full node of height
`8L+1` over leaves `[g·256^L, (g+1)·256^L)`, and a proof assembled from tiles is
**byte-identical** to the one `Tree::path()` / `Tree::past_path()` would emit.
Tile-derived proofs therefore verify with the unchanged `PathT::verify()`.

> External RFC 6962 interop would additionally require RFC 6962 domain
> separation (`SHA256(0x00‖entry)` leaves, `SHA256(0x01‖l‖r)` nodes). merklecpp
> already lets a consumer pass such a `HASH_FUNCTION`; doing so is **optional and
> out of scope**. This design never assumes it.

---

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
- **Partial tiles.** Number of complete entries available at level `L` for size
  `s` is `entries_L = floor(s / 256^L)`. Tile `N` holds entries
  `[N·256, (N+1)·256)` clamped to `entries_L`. Width of tile `N`:
  `clamp(entries_L − N·256, 0, 256)`. The partial (rightmost) width is
  `entries_L mod 256`; width 0 ⇒ not served.

This yields a single read primitive that both proof types are built on:

```
subtree_root(level k, index j) = MTH(D[j·2^k : (j+1)·2^k])
   L = k / 8 ; r = k % 8
   first = j << r                       // first level-L entry covered
   N = first / 256 ; off = first % 256  // tile and offset within tile
   return perfect_root( tile(L, N)[off : off + 2^r] )   // single entry if r==0
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

Rooted at a configurable `prefix` directory on local disk:

```
<prefix>/
  tile/
    0/                            # level 0 (leaf hashes)
      000  001 ... 255            # full tiles (8192 B each for HASH_SIZE=32)
      x001/
        000 ... 255
      <N>.p/<W>                   # partial tile, e.g. 273.p/112
    1/                            # level 1 (roll-ups of level-0 full tiles)
      000 ...
    .../
    entries/                      # optional raw entry bundles
      000  001 ...
      <N>.p/<W>
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

- Full tile: `tile/<L>/<encode_tile_index(N)>`
- Partial tile: `tile/<L>/<encode_tile_index(N)>.p/<W>`
- Entry bundle: `tile/entries/<encode_tile_index(N)>[.p/<W>]`

Tile byte format: the `W` entries concatenated, each `HASH_SIZE` raw bytes
(`HashT::bytes`); a full tile is `256 * HASH_SIZE` bytes.

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
| `TileWriterT`        | compute & persist newly-complete full/partial tiles (write path)     |
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
static constexpr uint16_t TILE_WIDTH  = 256;   // 2^TILE_HEIGHT

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

### 6.2 Optional core accessor (non-hashing)

To let proofs be served partly from the resident tree, add one small read-only
method to `TreeT`. It performs **no hashing changes**: it navigates to an
existing node and returns its already-computed hash.

```cpp
// In TreeT (merklecpp.h). Returns true and sets `out` to MTH(D[j<<level:(j+1)<<level])
// iff that perfect subtree is fully present and resident in memory.
bool subtree_root(uint8_t level, uint64_t index, Hash& out);
```

Implementation sketch (reuses `walk_to`/descent; calls `hash()` only to realise
an already-defined node hash, exactly as `root()`/`path()` already do):

- require `(index << level)` ≥ `min_index()` and `((index+1) << level)` ≤
  `num_leaves()`; otherwise return `false`;
- descend to the node at the target position whose `height == level + 1`;
- if that node `is_full()`, ensure its hash is computed and return it; else
  (right-frontier non-perfect node) return `false`.

If maintainers prefer **zero** core changes, `MemoryHashSource` can instead be
derived from `past_path(...)` (a path already carries sibling subtree roots), at
the cost of extra walks. The accessor is the cleaner primitive and is
recommended; both options keep hashing untouched.

### 6.3 `TileStoreT` — disk I/O

```cpp
struct TileRef { uint8_t level; uint64_t index; uint16_t width; }; // width 0 ⇒ full

class TileStoreT {
public:
  explicit TileStoreT(std::filesystem::path prefix);

  // Path helpers (pure)
  static std::string encode_index(uint64_t n);
  std::filesystem::path tile_path(const TileRef&) const;
  std::filesystem::path entries_path(uint64_t n, uint16_t width = 0) const;

  // Tiles
  bool has_full_tile(uint8_t level, uint64_t index) const;
  std::vector<Hash> read_tile(const TileRef&) const;           // width entries
  void write_tile(const TileRef&, const std::vector<Hash>&);   // atomic (tmp+rename)

  // Single entry convenience (used heavily by TileHashSource)
  bool read_entry(uint8_t level, uint64_t global_entry, Hash& out, uint64_t size) const;
};
```

- Writes are **atomic** (write to a temp file, `rename`) so a crash never leaves
  a half-written immutable tile.
- Full tiles are written once and never rewritten (immutability). Partial tiles
  are overwritten as the frontier advances and removed once the covering full
  tile exists (spec-permitted; configurable).
- An in-process LRU cache of recently read tiles avoids repeated I/O during
  proof generation.

### 6.4 Write path — `TileWriterT` (progressive, on compaction)

```cpp
class TileWriterT {
public:
  struct Options {
    bool write_partial_tiles = true;   // required for arbitrary sizes
    bool write_higher_levels = true;   // roll up L>=1 each flush
    bool remove_superseded_partials = true;
  };

  TileWriterT(TileStoreT& store, Options = {});

  // Persist all newly-complete full tiles (all levels) and, for `size`,
  // the partial tiles. Incremental: only writes tiles not already present.
  // `leaf_at` supplies level-0 leaf hashes for [0, size) (e.g. Tree::leaf).
  void write_up_to(uint64_t size,
                   const std::function<const Hash&(uint64_t)>& leaf_at);
};
```

Algorithm (`write_up_to`):

```
for L = 0, 1, 2, ... while entries_L = size >> (8*L) is > 0:
    full_L = entries_L / 256
    for N in [first_unwritten_full(L) .. full_L):          # new full tiles only
        entries = [ entry(L, N*256 + i) for i in 0..255 ]
        store.write_tile({L, N, width=0}, entries)
    if write_partial_tiles:
        w = entries_L % 256
        if w > 0:
            entries = [ entry(L, full_L*256 + i) for i in 0..w-1 ]
            store.write_tile({L, full_L, width=w}, entries)
            if remove_superseded_partials: remove stale partials < full_L

entry(L, g):
   if L == 0: return leaf_at(g)
   else:      return perfect_root( store.read_tile({L-1, g, width=0}) )  # 256→1
```

- Level 0 reads leaf hashes via the supplied `leaf_at` (e.g. `Tree::leaf(i)`),
  which exist **before** flushing. Higher levels roll up from the level-0 tiles
  just written — independent of tree internals and robust to prior flushes.
- `first_unwritten_full(L)` is tracked in the store (probe `has_full_tile`, or a
  small persisted cursor) to keep each flush O(new tiles).

Compaction integration (the "on compaction" story):

```cpp
// In TiledTreeT::flush(): write durable tiles, THEN free memory.
writer.write_up_to(size, [&](uint64_t i) -> const Hash& { return tree.leaf(i); });
uint64_t covered = (size / TILE_WIDTH) * TILE_WIDTH;       // full level-0 coverage
tree.flush_to(covered - retention_margin);                 // never past `covered`
```

### 6.5 `HashSource` — tiles, memory, or both

```cpp
struct HashSource {                       // concept (duck-typed or virtual)
  // MTH(D[index<<level : (index+1)<<level]) for a perfect, aligned subtree.
  virtual bool subtree_root(uint8_t level, uint64_t index, Hash& out) const = 0;
  // Optional fast path for level-0 leaves (frontier).
  virtual bool leaf(uint64_t i, Hash& out) const { return subtree_root(0,i,out); }
};
```

- `TileHashSource{store, size}` — resolves from tiles using the
  `subtree_root` formula in [§3](#3-tile--merklecpp-mapping-the-math); returns
  `false` when the requested subtree extends beyond what tiles cover for `size`.
- `MemoryHashSource{tree}` — `tree.subtree_root(level,index,out)` (or the
  `past_path`-derived fallback); resolves only resident, full subtrees
  (`≥ min_index`).
- `CombinedHashSource{mem, tiles}` — try memory first (no I/O), then tiles
  (configurable order). This is the "combination of tiles and in-memory tree".

### 6.6 `ProofEngineT` — inclusion & consistency

All three proof building blocks reduce to `mth_range` over a `HashSource`.
Returned `PathT` objects are byte-identical to `Tree::path` / `Tree::past_path`.

```cpp
class ProofEngineT {
public:
  explicit ProofEngineT(const HashSource& src);

  Hash root(uint64_t size) const;                 // = mth_range(0, size)

  // Inclusion path for leaf `index` in the tree of `size` leaves.
  // Equivalent to Tree::path(index) when size==num_leaves(),
  // and to Tree::past_path(index, size-1) otherwise.
  std::shared_ptr<Path> inclusion_proof(uint64_t index, uint64_t size) const;

  // RFC 6962 consistency proof that size `m` is a prefix of size `n` (m<=n).
  std::vector<Hash> consistency_proof(uint64_t m, uint64_t n) const;

  // Verifier (consistency is new to merklecpp; inclusion reuses PathT::verify).
  static bool verify_consistency(uint64_t m, uint64_t n,
                                 const Hash& old_root, const Hash& new_root,
                                 const std::vector<Hash>& proof);
};
```

Inclusion (top-down; element order/`direction` chosen to match `Tree::path`):

```
elements = []                         # leaf→root order via push_front
lo = 0, hi = size, idx = index
while hi - lo > 1:
    k = largest_pow2_lt(hi - lo)      # split at lo+k
    if idx - lo < k:                  # target in left ⇒ sibling on the RIGHT
        sib = mth_range(lo+k, hi);  dir = PATH_RIGHT;  hi = lo + k
    else:                             # target in right ⇒ sibling on the LEFT
        sib = mth_range(lo, lo+k);  dir = PATH_LEFT;   lo = lo + k
    elements.push_front({sib, dir})
leaf = src.leaf(index)
return Path(leaf, index, elements, max_index = size - 1)
```

Consistency (RFC 6962 `SUBPROOF`):

```
consistency_proof(m, n):              # 0 < m <= n
    if m == n: return []
    subproof(m, lo=0, hi=n, complete=true)

subproof(m, lo, hi, complete):
    if m == hi - lo:
        if not complete: proof.push_back(mth_range(lo, hi))
        return
    k = largest_pow2_lt(hi - lo)
    if m <= k:
        subproof(m, lo, lo+k, complete)
        proof.push_back(mth_range(lo+k, hi))
    else:
        subproof(m-k, lo+k, hi, false)
        proof.push_back(mth_range(lo, lo+k))
```

Because every emitted hash is an `mth_range` computed with `HASH_FUNCTION`, the
consistency proof reconciles `Tree::past_root(m-1)` with
`Tree::past_root(n-1)` — i.e. it is consistent with the existing library.

### 6.7 `TiledTreeT` — convenience wrapper

```cpp
class TiledTreeT {
public:
  struct Config {
    std::filesystem::path prefix;
    uint64_t retention_margin = 0;       // keep this many recent leaves resident
    bool compact_on_flush = false;       // opt in to dropping tiled leaves
    TileWriterT::Options writer = {};
  };
  explicit TiledTreeT(Config);

  void append(const Hash& leaf_hash);                 // tree.insert
  uint64_t size() const;                              // tree.num_leaves
  Hash root();                                        // tree.root

  // Write newly-complete tiles + partials. Compaction (dropping already-tiled
  // leaves from memory) happens only if compact_on_flush.
  Stats flush();

  // Drop from memory the leaves already covered by a full tile (opt-in);
  // proofs for dropped leaves remain available from the tiles.
  uint64_t compact();

  // Proofs over tiles ∪ resident tree (works for flushed indices).
  std::shared_ptr<Path> inclusion_proof(uint64_t index, uint64_t size);
  std::vector<Hash>     consistency_proof(uint64_t m, uint64_t n);

  Tree& tree();                                       // escape hatch
};
```

Stateless alternative for callers with their own storage: free functions
`write_tiles(tree, store, size, opts)` and a `ProofEngineT` built on a
`CombinedHashSource`, so the wrapper is optional sugar.

### 6.8 Entry bundles (optional)

merklecpp never sees raw entries (callers insert pre-computed leaf hashes), so
entry bundles are an **application-owned** add-on, included for completeness:

- `tile/entries/<N>[.p/<W>]` stores big-endian `uint16` length-prefixed entries,
  256 per full bundle.
- The application is responsible for the leaf-hash derivation it uses (e.g.
  `leaf_hash = H(entry)`); merklecpp stores whatever leaf hash is inserted.
- A `BundleWriter` mirrors `TileWriterT` (write full bundles on 256 boundaries,
  partial bundle for the current size). Marked optional/secondary.

---

## 7. Progressive production & compaction

The pairing of tile writing with `flush_to` gives the central correctness
invariant:

> **Coverage invariant.** Never flush past the durably-written full-level-0
> coverage: `min_index() ≤ covered`, where
> `covered = floor(size / 256) * 256` after `write_up_to(size, …)`.

Per flush:

1. `append(...)` new leaf hashes; compute `root()`.
2. `write_up_to(size, leaf_at)` — persist newly-complete **full** tiles at all
   levels (incremental) and the **partial** tiles for `size`.
3. *(optional)* `compact()` → `flush_to(covered − retention_margin)` — reclaim
   memory, only when `compact_on_flush` is set (or `compact()` is called
   explicitly). By default nothing is dropped and the tree stays whole.

Given the invariant, every leaf and every perfect subtree is resolvable:

- leaf `i < covered` ⇒ in a full level-0 tile; leaf `i ≥ min_index` ⇒ resident.
  Since `min_index ≤ covered`, **every** leaf is in tiles ∪ memory.
- `mth_range` resolves a perfect subtree directly when it lies wholly in tiles
  (`end ≤ covered`) or wholly in memory (`start ≥ min_index`); otherwise it
  splits and recurses, terminating at resolvable pieces (leaves at worst).

Hence inclusion and consistency proofs are always producible after compaction,
from tiles alone, from memory alone (when nothing relevant was flushed), or from
the combination — satisfying the request.

Cost per flush is `O(new full tiles + partial tiles)`; higher-level tiles
are cheap roll-ups of 256 child hashes. Proof generation is
`O(log(size))` `mth_range` calls, each at most one tile read plus a `≤ 256`-leaf
roll-up, served from cache in the common case.

---

## 8. Pruning / minimum index

tlog-tiles pruning maps directly onto merklecpp:

- The log's *minimum index* is `tree.min_index()` (== `num_flushed`).
- "Deny tiles/bundles whose end index ≤ minimum index" is implemented by the
  serving layer consulting `min_index()`; on-disk tiles may be retained
  (recommended by the spec) so historical proofs remain producible.
- The unpruned default is `min_index() == 0` (no `flush_to`).

`flush_to` is the mechanism; *retention policy* (when/whether to prune) is left
to the application, exactly as the spec leaves it to log ecosystems.

---

## 9. Worked examples (used as test vectors)

**Size 256:** one full level-0 tile (`tile/0/000`) and one partial level-1 tile
of width 1 (`tile/1/000.p/1`). No partial level-0 tile (width `256 mod 256 = 0`,
not served).

**Size 70 000:** 273 full level-0 tiles + one partial level-0 of width 112
(`70000 mod 256`); one full level-1 tile + one partial level-1 of width 17
(`273 mod 256`); one partial level-2 of width 1 (`floor(70000/65536) = 1`). These
exact counts are asserted in tests.

**Index encoding:** `1234067 → x001/x234/067`; `1000 → x001/000`; `255 → 255`.

---

## 10. Implementation plan

Each phase is independently testable; phases 1–4 require **no** core changes.

**Phase 0 — Scaffolding.** Add `merklecpp_tiles.h` (includes `merklecpp.h`),
`merkle::tiles` namespace, geometry constants, default aliases. Wire a new test
group in `test/CMakeLists.txt` following `add_merklecpp_test`.

**Phase 1 — Coordinates & store.** `TileCoord`/`encode_index`, `TileRef`,
`TileStoreT` (path building, atomic `write_tile`, `read_tile`). *Tests:*
index-encoding vectors; tile byte round-trip.

**Phase 2 — Write path.** `TileWriterT::write_up_to` (level-0 from `leaf_at`,
roll-ups, partials, incremental cursor; superseded-partial removal). *Tests:*
size 256 & 70 000 produce exactly the expected tile set/widths; full-tile
immutability (re-running writes nothing new).

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

## 11. Testing strategy

The existing library is the **oracle** — this is how we guarantee "proofs remain
compatible":

- **Equality, not just verification.** Assert `*inclusion_proof(i,size) ==
  *tree.path(i)` using the existing `PathT::operator==`, and likewise against
  `past_path`. This is stronger than `verify()` and pins byte-compatibility.
- **Root agreement.** `ProofEngineT::root(size)` == `tree.root()` /
  `tree.past_root(size-1)` for many random sizes.
- **Spec vectors.** Exact tile counts/widths for sizes 256 and 70 000; the empty
  partial-tile rule; index-encoding strings (incl. `x001/x234/067`).
- **Flush/compaction.** Cross-check proofs for flushed indices against a twin
  tree that was never flushed (same inserts) — roots must match and proofs must
  verify.
- **Consistency proofs.** Standard RFC 6962 `verify_consistency` plus the
  reconciliation-of-`past_root`s check; randomized `m < n`.
- **Templating.** Run a subset under `Tree`, `Tree384`, `Tree512` to confirm
  hash-size independence.
- **Style.** Mirror existing standalone-`main()` tests (`test/util.h`,
  `make_hashes`, `get_timeout`) and register via `add_merklecpp_test`.

---

## 12. Risks, edge cases, open questions

- **External interop (by design, no).** With the default combiner the tiles are
  *not* byte-compatible with RFC 6962 tooling. Documented in
  [§2.3](#23-compatibility-statement); opt-in via an RFC 6962 `HASH_FUNCTION` is
  the consumer's choice and out of scope.
- **Filesystem dependency.** Tile I/O needs `<filesystem>`/`<fstream>`; isolated
  in the companion header so the core stays dependency-free.
- **Partial-tile lifecycle.** Overwriting/removing partials must never race a
  reader; atomic rename + "fetch full tile as fallback" (spec-sanctioned)
  mitigate this.
- **`flush_to` alignment.** Must flush only to a 256-multiple (minus retention)
  to uphold the coverage invariant; enforced inside `TiledTreeT::flush`.
- **Rollback vs. immutable tiles.** Tiles are write-once, so rolling the tree
  back (`retract_to`) over already-tiled entries would leave stale, never-
  rewritten tiles and is forbidden: `TiledTreeT::retract_to` throws if the
  resulting size is below `flushed_size()`, permitting only rollback of the
  un-tiled (post-flush) frontier. Retracting the underlying tree directly
  via `tree_ref()` bypasses this guard and must be avoided.
- **Very large indices.** Index math uses `uint64_t`; encoding handles
  multi-group indices. Level bound `≤ 63` per spec (8 suffices for `2^64`).
- **Open question — `subtree_root` in core vs. `past_path`-derived memory
  source.** Recommend the tiny non-hashing accessor; falls back to zero-core-
  change if maintainers prefer. Either keeps hashing untouched.

---

## 13. Build & backwards-compatibility impact

- **Additive only.** New header + tests; existing `merklecpp.h` behaviour and
  API are unchanged. Default builds (no tiles) are unaffected.
- **No new mandatory dependencies.** Roll-ups/proofs use the existing
  `HASH_FUNCTION` (two-hash combiner), so `sha256_compress` suffices; OpenSSL
  stays optional.
- **Optional core change** is a single read-only accessor that performs no
  hashing and does not alter any existing code path.
- **CMake.** Add a `tiles` test group guarded like the others; no changes to the
  `merklecpp` INTERFACE target are required for consumers that don't use tiles.
```
