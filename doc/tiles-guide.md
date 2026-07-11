# Tiled storage and proofs — a guide to `merklecpp_tiles.h`

`merklecpp_tiles.h` is an optional, header-only companion to `merklecpp.h`. It
lets you persist a Merkle tree as a set of immutable **tile files** on disk and
serve **inclusion** and **consistency** proofs from those tiles, from the
in-memory tree, or from a combination of the two — so proofs stay available even
after old entries are dropped from memory.

It builds on the [tlog-tiles](https://c2sp.org/tlog-tiles) file/directory layout
but is **not** trying to be wire-compatible with external tlog-tiles clients. See
[`design/tlog-tiles.md`](design/tlog-tiles.md) for the design and internals; this
page is a practical how-to.

## Contents

- [Requirements and a note on hashing](#requirements-and-a-note-on-hashing)
- [Thread safety](#thread-safety)
- [Quick start: `TiledTree`](#quick-start-tiledtree)
- [Flushing and compaction](#flushing-and-compaction)
- [Rollback](#rollback)
- [Proofs](#proofs)
- [Lower-level building blocks](#lower-level-building-blocks)
- [Entry bundles (optional)](#entry-bundles-optional)
- [On-disk layout](#on-disk-layout)

## Requirements and a note on hashing

- C++17 (the header uses `<filesystem>` and small platform-specific file-sync
  calls for durable tile writes).
- Include the companion header; it pulls in `merklecpp.h` for you:

  ```cpp
  #include <merklecpp_tiles.h>
  ```

- Everything lives in `namespace merkle::tiles` and is templated on the same
  `<HASH_SIZE, HASH_FUNCTION>` as your tree. The default aliases
  (`merkle::tiles::TiledTree`, `TileStore`, `TileWriter`, `ProofEngine`, …) use
  the **same** SHA-256 as `merkle::Tree`, so a tile-derived inclusion proof is
  byte-identical to one from `merkle::Tree::path()` and verifies with the usual
  `merkle::Path::verify()`.
- You insert **leaf hashes**, not raw entries — exactly like `merkle::Tree`.
  Deriving a leaf hash from an entry (e.g. `leaf = H(entry)`) is your
  application's job. The tile hash values are whatever your `HASH_FUNCTION`
  produces; they are not RFC 6962 unless you instantiate your tree with an
  RFC 6962 hash function (not required, and not the goal here).

## Thread safety

The tiled-storage API provides no internal synchronization. Treat every
`TileStore`, `TileWriter`, `TileHashSource`, `ProofEngine`, `TiledTree`, and
`EntryBundleWriter` instance as single-threaded.

If an object or store prefix is shared between threads, the caller must
serialize every operation. This includes methods declared `const`: proof
generation updates the `TileHashSource` LRU cache. The library deliberately
does not add locks or otherwise coordinate concurrent readers and writers.

## Quick start: `TiledTree`

`TiledTree` is the high-level wrapper: append leaf hashes, flush them to disk
(which writes tiles), and ask for proofs.

```cpp
#include <merklecpp_tiles.h>

merkle::tiles::TiledTree::Config cfg;
cfg.prefix = "/var/log/mylog";   // directory for tile files

merkle::tiles::TiledTree log(cfg);

// Append leaf hashes (compute these from your entries however you like).
for (const merkle::Hash& leaf : batch)
  log.append(leaf);

// Persist newly-complete tiles to disk.
log.flush();

merkle::Hash root = log.root();          // current Merkle root
uint64_t      n    = log.size();         // number of leaves

// Inclusion proof for leaf 0 in the tree of `n` leaves.
auto inclusion = log.inclusion_proof(/*index=*/0, /*size=*/n);
assert(inclusion->verify(root));

// Consistency proof that size 100 is a prefix of size n.
auto consistency = log.consistency_proof(/*m=*/100, /*n=*/n);
```

`TiledTree` can be move-constructed, but it cannot be copied or assigned. Move
construction keeps its writer bound to the destination tree's tile store.

`TiledTree` always creates a new tiled tree. The configured directory may
already exist, but its `tile` subdirectory must be absent or empty. Construction
throws rather than adopting existing tiles because those files do not identify
the tree that produced them or contain enough state to restore its size and
root. If your application persists and validates that state separately, use
the lower-level `TileStore` and `TileWriter` APIs; `TileWriter` intentionally
resumes existing full tiles and therefore trusts the caller to supply the same
tree and hash function. A fresh writer scans the requested range in order,
stopping at the first missing or malformed file, so an interior hole is
rewritten rather than hidden by later files.

`flush()` is incremental: each call writes only the full tiles that became
complete since the previous call. Full tiles are immutable: written once after
all 256 entries are final and never rewritten. The remaining frontier stays in
memory until it crosses the next full-tile boundary.

Tile files are written through unique temporary files, synced, then published
with an atomic replace. On POSIX systems, each newly created directory is made
durable by syncing its parent, and the destination directory is synced after
the rename. Before reusing a visible file, a writer also re-confirms its
directory chain and destination directory. This makes a retry repeat a failed
directory sync even when the rename or directory creation is already visible.
A wrong-size file at a tile path is not a published tile and is rewritten.

## Flushing and compaction

By default `flush()` only *writes* tiles; it keeps every leaf resident in
memory. Dropping already-tiled leaves from memory ("compaction") is **opt-in**,
because once you drop them you can only prove them from the tiles.

```cpp
merkle::tiles::TiledTree::Config cfg;
cfg.prefix           = "/var/log/mylog";
cfg.compact_on_flush = true;   // drop tiled leaves after each flush
cfg.retention_margin = 4096;   // ...but keep the most recent 4096 resident

merkle::tiles::TiledTree log(cfg);
```

- `compact_on_flush` (default `false`): when set, `flush()` calls
  `compact()` for you.
- `compact()` can also be called explicitly at any time. It drops from memory
  only leaves already covered by a **durably written full tile**, keeping at
  least `retention_margin` recent leaves resident. It also retains the final
  tiled leaf so rollback to exactly `immutable_size()` remains representable.
  It returns the new minimum (smallest still-resident) leaf index.
- Proofs for dropped leaves are still produced — they are served from the tiles
  and transparently combined with the resident frontier.

`flushed_size()` is the boundary completed successfully at every required tile
level, and it is the only boundary used for proof reads and compaction.
`immutable_size()` is the rollback boundary. A flush seals that boundary before
it starts writing, because an error can occur after a full tile becomes visible.
If a flush throws, `immutable_size()` may advance while `flushed_size()` does
not. Keep the same tree contents, correct the I/O failure, and retry `flush()`;
finalized tiles are reused rather than rewritten.

```cpp
log.compact();                      // free memory now
uint64_t resident_from = log.tree_ref().min_index();
```

## Rollback

Tiles are immutable, so you may only roll back entries beyond the boundary
returned by `immutable_size()`. `retract_to` enforces this:

```cpp
log.retract_to(index);   // keep leaves [0, index], drop the rest
```

- Allowed when the resulting size is `>= immutable_size()`.
- Throws otherwise, because a flush may already have published an immutable
  full tile for that range.
- The exact `immutable_size()` boundary remains available after compaction;
  compaction retains the final tiled leaf needed by the in-memory tree.
- After a successful flush, `immutable_size() == flushed_size()`. After an
  interrupted flush, `immutable_size()` may be larger until the same tree state
  is flushed successfully.
- `retract_to` mirrors `merkle::Tree::retract_to`: `index` is the new *last*
  leaf, so the resulting size is `index + 1`.

> **Warning:** Treat `tree_ref()` as an inspection escape hatch unless you also
> maintain every tiled-tree invariant yourself. Direct retraction bypasses the
> guard, can make `flushed_size()` and `immutable_size()` exceed `size()`, and
> can make `flushed_size()` regress. Use `TiledTree::retract_to` instead.

`store_ref()` is similarly unsafe for mutation. A later flush trusts any
correctly sized tile written through it without checking that the hashes match
the in-memory tree. A mismatched tile can silently invalidate proofs after
compaction.

## Proofs

Both proof types come from `TiledTree` (or, at a lower level, from
`ProofEngine`). They are produced with your tree's hash function, so they match
what `merkle::Tree` would produce. Requests outside the current tree (e.g. a
size greater than `size()`, or an out-of-range index) throw `std::runtime_error`
rather than returning an incorrect proof.

### Inclusion proofs

```cpp
// Prove leaf `index` in a tree of `size` leaves.
std::shared_ptr<merkle::Path> p = log.inclusion_proof(index, size);
bool ok = p->verify(root_at_size);
```

`size` is the tree size you are proving against:

- `size == log.size()` ⇒ equivalent to `merkle::Tree::path(index)`; verify
  against `log.root()`.
- a past `size` ⇒ equivalent to `merkle::Tree::past_path(index, size - 1)`;
  verify against the root at that size (e.g. a past root you are
  auditing).

`size` may even exceed `flushed_size()`: the recent, not-yet-tiled frontier is
taken from the resident tree while the older part comes from tiles.

### Consistency proofs

```cpp
std::vector<merkle::Hash> proof = log.consistency_proof(m, n);   // m <= n
bool ok = merkle::tiles::ProofEngine::verify_consistency(
  m, n, old_root /* root at size m */, new_root /* root at size n */, proof);
```

`verify_consistency` is a static helper, so you can verify on a client that only
has the two roots and the proof.

The arguments are tree **sizes** (leaf counts). If you have leaf **indices**
instead, use the variant that maps index `i` to the tree of size `i + 1` (the
"last leaf" convention, matching `past_path`/`retract_to`):

```cpp
// Equivalent to consistency_proof(i + 1, j + 1).
auto proof = log.consistency_proof_from_indices(i, j);   // i <= j
```

Both `TiledTree` and the lower-level `ProofEngine` provide
`consistency_proof_from_indices`.

## Lower-level building blocks

If you manage your own tree/storage you can use the pieces directly instead of
`TiledTree`.

### Writing tiles from your own tree

```cpp
merkle::Tree tree;
for (auto& leaf : batch) tree.insert(leaf);

merkle::tiles::TileStore  store("/var/log/mylog");
merkle::tiles::TileWriter writer(store);

// Write all newly-complete full tiles; keep the remaining frontier in memory.
auto stats = writer.write_up_to(
  tree.num_leaves(),
  [&](uint64_t i) -> const merkle::Hash& { return tree.leaf(i); });
// stats.full_written
```

`TileWriter` keeps an in-memory next-file cursor. A new writer reconstructs it
by checking the contiguous prefix only up to the number of full files relevant
to the requested tree size. Existing files are re-confirmed as durably
published before reuse; malformed files and holes are rewritten.

### Reading tiles and computing proofs

A `HashSource` resolves the root of a complete subtree; pick where it reads from:

- `TileHashSource(store, available_size)` — from full tile files; resolves the
  full-tile-covered prefix only (the frontier needs a memory source).
- `MemoryHashSource(tree)` — from a resident `merkle::Tree`.
- `CombinedHashSource(primary, secondary)` — try `primary` first, then
  `secondary` (e.g. memory then tiles).

```cpp
merkle::tiles::TileHashSource src(store, /*available_size=*/tree.num_leaves());
merkle::tiles::ProofEngine    engine(src);

merkle::Hash root = engine.root(size);
auto inclusion    = engine.inclusion_proof(index, size);
auto consistency  = engine.consistency_proof(m, n);
```

A tile-only source can resolve proofs whose subtrees all lie within the
full-tile-covered prefix (`available_size` is rounded down to a whole number of
tiles). For the live frontier, combine it with a `MemoryHashSource` — which is
exactly what `TiledTree` does for you.

`TiledTree` simply wires a `CombinedHashSource(MemoryHashSource, TileHashSource)`
into a `ProofEngine` for you. It creates these sources for each proof call, so
its tile cache is per-call. A long-lived lower-level `TileHashSource` retains
its cache across calls.

## Entry bundles (optional)

If you also want to store the raw log entries (tlog-tiles "entry bundles"), use
`EntryBundleWriter`. Bundles are level-0 only and application-owned — merklecpp
stores leaf hashes; you supply the raw bytes and decide how an entry maps to its
leaf hash. Only full bundles (256 entries) are written; the incomplete tail
stays with your application until it completes a bundle.

```cpp
merkle::tiles::EntryBundleWriter bundles(store);
bundles.write_up_to(num_entries,
  [&](uint64_t i) -> std::vector<uint8_t> { return raw_entry_bytes(i); });

// Read a full bundle back (256 entries).
std::vector<std::vector<uint8_t>> e = store.read_entry_bundle(/*index=*/0);
```

Entries are encoded as big-endian `uint16` length-prefixed byte strings.

## On-disk layout

Under the configured `prefix`:

```
<prefix>/
  tile/0/000, tile/0/001 …   # level-0 tiles (leaf hashes), 256 hashes each
  tile/1/…                   # higher levels (roll-ups of full tiles below)
  tile/entries/…             # optional raw entry bundles
```

Tile indices use the tlog-tiles path encoding: zero-padded 3-digit groups with
all but the last prefixed by `x` (e.g. index `1234067` -> `x001/x234/067`). Every
tile is full (256-wide), final, and immutable. Entries beyond the last full-tile
boundary remain in memory. See
[`design/tlog-tiles.md`](design/tlog-tiles.md) for the full specification of the
geometry and proof algorithms.
