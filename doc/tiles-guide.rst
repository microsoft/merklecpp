.. _tiles-guide:

Tiled storage and proofs — a guide to ``merklecpp_tiles.h``
===========================================================

``merklecpp_tiles.h`` is an optional, header-only companion to ``merklecpp.h``. It
lets you persist a Merkle tree as a set of immutable **tile files** on disk and
serve **inclusion** and **consistency** proofs from those tiles, from the
in-memory tree, or from a combination of the two — so proofs stay available even
after old entries are dropped from memory.

It builds on the `tlog-tiles <https://c2sp.org/tlog-tiles>`__ file/directory layout
but is **not** trying to be wire-compatible with external tlog-tiles clients.
merklecpp adds an algorithm-qualified directory above the standard ``tile/``
layout so different hash functions can safely share one configured prefix.
This page is a practical how-to; see the
:doc:`illustrated walkthrough <tiles-illustrated>` for a visual model of the
tile layout and proof algorithms.

.. contents:: On this page
   :local:
   :depth: 2

Requirements and a note on hashing
----------------------------------

- C++20. Both headers use ``std::format``; tile storage additionally uses
  ``<filesystem>`` and small platform-specific file-sync calls.

- Include the companion header; it pulls in ``merklecpp.h`` for you:

  .. code:: cpp

     #include <merklecpp_tiles.h>

- Everything lives in ``namespace merkle::tiles`` and is templated on the same
  ``<HASH_SIZE, HASH_FUNCTION>`` as your tree. The default aliases
  (``merkle::tiles::TiledTree``, ``TileStore``, ``TileWriter``, ``ProofEngine``, …) use
  the **same** SHA-256 as ``merkle::Tree``, so a tile-derived inclusion proof is
  byte-identical to one from ``merkle::Tree::path()`` and verifies with the usual
  ``merkle::Path::verify()``.

- With ``OPENSSL=ON``, corresponding ``TiledTree384``/``TiledTree512``,
  ``TileStore384``/``TileStore512``, writer, source, proof-engine, and entry-bundle
  aliases use the OpenSSL-backed SHA-384 and SHA-512 functions.

- A custom hash function must use the constructor overload that supplies a
  lowercase storage name, for example ``MyTiledTree(cfg, "custom-sha256")``.
  Otherwise construction throws because merklecpp cannot infer the namespace.
  Names may contain lowercase letters, digits, and internal hyphens. The
  reserved ``sha256``, ``sha384``, and ``sha512`` names must match the configured hash
  size.

- You insert **leaf hashes**, not raw entries — exactly like ``merkle::Tree``.
  Deriving a leaf hash from an entry (e.g. ``leaf = H(entry)``) is your
  application's job. The tile hash values are whatever your ``HASH_FUNCTION``
  produces; they are not RFC 6962 unless you instantiate your tree with an
  RFC 6962 hash function (not required, and not the goal here).

Thread safety
-------------

The tiled-storage API provides no internal synchronization. Treat each
``TileStore``, ``TileWriter``, ``TileHashSource``, ``ProofEngine``, ``TiledTree``, and
``EntryBundleWriter`` instance as single-threaded.

Serialize access to any one object, including methods declared ``const``:
tile-backed proof generation updates the ``TileHashSource`` LRU cache. Writers
sharing a prefix must also be serialized. Independent ``TileStore`` objects may
read that prefix concurrently with the single serialized writer because tiles
are published atomically and only appear at their final path when complete.

.. _tiled-tree-quick-start:

Quick start: ``TiledTree``
--------------------------

``TiledTree`` is the high-level wrapper: append leaf hashes, flush them to disk
(which writes tiles), and ask for proofs. This example is included directly
from the ``tiles_docs`` test so the documented code is compiled and run.

.. literalinclude:: ../test/tiles_docs.cpp
   :language: cpp
   :start-after: SNIPPET_START: TiledTree-Quick-Start
   :end-before: SNIPPET_END: TiledTree-Quick-Start
   :dedent: 2

