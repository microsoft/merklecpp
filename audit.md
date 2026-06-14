# Audit: tiled storage & tile-backed proofs (`experiment/tiles`)

**Scope.** All changes on `experiment/tiles` relative to `main` (15 commits,
+3,900 / −2 lines): the new header `merklecpp_tiles.h`, the `TreeT::subtree_root`
addition in `merklecpp.h`, the `test/tiles_*.cpp` / `test/time_tiles.cpp` suite,
and `doc/design/tlog-tiles.md` + `doc/tiles-guide.md`.

**Method.** Four independent review passes: a manual read plus three specialist
reviewers. Two reviewers built and ran the tile suite under GCC **and** Clang
with **ASan + UBSan**, and one cross-checked **113,462** generated proofs against
the `merkle::TreeT` oracle across every tile boundary. Where a finding is marked
*reproduced*, it was triggered in a running build, not merely read.

**Headline.** The core proof math is **sound**: inclusion proofs are byte-identical
to `TreeT::path()/past_path()`, consistency proofs round-trip through
`verify_consistency`, and both were checked exhaustively for small trees and by a
large oracle sweep. The shift in `subtree_root` that looks like 64-bit UB is in
fact **safe**. No correctness defect exists on the **default** code path. The
issues below are (a) a real **durability** gap whose docs overstate the guarantee,
(b) a **non-default config** (`write_higher_levels = false`) that crashes proofs,
and (c) several **robustness / hostile-input / test / doc** gaps.

> **Update (follow-up commit).** Acting on this audit: the `write_higher_levels`
> option has been **removed** entirely — higher-level roll-up tiles are now
> always written — which makes **C1 unreachable** (no way to disable the tiles
> `resolve` relies on) and **T3 obsolete** (no flag to test). The test gaps
> **T1, T2, T4, T5, T6** are now **implemented**: corrupt/truncated-file reads
> (`tiles_store`), level-2 tiles end-to-end via a 256³-leaf test (`tiles_level2`)
> plus level-2 `resolve` descend coverage in the `tiles_proofs` oracle sweep
> (sizes 65536/65537/300000), writer reopen/resume (`tiles_writer` §E),
> non-zero `retention_margin` compaction (`tiles_tree` Part 2c), and the empty
> tree (`tiles_tree` Part 0). Still open for a future change: **C2/D2** (fsync
> durability), **C3/C4/C5** (robustness hardening), **P1/D1** (tile cache).

---

## Findings summary

| # | Severity | Area | Summary |
|---|----------|------|---------|
| C1 | Medium | Correctness (config) | `resolve` reads a higher-level tile by arithmetic, not existence → `write_higher_levels=false` throws `cannot open file` during proofs |
| C2 | Medium | Durability | `write_file_atomically` never `fsync`s → docs' crash-safety claim is false; a post-crash truncated tile permanently wedges the store |
| C3 | Low | Robustness (public API) | `TreeT::subtree_root` does not validate `level`/`index` → shift-UB at `level≥64`; silent wrong `true` on `lo+count` overflow |
| C4 | Low | Robustness (hostile input) | `largest_pow2_lt` infinite-loops for `size > 2^63`, hanging the public proof entry points |
| C5 | Low | Concurrency / cleanup | Fixed `*.tmp` name: concurrent-writer collision and temp-file leak on error |
| P1 | Low–Med | Performance | No tile read cache: `resolve`/roll-ups re-open & re-deserialize the same 8 KB tiles repeatedly (and the docs claim a cache that does not exist) |
| T1 | Medium | Tests | No corrupt/truncated-file read tests |
| T2 | Medium | Tests | No coverage of level-≥2 tiles (needs > 65,536 leaves) |
| T3 | Medium | Tests | No test with `write_higher_levels=false` (would have caught C1) |
| T4 | Low–Med | Tests | No reopen/resume test (the `full_prefix_length` cursor-resume path) |
| T5 | Low | Tests | No `compact()` with non-zero `retention_margin`; no retract-below-flush-after-margin |
| T6 | Low | Tests | Boundary sizes `0` and `65537` untested |
| D1 | Medium | Docs | Design doc describes an "in-process LRU cache" that does not exist |
| D2 | Low | Docs | Durability wording ("a crash never leaves a half-written tile") is false (see C2) |
| D3 | Low | Docs | Partial-tile / spec material retained as background; correct but worth a sharper "not implemented" caveat |

