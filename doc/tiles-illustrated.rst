.. _tiles-illustrated:

Tiled Merkle trees: an illustrated walkthrough
==============================================

This page builds a visual model of how an append-only Merkle tree moves from
memory into immutable tile files, and how proofs continue to work across both
places.

For API usage and operational guidance, start with the
:doc:`practical tiled-storage guide <tiles-guide>`.

.. important::
   Every example on this page uses an 8-entry tile width and a tile height of 3,
   matching the `interactive proof atlas <proof-viz/>`_ and keeping the diagrams
   readable.

   The default merklecpp aliases remain ``TILE_WIDTH = 256`` and
   ``TILE_HEIGHT = 8``. Unless a section explicitly says "illustrative" or
   "atlas", use the 256-entry default.

.. contents:: On this page
   :local:
   :depth: 2

The scaled-down model
---------------------

A default production tile contains 256 entries and spans 8 binary tree levels
because ``256 = 2^8``. This page and the atlas scale that geometry down to 8
entries and 3 levels because ``8 = 2^3``.

+------------------------------+----------------+----------------------+
| Property                     | This page only | Production merklecpp |
+==============================+================+======================+
| Tile width                   | 8 entries      | 256 entries          |
+------------------------------+----------------+----------------------+
| Tree levels spanned by one   | 3              | 8                    |
| tile                         |                |                      |
+------------------------------+----------------+----------------------+
| Leaves covered by one full   | 8              | 256                  |
| level-0 tile                 |                |                      |
+------------------------------+----------------+----------------------+
| Leaves covered by one        | 8              | 256                  |
| level-1 entry                |                |                      |
+------------------------------+----------------+----------------------+
| Leaves covered by one full   | 64             | 65,536               |
| level-1 tile                 |                |                      |
+------------------------------+----------------+----------------------+

The scaling changes only the numbers in the drawings. The rules are the same:

1. Only full tiles are written.
2. A level-0 tile contains leaf hashes.
3. A higher-level tile contains roots of complete tiles from the level below.
4. The incomplete right-hand frontier remains in memory.
5. Published tiles are immutable.
6. Proofs can resolve subtree roots from memory, tiles, or both.

Notation
~~~~~~~~

- ``h7`` is the hash of leaf 7.
- ``R[a, b)`` is the Merkle root of the half-open leaf range ``[a, b)``.
- ``tile/L/NNN`` is tile index ``NNN`` at tile level ``L``, relative to the
  algorithm-qualified store directory.
- "Resident" means the in-memory tree can still expand that range to answer
  proof requests.
- "Compacted" means the in-memory tree retains enough summary hashes to keep
  its root correct, but no longer retains all detail below that range.

Colors used below
~~~~~~~~~~~~~~~~~

.. mermaid::

   flowchart TB
     T["Tile-backed hash or range"]:::tile
     M["Frontier hash or range<br/>(resident memory)"]:::memory
     B["Available from both tile and frontier"]:::both
     S["Compacted in-memory summary"]:::summary
     X["Leaf being proved"]:::target
     PT["Proof element<br/>green outline: tile source"]:::proofTile
     PM["Proof element<br/>blue outline: frontier source"]:::proofMemory
     PC["Proof element<br/>gray outline: computed reduction"]:::proofComputed

     T ~~~ M
     M ~~~ B
     B ~~~ S
     S ~~~ X
     X ~~~ PT
     PT ~~~ PM
     PM ~~~ PC

     classDef tile fill:#00843f,stroke:#00843f,color:#ffffff
     classDef memory fill:#276be9,stroke:#276be9,color:#ffffff
     classDef both fill:#276be9,stroke:#00843f,stroke-width:3px,color:#ffffff
     classDef summary fill:#9ca4af,stroke:#677384,color:#222832
     classDef target fill:#222832,stroke:#222832,stroke-width:3px,color:#ffffff
     classDef proofTile fill:#d72d47,stroke:#00843f,stroke-width:3px,color:#ffffff
     classDef proofMemory fill:#d72d47,stroke:#276be9,stroke-width:3px,color:#ffffff
     classDef proofComputed fill:#d72d47,stroke:#9ca4af,stroke-width:3px,color:#ffffff