This example assumes ``batch`` is non-empty. ``root()`` throws on an empty tree;
``size()``, ``flush()``, and ``compact()`` are safe at size 0.

``TiledTree`` can be move-constructed, but it cannot be copied or assigned. Move
construction keeps its writer bound to the destination tree's tile store.

``Config::prefix`` is resolved to an absolute path when the store is constructed.
A relative prefix therefore binds to the working directory at that moment;
later working-directory changes do not move the tile store.

``TiledTree`` always creates a new tiled tree. The configured prefix may already
exist, but the algorithm-qualified tile namespace must not: the default alias
atomically creates ``<prefix>/sha256-256w/tile`` and rejects it whenever it already
exists, even if it is empty. Construction does not adopt existing tiles because
those files do not identify the tree that produced them or contain enough state
to restore its size and root. If your application persists and validates that
state separately, use the lower-level ``TileStore`` and ``TileWriter`` APIs;
``TileWriter`` intentionally resumes existing full tiles and therefore trusts the
caller to supply the same tree and hash function. A fresh writer scans the
requested range in order, stopping at the first missing or malformed file, so
an interior hole is rewritten rather than hidden by later files.

``flush()`` is incremental: each call writes only the full tiles that became
complete since the previous call. Full tiles are immutable: written once after
all 256 entries are final and never rewritten. The remaining frontier stays in
memory until it crosses the next full-tile boundary.

The writer also keeps an opportunistic per-level FIFO of perfect roots computed
from the exact hashes passed to each successful durable tile write. A parent
roll-up consumes matching roots in order instead of reading and hashing those
child tiles again. Each active level retains at most one tile width of roots
(256 with the default geometry). This changes neither the tile format nor the
source of truth: a fresh or moved ``TiledTree``, a resumed ``TileWriter``, a
pre-existing tile, an index gap, an interrupted write, an evicted root, or any
other cache miss reads the immutable child tile and recomputes its root. The
tradeoff is one perfect-root calculation during each normal tile write in
exchange for avoiding the concentrated read-and-hash work at parent boundaries.
The FIFO adds no synchronization; the external locking requirements above still
apply.

Tile files are written through unique temporary files, synced, then published
with an atomic replace. On POSIX, file contents are synced, each newly created
directory is made durable by syncing its parent, and the destination directory
is synced after the rename. Before reusing a visible file, a writer also
re-confirms its directory chain and destination directory. On Windows, file
contents are flushed and the rename is write-through, but directory syncs are
no-ops and directory-entry durability is left to the filesystem. A wrong-size
file at a tile path is treated as unpublished and is rewritten when the source
leaves are still resident.

.. _flushing-and-compaction:

Flushing and compaction
-----------------------

By default ``flush()`` only *writes* tiles; it keeps every leaf resident in
memory. Dropping already-tiled leaves from memory ("compaction") is **opt-in**,
because once you drop them you can only prove them from the tiles.

.. code:: cpp

   merkle::tiles::TiledTree::Config cfg;
   cfg.prefix           = "/var/log/mylog";
   cfg.compact_on_flush = true;   // drop tiled leaves after each flush
   cfg.retention_margin = 4096;   // keep at least 4096 tiled leaves resident too

   merkle::tiles::TiledTree log(cfg);

- ``compact_on_flush`` (default ``false``): when set, ``flush()`` calls
  ``compact()`` for you.
- ``compact()`` can also be called explicitly at any time. It drops from memory
  only leaves already covered by a **successfully published full tile**.
  ``retention_margin`` is counted back from ``flushed_size()``, then rounded down to
  a tile boundary; the un-tiled frontier is always resident in addition to the
  margin, and alignment may retain up to ``TILE_WIDTH - 1`` extra tiled leaves.
  The final tiled leaf is also retained so rollback to exactly
  ``immutable_size()`` remains representable. Treat the margin as a minimum, not
  an exact resident count. ``compact()`` returns the new minimum
  (smallest still-resident) leaf index.
- Proofs for dropped leaves are still produced — they are served from the tiles
  and transparently combined with the resident frontier.

