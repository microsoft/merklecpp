# Design: Tiled storage and tile-backed proofs for merklecpp

**Scope:** Persist trees progressively during compaction using
[tlog-tiles](https://c2sp.org/tlog-tiles), then serve inclusion and consistency
proofs from tiles, memory, or both.

---

## 1. Requirements

| Area | Contract |
|---|---|
| Format | SHA-256 uses the C2SP payload, paths, and geometry: 256 hashes per tile and 8 tree levels per tile. SHA-384 and SHA-512 use explicitly namespaced merklecpp extensions with the same width. |
| Persistence | A flush writes only newly completed, balanced tiles and makes them durable before `flush_to()` frees their nodes. Partial tiles are never written or read; the incomplete frontier remains in memory. |
| Immutability | Higher-level writers treat published tiles as immutable. The low-level store allows atomic replacement only for repair and idempotent publication. |
| Proofs | Tiled inclusion paths must equal `Tree::path()` / `Tree::past_path()` output and verify against the same `root()` / `past_root()`. Consistency proofs use the same tree and combiner. |
| Hashing | Leaf handling, node hashing, and `HASH_FUNCTION` are unchanged. |
| API | The implementation remains header-only and follows the existing `TreeT<HASH_SIZE, HASH_FUNCTION>` template style. |
| Concurrency | The API performs no synchronization. Callers must serialize access to each object, including `const` proof reads that update the `TileHashSourceT` LRU cache, and all writers sharing a prefix. Independent objects may read while the serialized writer publishes tiles atomically. Concurrent writers are out of scope. |

**Non-goals:** standardizing the SHA-384/512 extensions; adding RFC 6962 leaf
hashing or domain separation; coordinating concurrent writers; and providing an
HTTP server. Applications remain responsible for compatible hashing and for
serving the static resources.

## 2. Format and compatibility

### 2.1 Resource model

| Resource | Layout and meaning |
|---|---|
| Tile | `<prefix>/<algorithm>-256w/tile/<L>/<N>`, `application/octet-stream`; `L` is decimal `0..63` without leading zeros, and `N` uses the grouped encoding in [section 4](#4-storage-layout-and-publication). |
| Full tile | 256 hashes. Level 0 stores leaf hashes; each level-`L` entry for `L >= 1` is the Merkle Tree Hash of one complete level-`L-1` tile. |
| Entry bundle | `<prefix>/<algorithm>-256w/tile/entries/<N>`; 256 raw entries encoded as big-endian `uint16` length-prefixed values. See [section 5.4](#54-entry-bundles-optional). |
| Pruning | Tiles or bundles ending at or before a log's minimum index may be denied, matching `flush_to()` / `min_index()`. |

A tile spans eight tree levels. Tile `n` at level `l` contains, for
`i = 0..255`:

`MTH(D[(n*256+i)*256^l : (n*256+i+1)*256^l])`

Its leaf range is `[n*256^(l+1), (n+1)*256^(l+1))`. Only a full-tile prefix is
stored; the remaining frontier stays in memory.

### 2.2 merklecpp model

- `TreeT<HASH_SIZE, HASH_FUNCTION>` is left-balanced with the RFC 6962 shape.
  A full node of height `h` covers `2^(h-1)` aligned leaves.
- Internal nodes use `HASH_FUNCTION(left, right, out)` over two child hashes,
  without domain separation.
- `flush_to(index)` replaces the compacted prefix with hash-only nodes, removes
  its leaves, and raises `min_index()`. Memory alone can no longer prove leaves
  below that index.
- Existing proof surfaces are `root()`, `past_root(i)`, `path(i)`,
  `past_path(i, as_of)`, and `PathT::verify()` / `root()`. merklecpp does not
  currently expose a classic two-size consistency proof; this design adds one
  that reconciles historical roots with the same combiner.

### 2.3 Algorithms and namespaces

| Algorithm | Hash bytes | Full-tile bytes | Root | Status |
|---|---:|---:|---|---|
| SHA-256 | 32 | 8,192 | `sha256-256w` | C2SP payload and resource layout |
| SHA-384 | 48 | 12,288 | `sha384-256w` | merklecpp extension |
| SHA-512 | 64 | 16,384 | `sha512-256w` | merklecpp extension |

The width remains 256 hashes for every algorithm; the namespace prevents
different hash sizes from sharing files while retaining the C2SP `tile/...`
layout. Built-in SHA functions select their names automatically. Custom names
must be lowercase, path-safe short names, and recognized SHA names must match
`HASH_SIZE`.

Exact external interoperability also requires RFC 6962-compatible leaf and node
hashing; merklecpp continues to use the caller's `HASH_FUNCTION`.

## 3. Tile mapping and proof math

Let `TILE_HEIGHT = 8`, `TILE_WIDTH = 256 = 2^8`, and `MTH` use the tree's
`HASH_FUNCTION`.

- Entry `i` of tile `(L, N)`, with `g = N*256+i`, is
  `MTH(D[g*256^L : (g+1)*256^L])`: a perfect subtree of `2^(8L)` leaves and an
  in-memory full node of height `8L+1`.
- A level-`L` tile rolls up level `L-1` tiles:
  `tile(L,N)[i] = perfect_root(tile(L-1, N*256+i)[0..255])`. The writer
  therefore needs only level-0 leaf hashes and lower-level tiles.
- For `k = 8L+r`, the root of `2^r` aligned entries (`0 <= r <= 8`) comes from
  one level-`L` tile by hashing that run; `r = 0` reads one entry.
- At size `s`, `full_tiles_L = floor(floor(s / 256^L) / 256)` and
  `covered = floor(s/256)*256`. `[covered, s)` remains in memory. If a
  higher-level tile is absent, lookup descends to available lower-level tiles
  and rolls them up.

Both proof types use one perfect-subtree primitive:

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
mth_range(a, b):                    // MTH(D[a:b]); a aligned to 2^ceil(log2(b-a))
   w = b - a
   if w == 2^k and source can resolve subtree_root(k, a>>k): return it
   k = largest power of two < w     // RFC 6962 split
   return HASH_FUNCTION( mth_range(a, a+k), mth_range(a+k, b) )
```

If the selected source cannot resolve a perfect subtree directly, `mth_range`
splits until tile or memory sources can resolve each piece.

---

## 4. Storage layout and publication

All resources live below a configurable local `prefix`:

```
<prefix>/<algorithm>-256w/tile/
  0/                       # level-0 full tiles
    000 ... 255
    x001/000 ... 255
  1/                       # roll-ups of level-0 tiles
    000 ...
  ...
  entries/                 # optional full entry bundles
    000 ...
```

`encode_tile_index` groups decimal indices into zero-padded three-digit
components and prefixes every non-final component with `x`:

```
encode_tile_index(N):
   parts = split N into base-1000 groups, each formatted "{:03}"
   prefix every group except the last with "x"
   return join(parts, "/")
```

Examples: `5 -> "005"`, `255 -> "255"`, `1000 -> "x001/000"`, and
`1234067 -> "x001/x234/067"`.

The complete paths are
`<prefix>/<algorithm>-256w/tile/<L>/<encoded-N>` and
`<prefix>/<algorithm>-256w/tile/entries/<encoded-N>`. A tile concatenates 256
`HashT::bytes` values and is therefore `256 * HASH_SIZE` bytes; `256w` counts
hashes, not bytes.

### Publication guarantees

- Create a unique temporary file exclusively, sync its contents with `fsync`,
  and atomically replace the destination. Independent readers observe a complete
  old or new resource while the serialized writer publishes.
- On POSIX, sync each newly created directory link and the destination
  directory after rename. A higher-level retry rechecks the directory chain and
  destination even when the file is already visible.
- Wrong-size files are not durable full tiles and may be repaired. Higher-level
  writers enforce immutability; the low-level primitive permits replacement for
  repair or idempotent publication and trusts callers not to change valid
  content.

## 5. Architecture and API

```
            append(leaf hash)            flush()
   caller ───────────────▶  TiledTreeT ───────────────▶  TileStoreT
                              │ owns Tree                    ▲
              inclusion / consistency proofs                │ tiles
                              ▼                              │
                      ProofEngineT ──▶ HashSourceT ◀─────────┘
                                         ├─ MemoryHashSourceT
                                         ├─ TileHashSourceT
                                         └─ CombinedHashSourceT
```

`merklecpp_tiles.h` contains the public `merkle::tiles` API and includes
`merklecpp.h`; internal OS operations live in `merklecpp_pal.h` under
`merkle::pal`.

| Component | Responsibility |
|---|---|
| PAL helpers | Exclusive creation, atomic replacement, and durability |
| `TileRef` / encoder | Full-tile identity and index paths |
| `TileStoreT` | Local tile and entry-bundle I/O |
| `TileWriterT` | Persist newly completed full tiles |
| Hash sources | Resolve subtree roots from memory, tiles, or both |
| `ProofEngineT` | Roots, inclusion/consistency proofs, and verification |
| `TiledTreeT` | `append`, `flush`, proof APIs, and compaction |

`TileHashSourceT` owns the proof-read LRU cache; `TileStoreT` does not cache.
`MemoryHashSourceT` uses the `TreeT::subtree_root` accessor.

### 5.1 Types and aliases

```cpp
namespace merkle::tiles {

static constexpr uint16_t TILE_HEIGHT = 8;
static constexpr uint16_t TILE_WIDTH = uint16_t{1U << TILE_HEIGHT};
static constexpr uint8_t MAX_TILE_LEVEL = 63;

template <size_t HASH_SIZE,
          void HASH_FUNCTION(const HashT<HASH_SIZE>&,
                             const HashT<HASH_SIZE>&,
                             HashT<HASH_SIZE>&)>
class TileStoreT;

using TileStore =
  TileStoreT<Tree::Hash::size_bytes, Tree::hash_function>;
using TiledTree =
  TiledTreeT<Tree::Hash::size_bytes, Tree::hash_function>;
// Writer, hash-source, proof-engine, and entry-bundle aliases follow the same
// template pattern.

}
```

Aliases derive hash size and function from the core tree. Roll-up and proof code
combine two `HASH_SIZE`-byte hashes with that same function, so the default
built-in `sha256` function adds no OpenSSL dependency.

### 5.2 `TileStoreT`

```cpp
struct TileRef { uint8_t level; uint64_t index; }; // full tiles only

class TileStoreT {
public:
  explicit TileStoreT(std::filesystem::path prefix);
  TileStoreT(std::filesystem::path prefix,
             const std::string& hash_algorithm_short_name);

  static std::string storage_directory_name(const std::string& algorithm);
  static std::string encode_index(uint64_t n);
  std::filesystem::path tile_path(const TileRef&) const;
  std::filesystem::path entries_path(uint64_t n) const;

  bool has_full_tile(uint8_t level, uint64_t index) const;
  std::vector<Hash> read_tile(const TileRef&) const;         // 256 hashes
  void write_tile(const TileRef&, const std::vector<Hash>&); // durable replace
};
```

Built-in hash functions select `sha256`, `sha384`, or `sha512`; custom
algorithms use the explicit-name constructor. The concurrency contract is in
[section 1](#1-requirements), and publication semantics are in
[section 4](#4-storage-layout-and-publication).

### 5.3 `TileWriterT`

`TileWriterT` incrementally persists every newly complete tile at every level:

```cpp
class TileWriterT {
public:
  explicit TileWriterT(TileStoreT& store);

  struct Stats { uint64_t full_written; };
  Stats write_up_to(
    uint64_t size,
    const std::function<const Hash&(uint64_t)>& leaf_at);
};
```

For each level `L`, `write_up_to` computes the number of complete level entries
as `size >> (8 * L)`, then writes each complete group of 256 entries. Level 0
reads leaf hashes from `leaf_at`; higher levels roll up the complete child tiles
already on disk. The incomplete frontier is never written.

A writer caches the next full-tile index at each level. A fresh writer rebuilds
each cursor with a bounded scan of the confirmed contiguous prefix, then checks
later indices individually so malformed or missing interior tiles are repaired.
Existing valid tiles are immutable. If publication succeeds but a directory
sync fails, a retry confirms the visible tile's durability instead of rewriting
it.

The resulting tile counts provide useful test vectors:

- Size 256 produces one level-0 tile and no level-1 tile.
- Size 70,000 produces 273 level-0 tiles and one level-1 tile.
- Size 65,536 completes both level-0 tile 255 and level-1 tile 0.

### 5.4 Entry bundles (optional)

Entry bundles are application-owned because merklecpp receives precomputed leaf
hashes, not raw entries. Each full bundle stores 256 big-endian `uint16`
length-prefixed entries at `<algorithm>-256w/tile/entries/<N>`. The application
defines the leaf derivation (for example, `leaf_hash = H(entry)`); merklecpp
stores the supplied leaf hash unchanged. `EntryBundleWriterT` mirrors
`TileWriterT`: it writes only complete 256-entry bundles, confirms existing
bundles before reusing them, and leaves the incomplete tail with the application.

### 5.5 `TreeT::subtree_root`

Proofs over the resident frontier use one core accessor:

```cpp
std::optional<Hash> subtree_root(uint8_t level, size_t index);
```

It returns the existing root of the complete subtree spanning
`[index << level, (index + 1) << level)`. The method rejects overflow, flushed
or out-of-range leaves, and non-perfect frontier nodes by returning
`std::nullopt`. Like `root()` and `path()`, it may materialize pending nodes and
compute dirty hashes, but does not change logical leaf contents or hashing
semantics.

### 5.6 Hash sources

```cpp
struct HashSourceT {
  // MTH(D[index<<level : (index+1)<<level]) for a perfect, aligned subtree.
  virtual bool subtree_root(uint8_t level, uint64_t index, Hash& out) const = 0;
  virtual bool leaf(uint64_t i, Hash& out) const { return subtree_root(0,i,out); }
};
```

- `TileHashSourceT{store, size}` resolves from **full tiles** using the
  `subtree_root` formula in [section 3](#3-tile-mapping-and-proof-math) (`size` is
  rounded down to a whole number of full tiles); returns `false` when the
  requested subtree reaches into the un-tiled frontier.
- `MemoryHashSourceT{tree}` resolves only resident, full subtrees through
  `TreeT::subtree_root`.
- `CombinedHashSourceT{primary, secondary}` tries the primary source first,
  then falls back to the secondary. Using memory first avoids I/O for the
  resident frontier.
- `TileHashSourceT` mutates its LRU cache during `const` reads. It and every
  `ProofEngineT` that refers to it require caller-provided synchronization when
  shared between threads.

### 5.7 `ProofEngineT`

All three proof building blocks reduce to `mth_range` over a `HashSourceT`.
Returned `PathT` objects are byte-identical to `Tree::path` / `Tree::past_path`.

```cpp
class ProofEngineT {
public:
  using Source = HashSourceT<HASH_SIZE, HASH_FUNCTION>;
  explicit ProofEngineT(const Source& source);

  Hash root(uint64_t size) const;                 // = mth_range(0, size)

  // Inclusion path for leaf `index` in the tree of `size` leaves.
  // Equivalent to Tree::path(index) when size==num_leaves(),
  // and to Tree::past_path(index, size-1) otherwise.
  std::shared_ptr<Path> inclusion_proof(uint64_t index, uint64_t size) const;

  // RFC 6962 consistency proof that size `m` is a prefix of size `n` (m<=n).
  std::vector<Hash> consistency_proof(uint64_t m, uint64_t n) const;
  std::vector<Hash> consistency_proof_from_indices(
    uint64_t first_index, uint64_t second_index) const;

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

### 5.8 `TiledTreeT` - convenience wrapper

```cpp
class TiledTreeT {
public:
  struct Config {
    std::filesystem::path prefix;
    uint64_t retention_margin = 0;       // keep at least this many resident
    bool compact_on_flush = false;       // opt in to dropping tiled leaves
  };
  explicit TiledTreeT(Config);
  TiledTreeT(Config, const std::string& hash_algorithm_short_name);

  void append(const Hash& leaf_hash);                 // tree.insert
  uint64_t size() const;                              // tree.num_leaves
  Hash root();                                        // tree.root
  uint64_t flushed_size() const;                      // successful tile boundary
  uint64_t immutable_size() const;                    // rollback boundary

  // Write newly-complete full tiles. Compaction (dropping already-tiled
  // leaves from memory) happens only if compact_on_flush.
  Stats flush();

  // Drop from memory the leaves already covered by a full tile (opt-in); the
  // un-tiled frontier is always retained, and proofs for dropped leaves remain
  // available from the tiles.
  uint64_t compact();

  // Roll back only beyond immutable_size().
  void retract_to(size_t index);

  // Proofs over tiles ∪ resident tree (works for flushed indices).
  std::shared_ptr<Path> inclusion_proof(uint64_t index, uint64_t size);
  std::vector<Hash>     consistency_proof(uint64_t m, uint64_t n);
  std::vector<Hash>     consistency_proof_from_indices(uint64_t i, uint64_t j);

  Tree&  tree_ref();                                  // mutable escape hatch
  Store& store_ref();                                 // mutable escape hatch
};
```

`TiledTreeT` performs no internal locking. The caller must serialize every
operation on a shared instance, including proof calls.

Callers with their own storage can construct a `TileWriterT` and call
`write_up_to`, then build a `ProofEngineT` on a `CombinedHashSource`, so the
wrapper is optional sugar.

## 6. Progressive production and compaction

The pairing of tile writing with `flush_to` gives two central correctness
invariants:

> **Compaction invariant.** Retain the final leaf of the last fully successful
> flush: `flushed_size == 0 || min_index() < flushed_size`.
>
> **Immutability invariant.** Never roll back below a full-tile boundary that a
> flush may have published: `size >= immutable_size`.

`TiledTreeT` is fresh-only: its configured directory may exist, but the
algorithm-qualified `tile` subdirectory must be absent. Construction atomically
creates that directory to claim exclusive ownership. Tile files do not carry
the size, root, hash identity, or ownership information needed to reopen or
share a tree safely. The lower-level `TileWriterT` supports resume for
applications that persist and validate the matching tree state themselves.

Per flush:

1. Append new leaf hashes and compute the root as needed.
2. Compute `covered = floor(size / 256) * 256` and advance `immutable_size` to
   `covered` before any write can publish a full tile.
3. Call `write_up_to(size, leaf_at)` to persist newly complete full tiles at all
   levels.
4. After every level succeeds, set `flushed_size = covered`.
5. Optionally, `compact()` computes an aligned retention target capped below
  nonzero `covered`, then calls `flush_to(target)`. This reclaims memory only
  when `compact_on_flush` is set or `compact()` is called explicitly. It keeps
  at least `retention_margin` recent leaves; alignment can retain up to 255
  additional tiled leaves, and a zero margin keeps the final tiled leaf. The
  entire un-tiled frontier always remains resident.

If tile writing fails, the final two steps do not run. `immutable_size` stays
advanced to prevent stale-tile rollback, while `flushed_size` stays at the last
complete all-level write so proofs and compaction do not trust an incomplete
flush.

Given these invariants, every leaf and every perfect subtree is resolvable:

- A leaf below `covered` is in a full level-0 tile; a leaf at or above
  `min_index` is resident. Since `min_index <= covered`, every leaf is in tiles,
  memory, or both. The frontier `[covered, size)` is always resident because
  compaction never flushes past `covered`.
- `mth_range` resolves a perfect subtree directly when it lies wholly in tiles
  (`end <= covered`) or wholly in memory (`start >= min_index`). Otherwise it
  splits and recurses until each piece is resolvable. A subtree within
  `covered` whose level has no completed full tile descends to the highest
  available full tile.

Inclusion and consistency proofs therefore remain available after compaction,
from full tiles for the tiled prefix, memory for the resident frontier, or the
combination. A flush costs `O(new full tiles)`; higher-level tiles are roll-ups
of 256 child hashes. Proof generation performs `O(log(size))` range operations,
with repeated tile reads served from the per-source cache.

## 7. Pruning and minimum index

tlog-tiles pruning maps directly onto merklecpp:

- The log's minimum index is `tree.min_index()` (equal to `num_flushed`).
- A serving layer can deny tiles or bundles whose end index is at or below the
  minimum index. On-disk tiles may instead be retained so historical proofs
  remain producible.
- The unpruned default is `min_index() == 0` (no `flush_to`).

`flush_to` provides the mechanism; the application owns the retention policy,
as in the tlog-tiles ecosystem.

## 8. Delivery plan

Phases 0-4 now deliver the storage primitives, incremental tile and entry-bundle
writers, hash sources, proof engine, the only required core accessor, and the
lifecycle wrapper. Later PRs deliver user documentation and performance
coverage; no further core changes are planned.

| Phase | Scope | Key tests |
|---|---|---|
| 0. Scaffolding | Headers, PAL, namespace, geometry, aliases, and CMake test wiring | Public-header and build integration |
| 1. Coordinates/store | `TileRef`, index/path encoding, `TileStoreT`, durable atomic I/O, entry-bundle primitives | Encoding vectors; algorithm roots; 256-hash SHA-256/384 tiles; round trips; file/symlink collisions |
| 2. Writers | Incremental `TileWriterT::write_up_to` from `leaf_at`, roll-ups, and `EntryBundleWriterT`; full resources only | Sizes 256 and 70,000 produce the exact tile set; repeated writes preserve immutability |
| 3. Proof engine | `TreeT::subtree_root`; tile, memory, and combined hash sources; roots; inclusion/consistency proofs and verification | Tile roots equal tree roots; inclusion equals `path()` / `past_path()` and verifies; consistency reconciles `past_root()` values |
| 4. Combined tree | `TiledTreeT` append, flush, proof, and compaction APIs | Prove flushed and resident leaves against a non-flushed reference; consistency across a flush boundary |
| 5. Documentation/performance | README usage, design link, and tile-backed benchmarks | Documentation and benchmark coverage |

Delivered through phase 4 are `merklecpp_tiles.h`, `merklecpp_pal.h`,
`test/tiles_*.cpp`, CMake wiring, the core accessor, the lifecycle wrapper, and
design updates.

## 9. Risks and edge cases

- **External interop (by design, no).** With the default combiner the tiles are
  not byte-compatible with RFC 6962 tooling. See
  [section 2.3](#23-algorithms-and-namespaces); opting into a compatible
  `HASH_FUNCTION` is the consumer's choice and out of scope.
- **Filesystem dependency.** Tile I/O needs `<filesystem>`/`<fstream>`; isolated
  in the companion header so the core stays dependency-free.
- **Immutable full tiles.** A tile is emitted only after all of its entries are
  final, and every emitted tile is write-once. A stand-alone tile reader cannot
  serve the frontier; that is the in-memory tree's job (or the application must
  keep it elsewhere).
- **`flush_to` alignment.** Compaction normally flushes to a 256-multiple
  derived from retention. When that target equals `flushed_size`, it stops one
  leaf earlier so `TreeT` can still retract to exactly that size. This one-leaf
  overlap is enforced inside `TiledTreeT::compact`.
- **Rollback vs. immutable tiles.** Tiles are write-once, so rolling the tree
  back (`retract_to`) over a range that a flush may have published would leave
  stale, never-rewritten tiles. `TiledTreeT::retract_to` therefore throws if the
  resulting size is below `immutable_size()`. A failed flush may advance
  `immutable_size()` without advancing `flushed_size()`; retry with the same
  tree state. Retracting the underlying tree directly via `tree_ref()` bypasses
  this guard, can make the size boundaries inconsistent or non-monotonic, and
  must be avoided. Files written through `store_ref()` are trusted without
  checking that they match the tree and can invalidate proofs after compaction.
- **No internal synchronization.** Every tiled-storage object and shared store
  prefix requires external serialization. This includes `const` proof reads,
  which update the tile cache.
- **Very large indices.** Index math uses `uint64_t`; encoding handles
  multi-group indices. Level bound `<= 63` per spec (8 suffices for `2^64`).
  Resume scans are bounded by the requested tree size and cannot follow sparse
  files beyond that range.