What ``flush()`` and ``compact()`` each do
------------------------------------------

Appending, flushing, and compacting are separate operations:

.. mermaid::

   flowchart TB
     A["append(h)<br/>Add a leaf hash to the in-memory tree"]:::memory
     B["A complete 8-entry range now exists<br/>but no file is written automatically"]:::memory
     C["flush()<br/>Write every newly complete full tile"]:::tile
     D["The same range exists on disk and in memory<br/>(the default after flush)"]:::both
     E["compact()<br/>Optionally discard old resident detail"]:::summary
     F["Old detail is served from tiles;<br/>the incomplete frontier stays in memory"]:::both

     A --> B
     B --> C
     C --> D
     D --> E
     E --> F

     classDef tile fill:#00843f,stroke:#00843f,color:#ffffff
     classDef memory fill:#276be9,stroke:#276be9,color:#ffffff
     classDef both fill:#276be9,stroke:#00843f,stroke-width:3px,color:#ffffff
     classDef summary fill:#9ca4af,stroke:#677384,color:#222832

``flush()`` does not compact by default. Setting ``compact_on_flush = true`` makes
the final two steps happen in one call, but the durability rule is unchanged:
compaction happens only after all required tile writes succeed.

If a tile write fails, ``immutable_size()`` may advance past ``flushed_size()``
because a published tile cannot be rolled back. Keep the same tree contents and
retry the flush. See
:ref:`Flushing and compaction <flushing-and-compaction>` for the full
interrupted-write contract.

What is inside a tile file?
---------------------------

In the atlas model, ``tile/0/000`` is the concatenation of 8 leaf hashes:

.. mermaid::

   flowchart TB
     F["tile/0/000<br/>8 serialized hashes"]:::tile
     A["entries 0..3<br/>h0 ... h3"]:::tile
     B["entries 4..7<br/>h4 ... h7"]:::tile
     R["R[0, 8)<br/>reconstructed by hashing the entries"]:::computed
     N["Internal binary-tree nodes are reconstructed;<br/>they are not separately stored in the file"]:::note

     F -->|first bytes| A
     A -->|followed by| B
     B --> R
     R --> N

     classDef tile fill:#00843f,stroke:#00843f,color:#ffffff
     classDef computed fill:#9ca4af,stroke:#677384,color:#222832
     classDef note fill:#f3f4f5,stroke:#d1d5da,color:#222832

At level 1, each entry is already the root of 8 leaves:

.. mermaid::

   flowchart TB
     L1["tile/1/000<br/>8 serialized subtree roots"]:::tile
     L0A["entry 0<br/>root(tile/0/000) = R[0, 8)"]:::tile
     L0B["entry 1<br/>root(tile/0/001) = R[8, 16)"]:::tile
     L0C["entries 2..6<br/>..."]:::tile
     L0Z["entry 7<br/>root(tile/0/007) = R[56, 64)"]:::tile
     ROOT["R[0, 64)<br/>reconstructed from tile/1/000"]:::computed

     L1 --> L0A
     L0A --> L0B
     L0B --> L0C
     L0C --> L0Z
     L0Z --> ROOT

     classDef tile fill:#00843f,stroke:#00843f,color:#ffffff
     classDef computed fill:#9ca4af,stroke:#677384,color:#222832

The default production version of the second diagram needs 256 level-0 tile
roots, so its first full level-1 tile appears at 65,536 leaves rather than 64.

On-disk file layout
-------------------

After an illustrative 70-leaf tree is flushed, the full-tile boundary is 64:

.. code:: text

   prefix/
     sha256-8w/     # illustrative atlas geometry
       tile/
         0/
           000      # h0       ... h7
           001      # h8       ... h15
           ...
           007      # h56      ... h63
         1/
           000      # R[0,8), R[8,16), ... R[56,64)

Leaves ``[64, 70)`` do not appear in a tile file because they do not complete
another 8-entry tile. They remain in memory.