``flushed_size()`` is the boundary completed successfully at every required tile
level, and it is the only boundary used for proof reads and compaction.
``immutable_size()`` is the rollback boundary. A flush seals that boundary before
it starts writing, because an error can occur after a full tile becomes visible.
If a flush throws, ``immutable_size()`` may advance while ``flushed_size()`` does
not. Keep the same tree contents, correct the I/O failure, and retry ``flush()``;
finalized tiles are reused rather than rewritten.

Protect compacted tile files from external deletion or truncation. Once a
tile's leaves are no longer resident, ``flush()`` cannot regenerate a missing or
malformed copy and throws an error naming the first non-resident leaf. Restore
the tile from backup; retrying alone cannot recover it.

Performance exploration
-----------------------

``LONG_TESTS=ON`` builds ``time_tiles_continuous``. It runs 512 cycles of 256
appends (131,072 total) against ``TiledTree``, timing each append followed by one
``flush()`` and one ``compact()`` per cycle. A matching plain ``merkle::Tree``
control times the same appends and calls ``flush_to(max_index())`` every 256
leaves; it performs no tile flush or roll-up. Both roots are checked against the
same reference.

The same executable separately measures a level-0 durable write, reads of 256
child tiles, the 65,280 SHA-256 operations needed for their perfect roots, and a
level-1 durable write. Set ``TILE_PERF_OUTPUT_DIR`` to select the artifact
directory:

.. code:: console

   cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DLONG_TESTS=ON
   cmake --build build-release --target time_tiles_continuous
   TILE_PERF_OUTPUT_DIR="$PWD/tile-performance" \
     ./build-release/test/time_tiles_continuous
   python3 tools/summarize_tile_performance.py tile-performance \
     --viewer tools/tile-performance-viewer.html --markdown

The output directory contains per-event CSV, per-cycle CSV, and a JSON summary.
Open ``tools/tile-performance-viewer.html`` locally and choose the event CSV for
an interactive canvas plot with linear/log scales and data-driven roll-up
markers. Individual append observations subtract a median back-to-back
``steady_clock`` calibration recorded in the JSON; because append operations are
close to timer resolution, treat those values as comparative exploration rather
than standalone microbenchmarks. Generated CSV/JSON results are measurements,
not source, and must not be committed. Release CI runs the benchmark once,
validates and summarizes those files, then uploads them with the tracked viewer.

.. code:: cpp

   log.compact();                      // free memory now
   auto resident_from = log.tree_ref().min_index();

Rollback
--------

Tiles are immutable, so you may only roll back entries beyond the boundary
returned by ``immutable_size()``. ``retract_to`` enforces this:

.. code:: cpp

   log.retract_to(index);   // keep leaves [0, index], drop the rest

- Allowed when the resulting size is ``>= immutable_size()``.
- Throws otherwise, because a flush may already have published an immutable
  full tile for that range.
- The exact ``immutable_size()`` boundary remains available after compaction;
  compaction retains the final tiled leaf needed by the in-memory tree.
- After a successful flush, ``immutable_size() == flushed_size()``. After an
  interrupted flush, ``immutable_size()`` may be larger until the same tree state
  is flushed successfully.
- ``retract_to`` mirrors ``merkle::Tree::retract_to``: ``index`` is the new *last*
  leaf, so the resulting size is ``index + 1``.
- An ``index`` at or beyond the current last leaf is a no-op.

.. warning::
   Treat ``tree_ref()`` as an inspection escape hatch unless you also
   maintain every tiled-tree invariant yourself. Direct retraction bypasses the
   guard, can make ``flushed_size()`` and ``immutable_size()`` exceed ``size()``, and
   can make ``flushed_size()`` regress. Use ``TiledTree::retract_to`` instead.

``store_ref()`` is similarly unsafe for direct Merkle-tile mutation. A later
flush trusts any correctly sized tile written through it without checking that
the hashes match the in-memory tree. A mismatched tile can silently invalidate
proofs after compaction. Constructing an ``EntryBundleWriter`` over ``store_ref()``
is safe because entry bundles are separate application data and are never used
to resolve Merkle proofs.