---

## Correctness & robustness

### C1 — `TileHashSourceT::resolve` assumes higher-level tiles exist (Medium)

`merklecpp_tiles.h:636–642`.

```cpp
const uint64_t full_tiles = full_shift >= 64 ? 0 : (available_size >> full_shift);
if (n < full_tiles)
{
  const std::vector<Hash> tile = store.read_tile(TileRef{L, n}); // unchecked read
  ...
}
```

`resolve` infers the presence of a level-`L` (`L ≥ 1`) tile purely from
`available_size`. That inference is valid only with the default
`write_higher_levels = true` (`merklecpp_tiles.h:373`). The option is public and
reachable through `TiledTreeT::Config::writer.write_higher_levels`, and the design
doc (`§6.4`) does not state it is incompatible with proof generation. With it set
to `false`, only level-0 tiles are written, but `resolve` still believes level-≥1
tiles exist and calls `read_tile`, which **throws** rather than returning `false`,
so the `||` fallback in `CombinedHashSourceT::subtree_root` never engages.

*Reproduced:* 70,000-leaf tree, `write_higher_levels=false`,
`TileHashSource(store, 69888)`, `engine.root(69888)` →
`cannot open file: .../tile/1/000`. Triggers for any subtree of height ≥ 9 once
the tiled prefix reaches 65,536 leaves.

*Default path is safe:* `available_size >> (8·(L+1))` was shown (algebraically and
by the oracle sweep) to equal exactly the number of level-`L` tiles on disk when
higher levels are written, so the default never reads a missing tile.

**Fix.** Gate the read on real presence and fall through to the existing split
path, which already reconstructs the same root from level-0 tiles:
```cpp
if (n < full_tiles && store.has_full_tile(L, n)) { ... }
```
This makes proofs correct (just slower) regardless of the option. Alternatively,
remove/guard the option or document the incompatibility.

### C2 — `write_file_atomically` is atomic but not durable; the store can wedge after a crash (Medium)

`merklecpp_tiles.h:269–302` (`flush()` + `rename`, **no `fsync`** anywhere in the
file).

`std::ofstream::flush()` only moves bytes into the OS page cache. With no `fsync`
of the temp file before `rename` and no `fsync` of the parent directory after,
a power-loss can persist the rename's directory entry while the file's data blocks
are lost, leaving a tile **present at its final path but zero-length / truncated**.
(ext4's flush-on-rename heuristic does not apply to write-once renames over a
non-existent target and is not portable to XFS/btrfs/NFS.)

This contradicts the comment at `merklecpp_tiles.h:268` and
`doc/design/tlog-tiles.md:355–356`.

*Why it bites (reproduced):* the store uses **file existence** as its idempotence
signal — `flush()` skips any tile where `has_full_tile()` is true
(`merklecpp_tiles.h:422`) and `full_prefix_length()` seeds its cursor from
existence (`:476–499`). A present-but-truncated tile is therefore treated as
durably written and **never rewritten**, yet `read_tile`'s exact-size check rejects
it on every read — so every later roll-up or proof over that tile throws
indefinitely until the file is deleted by hand. (Integrity is preserved — no wrong
proof is ever produced — but availability/self-healing is not.)

**Fix.** `fsync` the temp fd before `rename`, then `fsync` the parent directory
(`FlushFileBuffers` on Windows). And/or treat a wrong-size tile as **absent** on
read so it is rewritten (self-heal). Soften the docs to an atomicity-only claim
(see D2).

### C3 — `TreeT::subtree_root` does not validate its arguments (Low)

`merklecpp.h:1333–1336`.

```cpp
const size_t lo    = index << level;        // line 1333
const size_t count = (size_t)1 << level;    // line 1334  -- run BEFORE any guard
if (num_leaves() == 0 || lo < min_index() || lo + count > num_leaves()) ...
```

`subtree_root` is a public method (used by `MemoryHashSourceT`). It shifts by the
caller-supplied `uint8_t level` *before* any bound check:

* **`level ≥ 64` → UB.** `index << level` / `1 << level` are shifts past the type
  width. *Reproduced under UBSan:* `subtree_root(64, 0, out)` →
  `shift exponent 64 is too large`.