.. mermaid::

   flowchart TB
     N["n = 70 leaves"]:::computed
     C["covered = floor(70 / 8) * 8 = 64"]:::computed
     L0["8 full level-0 files<br/>tile/0/000 through tile/0/007"]:::tile
     L1["1 full level-1 file<br/>tile/1/000"]:::tile
     M["6-leaf frontier<br/>[64, 70) in memory"]:::memory

     N --> C
     C --> L0
     L0 --> L1
     L1 --> M

     classDef tile fill:#00843f,stroke:#00843f,color:#ffffff
     classDef memory fill:#276be9,stroke:#276be9,color:#ffffff
     classDef computed fill:#9ca4af,stroke:#677384,color:#222832

The optional ``sha256-8w/tile/entries/`` bundles are omitted here. They store
raw application entries, not Merkle tree nodes, and do not change proof
generation. The default aliases use ``sha256-256w`` instead.

Tree growth, one snapshot at a time
-----------------------------------

The next snapshots assume ``retention_margin = 0``. Where compaction is shown,
merklecpp still retains the final tiled leaf as a boundary leaf. This is why
the "both" range below is one leaf wide.

Snapshot A: 7 leaves
~~~~~~~~~~~~~~~~~~~~

No full 8-entry tile exists:

.. mermaid::

   flowchart TB
     N["n = 7"]:::computed
     C["full-tile boundary = 0"]:::computed
     M["Frontier only<br/>[0, 7)"]:::memory
     D["Disk<br/>no tile files"]:::empty

     N --> C
     C --> M
     M --> D

     classDef memory fill:#276be9,stroke:#276be9,color:#ffffff
     classDef computed fill:#9ca4af,stroke:#677384,color:#222832
     classDef empty fill:#f3f4f5,stroke:#d1d5da,color:#222832

Calling ``flush()`` at this point writes nothing. Every root and proof is served
from the in-memory tree.

Snapshot B: 11 leaves, before the first flush
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The first 8 leaves form a complete tile, but tile creation is explicit:

.. mermaid::

   flowchart TB
     N["n = 11"]:::computed
     M0["[0, 8)<br/>complete and eligible, still frontier only"]:::memory
     M1["[8, 11)<br/>incomplete frontier"]:::memory
     D["Disk<br/>still empty until flush()"]:::empty

     N --> M0
     M0 --> M1
     M1 --> D

     classDef memory fill:#276be9,stroke:#276be9,color:#ffffff
     classDef computed fill:#9ca4af,stroke:#677384,color:#222832
     classDef empty fill:#f3f4f5,stroke:#d1d5da,color:#222832

Snapshot B: 11 leaves, after ``flush()``
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The default ``flush()`` writes the full prefix but does not remove it from
memory:

.. mermaid::

   flowchart TB
     F["flush() succeeds<br/>flushed_size() = 8"]:::computed
     B["[0, 8)<br/>tile/0/000 + resident frontier"]:::both
     M["[8, 11)<br/>resident frontier only"]:::memory

     F --> B
     B --> M

     classDef memory fill:#276be9,stroke:#276be9,color:#ffffff
     classDef both fill:#276be9,stroke:#00843f,stroke-width:3px,color:#ffffff
     classDef computed fill:#9ca4af,stroke:#677384,color:#222832

At this point a proof may be answered entirely from memory even though a tile
copy exists.

Snapshot B: 11 leaves, after compaction
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

With zero retention, compaction drops old leaf detail while preserving leaf 7
as the rollback boundary:

.. mermaid::

   flowchart TB
     R["R[0, 11)<br/>current in-memory root"]:::computed
     P["R[0, 8)<br/>prefix represented by compacted summaries"]:::summary
     C["[0, 7)<br/>not leaf-addressable in memory"]:::summary
     B["h7<br/>retained boundary leaf"]:::both
     M["R[8, 11)<br/>fully resident frontier"]:::memory
     T["tile/0/000<br/>proof detail for [0, 8)"]:::tile

     R --> P
     P --> C
     P --> B
     R --> M
     P -.->|subtree and leaf detail| T

     classDef tile fill:#00843f,stroke:#00843f,color:#ffffff
     classDef memory fill:#276be9,stroke:#276be9,color:#ffffff
     classDef both fill:#276be9,stroke:#00843f,stroke-width:3px,color:#ffffff
     classDef summary fill:#9ca4af,stroke:#677384,color:#222832
     classDef computed fill:#9ca4af,stroke:#677384,color:#222832