Proofs
------

Both proof types come from ``TiledTree`` (or, at a lower level, from
``ProofEngine``). They are produced with your tree's hash function, so they match
what ``merkle::Tree`` would produce. Requests outside the current tree (e.g. a
size greater than ``size()``, or an out-of-range index) throw ``std::runtime_error``
rather than returning an incorrect proof.

Inclusion proofs
~~~~~~~~~~~~~~~~

.. code:: cpp

   // Prove leaf `index` in a tree of `size` leaves.
   std::shared_ptr<merkle::Path> p = log.inclusion_proof(index, size);
   bool ok = p->verify(root_at_size);

``size`` is the tree size you are proving against:

- ``size == log.size()`` ⇒ equivalent to ``merkle::Tree::path(index)``; verify
  against ``log.root()``.
- a past ``size`` ⇒ equivalent to ``merkle::Tree::past_path(index, size - 1)``;
  verify against the root at that size (e.g. a past root you are
  auditing).

``size`` may even exceed ``flushed_size()``: the recent, not-yet-tiled frontier is
taken from the resident tree while the older part comes from tiles.

Consistency proofs
~~~~~~~~~~~~~~~~~~

.. code:: cpp

   std::vector<merkle::Hash> proof = log.consistency_proof(m, n);   // m <= n
   bool ok = merkle::tiles::ProofEngine::verify_consistency(
     m, n, old_root /* root at size m */, new_root /* root at size n */, proof);

``verify_consistency`` is a static helper, so you can verify on a client that only
has the two roots and the proof.

The arguments are tree **sizes** (leaf counts). If you have leaf **indices**
instead, use the variant that maps index ``i`` to the tree of size ``i + 1`` (the
"last leaf" convention, matching ``past_path``/``retract_to``):

.. code:: cpp

   // Equivalent to consistency_proof(i + 1, j + 1).
   auto proof = log.consistency_proof_from_indices(i, j);   // i <= j

Both ``TiledTree`` and the lower-level ``ProofEngine`` provide
``consistency_proof_from_indices``.

Lower-level building blocks
---------------------------

If you manage your own tree/storage you can use the pieces directly instead of
``TiledTree``.

Index types and object lifetimes
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

``TiledTree`` mirrors ``merkle::Tree`` and uses ``size_t`` for indices and counts. The
lower storage and proof layer (``TileStore``, ``TileWriter``, hash sources,
``ProofEngine``, and ``EntryBundleWriter``) uses ``uint64_t``, matching tlog-tiles;
that is why leaf and entry callbacks below take ``uint64_t``. Conversions back to
the in-memory tree's index type are range-checked rather than truncated.

Low-level objects own none of the objects passed to them. A store must outlive
its writers and tile sources; a tree must outlive its memory source; sources
must outlive a ``CombinedHashSource`` and ``ProofEngine``. Returned proofs hold hash
copies and may safely outlive the engine that produced them.

Writing tiles from your own tree
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

.. code:: cpp

   merkle::Tree tree;
   for (auto& leaf : batch) tree.insert(leaf);

   merkle::tiles::TileStore  store("/var/log/mylog");
   merkle::tiles::TileWriter writer(store);

   // Write all newly-complete full tiles; keep the remaining frontier in memory.
   auto stats = writer.write_up_to(
     tree.num_leaves(),
     [&](uint64_t i) -> const merkle::Hash& { return tree.leaf(i); });
   // stats.full_written

``TileWriter`` keeps an in-memory next-file cursor. A new writer reconstructs it
by checking the contiguous prefix only up to the number of full files relevant
to the requested tree size. Existing files are re-confirmed as durably
published before reuse; malformed files and holes are rewritten.

Reading tiles and computing proofs
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

A ``HashSource`` resolves the root of a complete subtree; pick where it reads from:

- ``TileHashSource(store, available_size)`` — from full tile files; resolves the
  full-tile-covered prefix only (the frontier needs a memory source).