* **`lo + count` overflow → silent wrong `true`.** With `level=1` and
  `index ≈ 2^63`, `lo + count` wraps to `0`, the upper-bound guard passes, and the
  call returns `true` with `out` set to an unrelated node hash (ASan-clean —
  memory-safe, but a wrong positive).

No in-repo caller hits either case (internal `level = log2_exact(w) ≤ 63` or `0`);
this is strictly out-of-contract input to a public API.

> Related robustness note (no behavioural bug today): unlike the canonical
> `walk_to`, which guards its identical shift with `if (_root->height > 1)`
> (`merklecpp.h:1068`), `subtree_root`'s shift at `:1356` is left unguarded and is
> only safe because the `level==0` early-return plus the `_root->height ≥
> target_height ≥ 2` guard keep the exponent in `[1,63]`. Verified safe under
> UBSan, but it is a fragile invariant; an explicit guard/assert would harden it.

**Fix.** First lines of the method:
```cpp
if (level >= sizeof(size_t) * 8) return false;
if (num_leaves() == 0 || index > (num_leaves() >> level)) return false; // overflow-safe
```

### C4 — `largest_pow2_lt` hangs for `size > 2^63` (Low)

`merklecpp_tiles.h:846–854`.

```cpp
uint64_t k = 1;
while ((k << 1) < n) k <<= 1;   // k reaches 2^63, k<<1 overflows to 0, loop never ends
```

Reachable from the public `root(size)`, `inclusion_proof(index, size)` and
`consistency_proof(m, n)` (via `mth_range`, *before* any leaf resolution), so an
oversized `size` argument hangs the call. *Reproduced:* `n = 2^63+1`, `2^63+5`,
`2^64−1` never terminate. Not reachable with a real tree (>9.2×10¹⁸ leaves); a
hostile/buggy size only.

**Fix.** `while (k <= (n - 1) / 2) k <<= 1;` (or stop at `k == (1ull << 63)`).

### C5 — fixed `*.tmp` name: concurrent collision + leak on error (Low)

`merklecpp_tiles.h:282` (`tmp += ".tmp"`).

A single fixed temp name per target means two writers persisting the *same* tile
race on the same `*.tmp`; the second `rename` can fail with `ENOENT` and throw, and
there is a window where one writer renames the other's partially-written temp into
place (corruption-safe only because tile content is deterministic). On a write or
`rename` error the `*.tmp` is also left behind.

**Fix.** Unique temp suffix (pid + counter/random) and `remove` it on the error
path.

---

## Performance

### P1 — no tile read cache (Low–Medium)

There is no caching anywhere in `merklecpp_tiles.h` (confirmed). `resolve` and the
writer's `collect` roll-ups call `store.read_tile`, which `open`s, reads and
deserializes a full 256-hash (8 KB at HASH_SIZE=32) tile **every** call. Near the
tiled/frontier boundary `resolve` descends and re-reads level-0 tiles, and across
many proofs the same hot tiles are re-read from scratch. The design doc assumes
this is mitigated by a cache that does not exist (see D1).

**Fix.** A small LRU (or per-proof memo) keyed by `{level, index}` over decoded
tiles; or document that callers should wrap `TileStore` with their own cache.

---

## Test coverage gaps

Empirical validation is otherwise strong: `tiles_proofs.cpp` checks generated
proofs against `TreeT::path/past_path/past_root`, round-trips consistency proofs
through `verify_consistency`, exercises tamper/wrong-root rejection, and is
**exhaustive** over `(m,k)` consistency pairs for `n ≤ 16`. The gaps:

* **T1 (Medium)** — No corrupt/truncated tile or entry-bundle read tests. The code
  *is* defensive (`read_tile` exact-size check; `decode_entries` bounds checks,
  fuzzed clean under ASan), but the error paths are unverified by the suite. Only
  wrong-width *writes* are tested (`tiles_store.cpp`).
* **T2 (Medium)** — No level-≥2 tiles. The largest tree is 70,000 leaves
  (`tiles_proofs.cpp`, `tiles_writer.cpp`), which reaches level-1 only; level-2
  needs > 256² = 65,536 (in practice 256³ ≈ 16.7 M for a full level-2 tile).
* **T3 (Medium)** — No `write_higher_levels=false` test; such a test would have
  caught C1.
* **T4 (Low–Med)** — No reopen/resume test: every writer runs once against a fresh
  temp dir, so the `full_prefix_length` binary-search resume path is unexercised.