There are now three logical ownership ranges:

============ ===========================
Leaf range   Proof detail available from
============ ===========================
``[0, 7)``   tiles only
``[7, 8)``   tiles and frontier
``[8, 11)``  frontier only
============ ===========================

The compacted in-memory summaries still contribute to ``root()``. "Tiles only"
means that a request for a leaf or complete subtree in that range must use the
tile source; it does not mean the in-memory root forgot the prefix hash.

Snapshot C: grow from 11 to 19 leaves
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Assume the tree was flushed and compacted at size 11, then 8 more leaves were
appended.

Before the second flush:

.. mermaid::

   flowchart TB
     N["n = 19<br/>flushed_size() is still 8"]:::computed
     T["[0, 7)<br/>tiles only"]:::tile
     B["[7, 8)<br/>boundary leaf in both"]:::both
     E["[8, 16)<br/>complete and eligible, but still frontier only"]:::memory
     F["[16, 19)<br/>incomplete frontier"]:::memory

     N --> T
     T --> B
     B --> E
     E --> F

     classDef tile fill:#00843f,stroke:#00843f,color:#ffffff
     classDef memory fill:#276be9,stroke:#276be9,color:#ffffff
     classDef both fill:#276be9,stroke:#00843f,stroke-width:3px,color:#ffffff
     classDef computed fill:#9ca4af,stroke:#677384,color:#222832

After the second flush and compaction:

.. mermaid::

   flowchart TB
     F["flush() writes tile/0/001<br/>flushed_size() = 16"]:::computed
     T0["tile/0/000 covers [0, 8)"]:::tile
     T1["tile/0/001 covers [8, 16)"]:::tile
     C["[0, 15)<br/>tiles only after compaction"]:::tile
     B["[15, 16)<br/>new boundary leaf in both"]:::both
     M["[16, 19)<br/>frontier only"]:::memory

     F --> T0
     T0 --> T1
     T1 --> C
     C --> B
     B --> M

     classDef tile fill:#00843f,stroke:#00843f,color:#ffffff
     classDef memory fill:#276be9,stroke:#276be9,color:#ffffff
     classDef both fill:#276be9,stroke:#00843f,stroke-width:3px,color:#ffffff
     classDef computed fill:#9ca4af,stroke:#677384,color:#222832

Snapshot D: 70 leaves
~~~~~~~~~~~~~~~~~~~~~

This is the first snapshot with a full illustrative level-1 tile:

.. mermaid::

   flowchart TB
     N["n = 70"]:::computed
     T0["Level 0<br/>8 files cover [0, 64)"]:::tile
     T1["Level 1<br/>tile/1/000 contains their 8 roots"]:::tile
     C["After compaction<br/>[0, 63) uses tiles for proof detail"]:::tile
     B["h63<br/>boundary leaf in both"]:::both
     M["[64, 70)<br/>frontier only"]:::memory

     N --> T0
     T0 --> T1
     T1 --> C
     C --> B
     B --> M

     classDef tile fill:#00843f,stroke:#00843f,color:#ffffff
     classDef memory fill:#276be9,stroke:#276be9,color:#ffffff
     classDef both fill:#276be9,stroke:#00843f,stroke-width:3px,color:#ffffff
     classDef computed fill:#9ca4af,stroke:#677384,color:#222832

Snapshot summary
~~~~~~~~~~~~~~~~

This table assumes each snapshot has just completed a successful flush and
compaction with zero retention:

+-----------+--------------------+---------------------+---------------+------------------+------------------+
| Tree size | ``flushed_size()`` | Files written       | Tiles only    | Tile + frontier  | Frontier only    |
+===========+====================+=====================+===============+==================+==================+
| 7         | 0                  | none                | none          | none             | ``[0, 7)``       |
+-----------+--------------------+---------------------+---------------+------------------+------------------+
| 11        | 8                  | ``tile/0/000``      | ``[0, 7)``    | ``[7, 8)``       | ``[8, 11)``      |
+-----------+--------------------+---------------------+---------------+------------------+------------------+
| 19        | 16                 | ``tile/0/000..001`` | ``[0, 15)``   | ``[15, 16)``     | ``[16, 19)``     |
+-----------+--------------------+---------------------+---------------+------------------+------------------+
| 70        | 64                 | 8 level-0 tiles +   | ``[0, 63)``   | ``[63, 64)``     | ``[64, 70)``     |
|           |                    | ``tile/1/000``      |               |                  |                  |
+-----------+--------------------+---------------------+---------------+------------------+------------------+

