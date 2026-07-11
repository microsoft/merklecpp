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
> tree (`tiles_tree` Part 0).
>
> **Update (hardening commit).** The remaining implementation findings are now
> addressed: **C2/D2** by synced unique-temp writes, parent-directory sync on
> POSIX, and size-validated tile presence; **C3/C4** by overflow-safe public
> arithmetic guards; **C5** by unique temp names and cleanup; and **P1/D1** by a
> per-source tile read cache plus corrected docs.

---

## Findings summary

| # | Severity | Area | Summary |
|---|----------|------|---------|
| C1 | Medium | Correctness (config) | **Fixed:** the `write_higher_levels` option was removed, so required roll-up tiles are always written |
| C2 | Medium | Durability | **Fixed:** `write_file_atomically` syncs unique temp files, atomically replaces, syncs POSIX parent dirs, and bad-size tiles are rewritten |
| C3 | Low | Robustness (public API) | **Fixed:** `TreeT::subtree_root` validates `level`/`index` before shifting and uses overflow-safe range checks |
| C4 | Low | Robustness (hostile input) | **Fixed:** `largest_pow2_lt` uses an overflow-safe loop condition for `size > 2^63` |
| C5 | Low | Concurrency / cleanup | **Fixed:** temp file names are unique per process/time/counter and cleaned up on error |
| P1 | Low–Med | Performance | **Fixed:** `TileHashSource` keeps a small decoded tile cache for proof generation |
| T1 | Medium | Tests | **Fixed:** corrupt/truncated tile and entry-bundle read tests added |
| T2 | Medium | Tests | **Fixed:** level-2 coverage added via `tiles_level2` and large proof sweeps |
| T3 | Medium | Tests | **Obsolete:** the `write_higher_levels` flag was removed with C1 |
| T4 | Low–Med | Tests | **Fixed:** reopen/resume coverage added for `full_prefix_length` cursor recovery |
| T5 | Low | Tests | **Fixed:** non-zero `retention_margin` compaction and guarded rollback are covered |
| T6 | Low | Tests | **Fixed:** empty tree and `65537` boundary coverage added |
| D1 | Medium | Docs | **Fixed:** design doc now matches the per-source tile cache |
| D2 | Low | Docs | **Fixed:** durability wording now describes synced temp-file atomic replace and bad-size tile rewrite |

---

## Correctness & robustness

### C1 — `TileHashSourceT::resolve` assumes higher-level tiles exist (Medium)

`merklecpp_tiles.h:636–642`.

> **Status:** Fixed by removing the public `write_higher_levels` option. Higher
> level roll-up tiles are now always written, so the arithmetic existence
> invariant used by `resolve` is maintained.

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

> **Status:** Fixed by the hardening commit. `write_file_atomically` now writes a
> unique temp file, syncs it, publishes it with atomic replace, syncs the POSIX
> parent directory, and removes the temp file on error. `has_full_tile` also
> requires the exact full-tile byte size, so a truncated/oversized tile is
> rewritten instead of treated as durable.

The original implementation used stream flush plus rename and treated file
existence as the idempotence signal. A crash could therefore leave a present but
wrong-size tile that future writes skipped and future reads rejected. The current
implementation syncs the temp file before publishing, syncs POSIX parent
directories after publishing, and makes `has_full_tile` require the exact full
tile size so wrong-size tiles are rewritten.

### C3 — `TreeT::subtree_root` does not validate its arguments (Low)

> **Status:** Fixed by the hardening commit. The method now rejects unsupported
> levels and overflowing indices before shifting, and uses an overflow-safe
> `lo/count/leaves` containment check.

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

> **Status:** Fixed by the hardening commit. The loop condition is now based on
> `(n - 1) / 2`, so it cannot shift past `2^63` while searching for the largest
> power of two below `n`.

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

> **Status:** Fixed by the hardening commit. Temp names now include process,
> timestamp, and an atomic counter, and a guard removes the temp path unless the
> atomic replace succeeds.

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

> **Status:** Fixed for proof generation by the hardening commit. `TileHashSource`
> now keeps a small decoded tile cache and serves repeated reads from memory.

The original implementation opened, read, and deserialized a full 256-hash tile
for every `resolve` call, and the design doc described a cache that had not yet
been implemented. `TileHashSource` now keeps a small decoded tile cache keyed by
`{level, index}` and reuses hot tiles during proof generation.

---

## Test coverage gaps

Empirical validation is otherwise strong: `tiles_proofs.cpp` checks generated
proofs against `TreeT::path/past_path/past_root`, round-trips consistency proofs
through `verify_consistency`, exercises tamper/wrong-root rejection, and is
**exhaustive** over `(m,k)` consistency pairs for `n ≤ 16`. The gaps:

The historical gaps listed in the first audit pass are now covered: corrupt and
wrong-size tile/bundle files, level-2 tiles, reopen/resume, non-zero retention
margin, guarded rollback below the flushed prefix, empty trees, and `65537` /
large-boundary proof cases. The `write_higher_levels=false` test became obsolete
when that option was removed.

---

## Documentation

* **D1 (Medium)** — Fixed: `doc/design/tlog-tiles.md` now describes the
  implemented per-source tile cache.
* **D2 (Low)** — Fixed: `doc/design/tlog-tiles.md` and the code comment now
  describe synced unique-temp writes, atomic replace, and bad-size tile rewrite.

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
* **`compact` / `retract_to`** - `target` is normally aligned to a `TILE_WIDTH`
  multiple and, for a nonzero `tiles_size`, is capped below it, so the frontier
  and final tiled leaf remain resident. The `index + 1 < sealed_size` guard
  protects the immutable prefix while allowing rollback to exactly
  `immutable_size`.
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

All implementation, test, and documentation findings from this audit have now
been addressed.