* **T5 (Low)** — `compact()` is only tested with `retention_margin = 0`; no
  retract-below-flush case after compaction with a non-zero margin.
* **T6 (Low)** — Boundary sizes `0` and `65537` untested (`1/256/257/65535/65536`
  are covered).

---

## Documentation

* **D1 (Medium)** — `doc/design/tlog-tiles.md:359–360` ("An in-process LRU cache of
  recently read tiles avoids repeated I/O") and `:593` ("served from cache")
  describe a component that does not exist (see P1). Either implement it or remove
  the claim.
* **D2 (Low)** — `doc/design/tlog-tiles.md:355–356` and the `:268` code comment
  claim crashes never leave a half-written tile; false without `fsync` (see C2).
  Reword to "atomic (no torn writes); durability requires `fsync`".
* **D3 (Low)** — Partial-tile spec material remains as background (`§2.1`, `§4`).
  It is consistently caveated as the upstream format and "not produced or read
  here", so this is acceptable; a one-line "the implementation has no notion of a
  partial tile" near each occurrence would remove any doubt.

---

## Examined and found correct (no action)

Recorded so the breadth of the review is auditable:

* **Shift safety** — every shift in `merklecpp_tiles.h` uses 64-bit operands or is
  guarded; the `subtree_root` `:1356` shift is provably in `[1,63]` (UBSan-clean).
  The only shift defect is C4.
* **Recursion / termination** — `resolve` decrements `level` to a `≤ 8` base
  (depth ≤ ~55); `mth_range`/`subproof` strictly shrink their range;
  `inclusion_proof` is iterative. No stack risk at 2⁴⁰ leaves.
* **Inclusion proofs** — sibling ranges, `PATH_LEFT/RIGHT` directions, leaf→root
  ordering, and `max_index = size − 1` match `PathT::verify`; byte-identical to
  `TreeT::path/past_path` across exhaustive index sweeps over the 256/512/768/
  1024/2048 boundaries.
* **Consistency proofs** — `subproof` matches RFC 6962 SUBPROOF (pow2 and non-pow2
  `m`); `verify_consistency` matches the standard CT verifier (`m==n`, `m==0`,
  `m>n`, `is_pow2(m)` prepend, the `fn/sn` reduction, final `sn==0`). Round-trips
  for all `m<n≤300` plus boundary pairs.
* **`TileWriterT::write_up_to`** — bottom-up level order means roll-ups always read
  already-written child tiles; the `next_full` cursor + `has_full_tile` skip avoid
  duplicate writes and never skip a tile; atomic temp+rename keeps the on-disk
  prefix contiguous, so the monotonic `full_prefix_length` binary search is valid.
* **`compact` / `retract_to`** — `target` is clamped to a `TILE_WIDTH` multiple
  ≤ `tiles_size`, so the frontier is always retained and `min_index ≤ tiles_size`
  (no coverage gap); `target ≥ n` keeps ≥ 1 resident leaf; the `index + 1 <
  tiles_size` guard protects the immutable prefix while allowing rollback to
  exactly `flushed_size`.
* **`TileStoreT` I/O** — `decode_entries`/`read_tile` bounds checks are correct and
  overflow-free (fuzzed clean under ASan/UBSan); paths are built only from integer
  indices + the caller's `prefix`, so path traversal is not realistic;
  `encode_tile_index` cannot overflow its buffer.

## Out of scope / pre-existing (not introduced by this change)

* `Node::is_full()` computes `(1 << height)` with `1` as `int` (`merklecpp.h:690`),
  which is UB for `height ≥ 31` (> 2³⁰ leaves). `subtree_root` calls `is_full()`,
  but the same expression is already on the core insertion path, so this change
  neither introduces nor newly reaches it. Flagged for awareness only.

---

## Recommended priority

1. **C2 + D2** — add `fsync` (or self-heal wrong-size tiles) and correct the
   durability docs. Highest real-world risk.
2. **C1 (+ T3)** — make `resolve` gate on `has_full_tile`; add a
   `write_higher_levels=false` proof test.
3. **C3, C4** — validate inputs to the public `subtree_root` and bound
   `largest_pow2_lt`. Cheap hardening of public entry points.
4. **D1 / P1** — implement a tile cache or drop the doc claim.
5. **T1, T2, T4** — add corruption, level-≥2, and reopen/resume tests.
6. **C5, T5, T6, D3** — opportunistic cleanup.