The default production geometry remains 256-wide. In particular, its first
level-1 tile starts at 65,536 leaves, not 64.

How a proof finds a subtree root
--------------------------------

``TiledTree`` gives ``ProofEngine`` a combined source. It tries the resident tree
first because that avoids I/O, then falls back to tiles:

.. mermaid::

   flowchart TB
     Q["ProofEngine requests R[a, b)"]:::computed
     L{"Is the range one leaf?"}:::decision
     A{"Otherwise, is its width a power of two<br/>and is the range aligned to that width?"}:::decision
     P{"Is this complete subtree<br/>fully resident in memory?"}:::decision
     M["Return the in-memory hash"]:::memory
     T{"Can the tile source resolve it<br/>inside flushed_size()?"}:::decision
     D["Read the appropriate tile entries<br/>and roll them up"]:::tile
     S["Split the range into smaller subtrees<br/>and resolve each one"]:::computed
     E["Fail if no source can resolve a required leaf"]:::error

     Q --> L
     L -->|yes| P
     L -->|no| A
     A -->|yes| P
     A -->|no| S
     P -->|yes| M
     P -->|no| T
     T -->|yes| D
     T -->|no, and range has multiple leaves| S
     T -->|no, and range is one leaf| E
     S -->|smaller range| Q

     classDef tile fill:#00843f,stroke:#00843f,color:#ffffff
     classDef memory fill:#276be9,stroke:#276be9,color:#ffffff
     classDef computed fill:#9ca4af,stroke:#677384,color:#222832
     classDef decision fill:#f66a0a,stroke:#f66a0a,color:#ffffff
     classDef error fill:#d72d47,stroke:#d72d47,color:#ffffff

For example, after compacting the 11-leaf tree:

- ``R[0, 8)`` is not fully resident, so memory declines it and tiles return it.
- ``R[8, 10)`` is resident, so memory returns it without touching disk.
- ``R[6, 11)`` crosses the boundary and is not one complete aligned subtree.
  The proof engine splits it into resolvable pieces.

Inclusion proof 1: entirely from one tile
-----------------------------------------

Consider a proof against tree size 8 after ``tile/0/000`` has been written and
the old leaves have been compacted. This may be the current size or a historical
prefix of a larger tree. We want to prove leaf 5.

Every required hash is reconstructed from ``tile/0/000``:

.. mermaid::

   flowchart TB
     R08["R[0, 8)"]:::tile
     P04["R[0, 4)<br/>proof"]:::proofTile
     R48["R[4, 8)"]:::tile
     R46["R[4, 6)"]:::tile
     P68["R[6, 8)<br/>proof"]:::proofTile
     P4["h4<br/>proof"]:::proofTile
     X5["h5<br/>target leaf"]:::target

     R08 --> P04
     R08 --> R48
     R48 --> R46
     R48 --> P68
     R46 --> P4
     R46 --> X5

     classDef tile fill:#00843f,stroke:#00843f,color:#ffffff
     classDef target fill:#222832,stroke:#222832,stroke-width:3px,color:#ffffff
     classDef proofTile fill:#d72d47,stroke:#00843f,stroke-width:3px,color:#ffffff

The proof payload is ordered from the leaf toward the root:

===== ============= ===================================== ==============
Order Proof hash    Position relative to the running hash Source
===== ============= ===================================== ==============
1     ``h4``        left                                  ``tile/0/000``
2     ``R[6, 8)``   right                                 ``tile/0/000``
3     ``R[0, 4)``   left                                  ``tile/0/000``
===== ============= ===================================== ==============

The internal roots in this table are computed on demand from the tile's leaf
hashes. They are not additional files.