- ``MemoryHashSource(tree)`` — from a resident ``merkle::Tree``.
- ``CombinedHashSource(primary, secondary)`` — try ``primary`` first, then
  ``secondary`` (e.g. memory then tiles).

.. code:: cpp

   const uint64_t available = tree.num_leaves();
   const uint64_t tiled =
     available - (available % merkle::tiles::TILE_WIDTH);

   merkle::tiles::TileHashSource src(store, available);
   merkle::tiles::ProofEngine    engine(src);

   if (tiled > 0)
   {
     merkle::Hash root = engine.root(tiled);
     auto inclusion    = engine.inclusion_proof(/*index=*/0, tiled);
   }

   if (tiled > 1)
   {
     auto consistency = engine.consistency_proof(/*m=*/tiled / 2, tiled);
   }

A tile-only source can resolve proofs whose subtrees all lie within the
full-tile-covered prefix (``available_size`` is rounded down to a whole number of
tiles); requests beyond ``tiled`` throw. For the live frontier, combine it with a
``MemoryHashSource`` — which is exactly what ``TiledTree`` does for you.

``TiledTree`` simply wires a ``CombinedHashSource(MemoryHashSource, TileHashSource)``
into a ``ProofEngine`` for you. It creates these sources for each proof call, so
its tile cache is per-call and repeated tile-served proofs may re-read the same
files. A long-lived lower-level ``TileHashSource`` retains its 64-tile LRU cache
across calls; reuse it with a ``ProofEngine`` when that matters, and serialize
access.

Entry bundles (optional)
------------------------

If you also want to store the raw log entries (tlog-tiles "entry bundles"), use
``EntryBundleWriter``. Bundles are level-0 only and application-owned — merklecpp
stores leaf hashes; you supply the raw bytes and decide how an entry maps to its
leaf hash. Only full bundles (256 entries) are written; the incomplete tail
stays with your application until it completes a bundle.

.. code:: cpp

   merkle::tiles::EntryBundleWriter bundles(store);
   bundles.write_up_to(num_entries,
     [&](uint64_t i) -> std::vector<uint8_t> { return raw_entry_bytes(i); });

   // Read a full bundle back (256 entries).
   std::vector<std::vector<uint8_t>> e = store.read_entry_bundle(/*index=*/0);

Entries are encoded as big-endian ``uint16`` length-prefixed byte strings.
Each entry is therefore limited to 65,535 bytes; a larger entry throws. Bundles
live in the same algorithm-qualified store at ``tile/entries/<index>``, although
their bytes are application-owned rather than hash-dependent. Missing or
malformed bundles break the contiguous prefix and are rewritten by a new writer
when it resumes.

With ``TiledTree``, construct the writer from the exposed store:

.. code:: cpp

   merkle::tiles::EntryBundleWriter bundles(log.store_ref());

On-disk layout
--------------

Under the configured ``prefix``, merklecpp adds an algorithm-qualified format
directory above the standard tlog-tiles layout:

::

   <prefix>/
     sha256-256w/                # algorithm-qualified format directory
       tile/0/000, tile/0/001 … # level-0 tiles (leaf hashes), 256 hashes each
       tile/1/…                 # higher levels (roll-ups of full tiles below)
       tile/entries/…           # optional raw entry bundles

Tile indices use the tlog-tiles path encoding: zero-padded 3-digit groups with
all but the last prefixed by ``x`` (e.g. index ``1234067`` -> ``x001/x234/067``). Every
tile is full (256-wide), final, and immutable. Entries beyond the last full-tile
boundary remain in memory. The built-in SHA-384 and SHA-512 aliases use
``sha384-256w`` and ``sha512-256w``; a custom hash uses the explicit algorithm name
passed to its constructor. ``TileStore::root()`` returns this format directory,
not the configured prefix. Levels range from 0 through 63; every successful
flush writes all full roll-ups required by the current tree size. See the
`tlog-tiles specification <https://c2sp.org/tlog-tiles>`__ for the standard
geometry and the :doc:`illustrated walkthrough <tiles-illustrated>` for how
merklecpp stores and resolves those tiles.
