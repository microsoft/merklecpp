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
- [Quick start: `TiledTree`](#quick-start-tiledtree)
- [Flushing and compaction](#flushing-and-compaction)
- [Rollback](#rollback)
- [Proofs](#proofs)
- [Lower-level building blocks](#lower-level-building-blocks)
- [Entry bundles (optional)](#entry-bundles-optional)
- [On-disk layout](#on-disk-layout)

## Requirements and a note on hashing

- C++17 (the header uses `<filesystem>` and `<fstream>`).
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

`flush()` is incremental: each call writes only the full tiles that became
complete since the previous call. Full tiles are immutable — written once and
never rewritten — and the incomplete frontier is never tiled (it stays in
memory until it grows into a full tile).

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
  only the leaves already covered by a **durably written full tile**, keeping
  `retention_margin` recent leaves resident. It returns the new minimum
  (smallest still-resident) leaf index.
- Proofs for dropped leaves are still produced — they are served from the tiles
  and transparently combined with the resident frontier.

```cpp
log.compact();                      // free memory now
uint64_t resident_from = log.tree_ref().min_index();
```

## Rollback

Tiles are immutable, so you may only roll back entries that have **not yet been
committed to tiles** (i.e. appended since the last flush). `retract_to`
enforces this:

```cpp
log.retract_to(index);   // keep leaves [0, index], drop the rest
```

- Allowed when the resulting size is `>= flushed_size()` — i.e. only the
  un-tiled frontier is removed. `flushed_size()` is the full-tile-covered prefix
  (a multiple of 256), so everything appended since the last full tile can be
  rolled back.
- Throws otherwise (rolling back committed entries would leave stale tiles).
- `retract_to` mirrors `merkle::Tree::retract_to`: `index` is the new *last*
  leaf, so the resulting size is `index + 1`.

> ⚠️ Retracting the underlying tree directly via `log.tree_ref().retract_to(...)`
> bypasses this guard and can leave the tiles inconsistent with the tree. Use
> `TiledTree::retract_to` instead.

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

// Write all newly-complete full tiles (the incomplete frontier is not tiled).
auto stats = writer.write_up_to(
  tree.num_leaves(),
  [&](uint64_t i) -> const merkle::Hash& { return tree.leaf(i); });
// stats.full_written
```

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
into a `ProofEngine` for you.

## Entry bundles (optional)

If you also want to store the raw log entries (tlog-tiles "entry bundles"), use
`EntryBundleWriter`. Bundles are level-0 only and application-owned — merklecpp
stores leaf hashes; you supply the raw bytes and decide how an entry maps to its
leaf hash.

```cpp
merkle::tiles::EntryBundleWriter bundles(store);
bundles.write_up_to(num_entries,
  [&](uint64_t i) -> std::vector<uint8_t> { return raw_entry_bytes(i); });

// Read them back (full bundle = 256 entries; pass the width for a partial one).
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
all but the last prefixed by `x` (e.g. index `1234067` → `x001/x234/067`). All
tiles are full (256-wide) and immutable; the incomplete frontier is never tiled.
See
[`design/tlog-tiles.md`](design/tlog-tiles.md) for the full specification of the
geometry and proof algorithms.