Verification starts with ``h5``, combines the three proof hashes in order, and
arrives at ``R[0, 8)``.

Inclusion proof 2: tiles and memory together
--------------------------------------------

Return to the compacted 11-leaf tree and prove leaf 9 against the current root
``R[0, 11)``. This is the atlas's "frontier proof reaches backward" scenario.

The target and its nearby siblings are in the resident frontier. The old
8-leaf prefix is supplied as one tile-backed subtree root:

.. mermaid::

   flowchart TB
     R011["R[0, 11)"]:::computed
     P08["R[0, 8)<br/>proof from tile"]:::proofTile
     R811["R[8, 11)<br/>resident frontier"]:::memory
     R810["R[8, 10)"]:::memory
     P10["h10<br/>proof from frontier"]:::proofMemory
     P8["h8<br/>proof from frontier"]:::proofMemory
     X9["h9<br/>target leaf"]:::target

     R011 --> P08
     R011 --> R811
     R811 --> R810
     R811 --> P10
     R810 --> P8
     R810 --> X9

     classDef memory fill:#276be9,stroke:#276be9,color:#ffffff
     classDef computed fill:#9ca4af,stroke:#677384,color:#222832
     classDef target fill:#222832,stroke:#222832,stroke-width:3px,color:#ffffff
     classDef proofTile fill:#d72d47,stroke:#00843f,stroke-width:3px,color:#ffffff
     classDef proofMemory fill:#d72d47,stroke:#276be9,stroke-width:3px,color:#ffffff

The mixed proof payload is:

===== ============= ======== ==============
Order Proof hash    Position Source
===== ============= ======== ==============
1     ``h8``        left     frontier
2     ``h10``       right    frontier
3     ``R[0, 8)``   left     ``tile/0/000``
===== ============= ======== ==============

The caller sees one ordinary ``merkle::Path``. Source selection is internal; the
proof format does not mark some hashes as "tile" and others as "memory".

Proving an old leaf in the current tree is mixed in the opposite direction.
For example, the atlas's proof for leaf 2 at size 11 gets its target and lower
siblings from ``tile/0/000``, then gets the final sibling ``R[8, 11)`` by
reducing the resident frontier.

Consistency proofs: the idea
----------------------------

An inclusion proof answers:

   Is this leaf part of this tree root?

A consistency proof answers:

   Can the tree with ``m`` leaves be extended, without changing its first ``m``
   leaves, to produce the tree with ``n`` leaves?

The verifier already knows:

- ``m`` and the old root ``R[0, m)``;
- ``n`` and the new root ``R[0, n)``.

The proof supplies enough complete subtree roots to reconstruct both roots
through a shared history.

The producer recursively follows the part of the new tree that contains the
old boundary and emits the sibling subtree at each split:

.. mermaid::

   flowchart TB
     A["Start with [0, n) and old size m"]:::computed
     B["Split at the largest power of two<br/>smaller than the current range"]:::computed
     C{"Which side contains<br/>the old boundary?"}:::decision
     D["Recurse into that side"]:::computed
     E["Emit the other side's root<br/>as a proof hash"]:::proof
     F{"Reached exactly<br/>the old boundary?"}:::decision
     G["Return proof hashes<br/>from deepest to highest"]:::proof

     A --> B
     B --> C
     C --> D
     D --> E
     E --> F
     F -->|no| B
     F -->|yes| G

     classDef computed fill:#9ca4af,stroke:#677384,color:#222832
     classDef decision fill:#f66a0a,stroke:#f66a0a,color:#ffffff
     classDef proof fill:#d72d47,stroke:#d72d47,color:#ffffff

Each emitted range is resolved through the same memory-first, tile-second
source used by inclusion proofs.

Consistency proof 1: a perfect old tree
---------------------------------------

First prove that the 64-leaf tree is a prefix of the 70-leaf tree. This is the
atlas's "consistency across the flush line" scenario:

.. code:: cpp

   auto proof = log.consistency_proof(64, 70);

Because 64 is a power of two, the old root is already one complete left subtree
backed by the level-1 tile. The proof needs only the new right-hand frontier:

