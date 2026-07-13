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
| Concurrency | The API performs no synchronization. Callers must serialize access, including across objects sharing a prefix and through `const` proof reads that update the `TileHashSourceT` LRU cache. |

**Non-goals:** standardizing the SHA-384/512 extensions; adding RFC 6962 leaf
hashing or domain separation; and providing an HTTP server. Applications remain
responsible for compatible hashing and for serving the static resources.

## 2. Format and compatibility

### 2.1 Resource model

| Resource | Layout and meaning |
|---|---|
| Tile | `<prefix>/<algorithm>-256w/tile/<L>/<N>`, `application/octet-stream`; `L` is decimal `0..63` without leading zeros, and `N` uses the grouped encoding in [section 4](#4-storage-layout-and-publication). |
| Full tile | 256 hashes. Level 0 stores leaf hashes; each level-`L` entry for `L >= 1` is the Merkle Tree Hash of one complete level-`L-1` tile. |
| Entry bundle | `<prefix>/tile/entries/<N>`; 256 raw entries encoded as big-endian `uint16` length-prefixed values. See [section 5.3](#53-entry-bundles-optional). |
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
  and atomically replace the destination.
- On POSIX, sync each newly created directory link and the destination
  directory after rename. A higher-level retry rechecks the directory chain and
  destination even when the file is already visible.
- Wrong-size files are not durable full tiles and may be repaired. Higher-level
  writers enforce immutability; the low-level primitive permits replacement for
  repair or idempotent concurrent publication and trusts callers not to change
  valid content.

## 5. Architecture and API

```
            append(leaf hash)            flush()
   caller ───────────────▶  TiledTreeT ───────────────▶  TileStoreT
                              │ owns Tree                    ▲
              inclusion / consistency proofs                │ tiles
                              ▼                              │
                       ProofEngine ──▶ HashSource ◀──────────┘
                                        ├─ MemoryHashSource
                                        ├─ TileHashSource
                                        └─ CombinedHashSource
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

The planned `TileHashSource` owns the proof-read LRU cache; `TileStoreT` does
not cache. A combined source may require one read-only, non-hashing core
`subtree_root` accessor.

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
// Equivalent SHA-384 and SHA-512 aliases.

}
```

Aliases derive hash size and function from the core tree. Roll-up and proof code
combine two `HASH_SIZE`-byte hashes with that same function, so the default
single-block `sha256_compress` remains sufficient and adds no OpenSSL
dependency.

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

### 5.3 Entry bundles (optional)

Entry bundles are application-owned because merklecpp receives precomputed leaf
hashes, not raw entries. Each full bundle stores 256 big-endian `uint16`
length-prefixed entries at `<algorithm>-256w/tile/entries/<N>`. The application
defines the leaf derivation (for example, `leaf_hash = H(entry)`); merklecpp
stores the supplied leaf hash unchanged.

## 6. Delivery plan

This PR delivers phases 0 and 1 plus the standalone entry-bundle codec and
storage primitives. Later PRs deliver the remaining independently testable
phases; phases 1-3 need no further core changes, while phase 4 may add one
non-hashing accessor.

| Phase | Scope | Key tests |
|---|---|---|
| 0. Scaffolding | Headers, PAL, namespace, geometry, aliases, and CMake test wiring | Public-header and build integration |
| 1. Coordinates/store | `TileRef`, index/path encoding, `TileStoreT`, durable atomic I/O, entry-bundle primitives | Encoding vectors; algorithm roots; 256-hash SHA-256/384 tiles; round trips; file/symlink collisions |
| 2. Writers | Incremental `TileWriterT::write_up_to` from `leaf_at`, roll-ups, and `EntryBundleWriterT`; full resources only | Sizes 256 and 70,000 produce the exact tile set; repeated writes preserve immutability |
| 3. Proof engine | `TileHashSource`, `mth_range`, roots, inclusion/consistency proofs, and verification | Tile roots equal tree roots; inclusion equals `path()` / `past_path()` and verifies; consistency reconciles `past_root()` values |
| 4. Combined tree | Optional `subtree_root`; memory/combined sources; `TiledTreeT` append, flush, proof, and compaction APIs | Prove flushed and resident leaves against a non-flushed reference; consistency across a flush boundary |
| 5. Documentation/performance | README usage, design link, and tile-backed benchmarks | Documentation and benchmark coverage |

Deliverables are `merklecpp_tiles.h`, `merklecpp_pal.h`, `test/tiles_*.cpp`,
CMake wiring, the optional core accessor, and README/design updates.