.. mermaid::

   flowchart TB
     OLD["Known old root<br/>R[0, 64)"]:::tile
     EXT["proof[0]<br/>R[64, 70) reduced from frontier"]:::proofComputed
     JOIN["H(R[0, 64), R[64, 70))"]:::computed
     NEW["Expected new root<br/>R[0, 70)"]:::result

     OLD --> JOIN
     EXT --> JOIN
     JOIN --> NEW

     classDef tile fill:#00843f,stroke:#00843f,color:#ffffff
     classDef proofComputed fill:#d72d47,stroke:#9ca4af,stroke-width:3px,color:#ffffff
     classDef computed fill:#9ca4af,stroke:#677384,color:#222832
     classDef result fill:#0a7d91,stroke:#0a7d91,stroke-width:3px,color:#ffffff

The old root is available from ``tile/1/000``; the extension is reduced from
the resident frontier. Verification combines the known old root with the
single proof hash and compares the result with the known new root.

Consistency proof 2: a non-perfect old tree
-------------------------------------------

Now prove that the 23-leaf tree is a prefix of the 68-leaf tree inside the same
70-leaf backing tree. This is the atlas's "two unaligned sizes cross the
boundary" scenario:

.. code:: cpp

   auto proof = log.consistency_proof(23, 68);

Size 23 is not a power of two, so the old root does not line up with a single
node in the 68-leaf tree. The proof decomposes the relevant ranges:

.. mermaid::

   flowchart TB
     R068["R[0, 68)"]:::computed
     R064["R[0, 64)"]:::tile
     P6468["P7 = R[64, 68)<br/>frontier"]:::proofMemory
     R032["R[0, 32)"]:::tile
     P3264["P6 = R[32, 64)<br/>tile"]:::proofTile
     P016["P5 = R[0, 16)<br/>tile"]:::proofTile
     R1632["R[16, 32)"]:::tile
     R1624["R[16, 24)"]:::tile
     P2432["P4 = R[24, 32)<br/>tile"]:::proofTile
     R2024["R[20, 24)"]:::tile
     P1620["P3 = R[16, 20)<br/>tile"]:::proofTile
     P2022["P2 = R[20, 22)<br/>tile"]:::proofTile
     R2224["R[22, 24)"]:::tile
     P22["P0 = h22<br/>tile seed"]:::proofTile
     P23["P1 = h23<br/>tile"]:::proofTile

     R068 --> R064
     R068 --> P6468
     R064 --> R032
     R064 --> P3264
     R032 --> P016
     R032 --> R1632
     R1632 --> R1624
     R1632 --> P2432
     R1624 --> P1620
     R1624 --> R2024
     R2024 --> P2022
     R2024 --> R2224
     R2224 --> P22
     R2224 --> P23

     classDef tile fill:#00843f,stroke:#00843f,color:#ffffff
     classDef computed fill:#9ca4af,stroke:#677384,color:#222832
     classDef proofTile fill:#d72d47,stroke:#00843f,stroke-width:3px,color:#ffffff
     classDef proofMemory fill:#d72d47,stroke:#276be9,stroke-width:3px,color:#ffffff

The proof vector contains hashes only; the range labels are shown here to make
the algorithm visible. Given ``m = 23`` and ``n = 68``, the verifier derives
where each hash belongs.

+--------+------------------+----------+---------------------------------------+
| Order  | Atlas range      | Source   | Why it is needed                      |
+========+==================+==========+=======================================+
| ``P0`` | ``R[22, 23)``    | tile     | Seed both reconstructions             |
+--------+------------------+----------+---------------------------------------+
| ``P1`` | ``R[23, 24)``    | tile     | Extend only the new reconstruction    |
+--------+------------------+----------+---------------------------------------+
| ``P2`` | ``R[20, 22)``    | tile     | Extend both reconstructions left      |
+--------+------------------+----------+---------------------------------------+
| ``P3`` | ``R[16, 20)``    | tile     | Extend both reconstructions left      |
+--------+------------------+----------+---------------------------------------+
| ``P4`` | ``R[24, 32)``    | tile     | Extend only the new reconstruction    |
+--------+------------------+----------+---------------------------------------+
| ``P5`` | ``R[0, 16)``     | tile     | Complete the old root and new prefix  |
+--------+------------------+----------+---------------------------------------+
| ``P6`` | ``R[32, 64)``    | tile     | Extend the new reconstruction         |
+--------+------------------+----------+---------------------------------------+
| ``P7`` | ``R[64, 68)``    | frontier | Cross the flush line to the new size  |
+--------+------------------+----------+---------------------------------------+

Verification evolves two accumulators. The verifier uses the bit structure of
``m`` and ``n`` to decide which accumulator each proof hash updates. ``P0``
seeds a subtree shared by both histories; ``P2``, ``P3``, and ``P5`` grow both
accumulators to the left. ``P1``, ``P4``, ``P6``, and ``P7`` extend only the
new accumulator to the right.

.. mermaid::

   flowchart TB
     S["Seed both with P0<br/>old = new = R[22, 23)"]:::proofTile
     A["P1 right<br/>new = R[22, 24)"]:::computed
     B["P2 left<br/>old = R[20, 23)<br/>new = R[20, 24)"]:::computed
     C["P3 left<br/>old = R[16, 23)<br/>new = R[16, 24)"]:::computed
     D["P4 right<br/>new = R[16, 32)"]:::computed
     E["P5 left<br/>old = R[0, 23)<br/>new = R[0, 32)"]:::computed
     F["P6 right<br/>new = R[0, 64)"]:::computed
     G["P7 right<br/>new = R[0, 68)"]:::computed
     V["Compare both reconstructed roots<br/>with the caller's old and new roots"]:::result

     S --> A
     A --> B
     B --> C
     C --> D
     D --> E
     E --> F
     F --> G
     G --> V

     classDef computed fill:#9ca4af,stroke:#677384,color:#222832
     classDef proofTile fill:#d72d47,stroke:#00843f,stroke-width:3px,color:#ffffff
     classDef result fill:#0a7d91,stroke:#0a7d91,stroke-width:3px,color:#ffffff

This example is mixed in a useful way:

- The old 23-leaf state is reconstructed entirely from tiled history.
- The new 68-leaf state reuses that tiled history, crosses the level-1 boundary,
  and obtains ``R[64, 68)`` from the resident frontier.
- The proof is still an ordinary vector of hashes, independent of where each
  hash was found.

The complete mental model
-------------------------

.. mermaid::

   flowchart TB
     A["Append leaf hashes"]:::memory
     B["In-memory left-balanced Merkle tree"]:::memory
     C["An 8-entry range becomes complete<br/>(256 entries by default)"]:::both
     D["flush() publishes immutable full tiles"]:::tile
     E["compact() optionally drops old resident detail"]:::summary
     F["MemoryHashSource<br/>serves whatever remains resident"]:::memory
     G["TileHashSource<br/>serves the flushed prefix"]:::tile
     H["CombinedHashSource<br/>tries memory, then tiles"]:::both
     I["ProofEngine"]:::computed
     J["Current or historical root"]:::result
     K["Inclusion proof"]:::result
     L["Consistency proof"]:::result

     A --> B
     B --> C
     C --> D
     D -.->|optional| E
     D --> F
     D --> G
     E --> F
     F --> H
     G --> H
     H --> I
     I --> J
     I --> K
     I --> L

     classDef tile fill:#00843f,stroke:#00843f,color:#ffffff
     classDef memory fill:#276be9,stroke:#276be9,color:#ffffff
     classDef both fill:#276be9,stroke:#00843f,stroke-width:3px,color:#ffffff
     classDef summary fill:#9ca4af,stroke:#677384,color:#222832
     classDef computed fill:#9ca4af,stroke:#677384,color:#222832
     classDef result fill:#0a7d91,stroke:#0a7d91,stroke-width:3px,color:#ffffff

The important boundary is always the last successfully flushed full tile:

- Below it, immutable tiles can preserve proof detail after compaction.
- Above it, the incomplete frontier must remain resident in memory.
- A proof may resolve several component subtrees from either side of the
  boundary, but the caller receives one normal proof.
- None of these rules depends on the atlas width of 8. The default aliases use
  the same model with 256-entry tiles.
