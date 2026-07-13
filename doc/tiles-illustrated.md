# Tiled Merkle trees: an illustrated walkthrough

This page builds a visual model of how an append-only Merkle tree moves from
memory into immutable tile files, and how proofs continue to work across both
places.

> [!IMPORTANT]
> **Every example on this page uses an imaginary tile width of 32 entries
> purely to keep the diagrams readable.**
>
> The merklecpp implementation remains fixed at `TILE_WIDTH = 256` and
> `TILE_HEIGHT = 8`. A width of 32 is not a configuration option, and this page
> does not propose changing the code, file format, defaults, or examples
> elsewhere. Unless a section explicitly says "illustrative", use 256.

## The scaled-down model

A production tile contains 256 entries and spans 8 binary tree levels because
`256 = 2^8`. This page scales that geometry down to 32 entries and 5 levels
because `32 = 2^5`.

| Property | This page only | Production merklecpp |
|---|---:|---:|
| Tile width | 32 entries | 256 entries |
| Tree levels spanned by one tile | 5 | 8 |
| Leaves covered by one full level-0 tile | 32 | 256 |
| Leaves covered by one level-1 entry | 32 | 256 |
| Leaves covered by one full level-1 tile | 1,024 | 65,536 |

The scaling changes only the numbers in the drawings. The rules are the same:

1. Only full tiles are written.
2. A level-0 tile contains leaf hashes.
3. A higher-level tile contains roots of complete tiles from the level below.
4. The incomplete right-hand frontier remains in memory.
5. Published tiles are immutable.
6. Proofs can resolve subtree roots from memory, tiles, or both.

### Notation

- `h7` is the hash of leaf 7.
- `R[a, b)` is the Merkle root of the half-open leaf range `[a, b)`.
- `tile/L/NNN` is tile index `NNN` at tile level `L`.
- "Resident" means the in-memory tree can still expand that range to answer
  proof requests.
- "Compacted" means the in-memory tree retains enough summary hashes to keep
  its root correct, but no longer retains all detail below that range.

### Colors used below

```mermaid
flowchart TB
  T["Tile-backed hash or range"]:::tile
  M["Resident in-memory hash or range"]:::memory
  B["Available from both tiles and memory"]:::both
  S["Compacted in-memory summary"]:::summary
  X["Leaf being proved"]:::target
  PT["Hash emitted in a proof<br/>blue outline: from tiles"]:::proofTile
  PM["Hash emitted in a proof<br/>green outline: from memory"]:::proofMemory

  T ~~~ M
  M ~~~ B
  B ~~~ S
  S ~~~ X
  X ~~~ PT
  PT ~~~ PM

  classDef tile fill:#dbeafe,stroke:#2563eb,color:#111827
  classDef memory fill:#dcfce7,stroke:#16a34a,color:#111827
  classDef both fill:#f3e8ff,stroke:#9333ea,color:#111827
  classDef summary fill:#e5e7eb,stroke:#6b7280,color:#111827
  classDef target fill:#fed7aa,stroke:#ea580c,stroke-width:3px,color:#111827
  classDef proofTile fill:#fef3c7,stroke:#2563eb,stroke-width:3px,color:#111827
  classDef proofMemory fill:#fef3c7,stroke:#16a34a,stroke-width:3px,color:#111827
```

## What `flush()` and `compact()` each do

Appending, flushing, and compacting are separate operations:

```mermaid
flowchart TB
  A["append(h)<br/>Add a leaf hash to the in-memory tree"]:::memory
  B["A complete 32-entry range now exists<br/>but no file is written automatically"]:::memory
  C["flush()<br/>Write every newly complete full tile"]:::tile
  D["The same range exists on disk and in memory<br/>(the default after flush)"]:::both
  E["compact()<br/>Optionally discard old resident detail"]:::summary
  F["Old detail is served from tiles;<br/>the incomplete frontier stays in memory"]:::both

  A --> B
  B --> C
  C --> D
  D --> E
  E --> F

  classDef tile fill:#dbeafe,stroke:#2563eb,color:#111827
  classDef memory fill:#dcfce7,stroke:#16a34a,color:#111827
  classDef both fill:#f3e8ff,stroke:#9333ea,color:#111827
  classDef summary fill:#e5e7eb,stroke:#6b7280,color:#111827
```

`flush()` does not compact by default. Setting `compact_on_flush = true` makes
the final two steps happen in one call, but the durability rule is unchanged:
compaction happens only after all required tile writes succeed.

If a tile write fails, `immutable_size()` may advance past `flushed_size()`
because a published tile cannot be rolled back. Keep the same tree contents and
retry the flush. See
[Flushing and compaction](tiles-guide.md#flushing-and-compaction) for the full
interrupted-write contract.

## What is inside a tile file?

In the illustrative model, `tile/0/000` is the concatenation of 32 leaf hashes:

```mermaid
flowchart TB
  F["tile/0/000<br/>32 serialized hashes"]:::tile
  A["entries 0..7<br/>h0 ... h7"]:::tile
  B["entries 8..15<br/>h8 ... h15"]:::tile
  C["entries 16..23<br/>h16 ... h23"]:::tile
  D["entries 24..31<br/>h24 ... h31"]:::tile
  R["R[0, 32)<br/>reconstructed by hashing the entries"]:::computed
  N["Internal binary-tree nodes are reconstructed;<br/>they are not separately stored in the file"]:::note

  F -->|first bytes| A
  A -->|followed by| B
  B -->|followed by| C
  C -->|followed by| D
  D --> R
  R --> N

  classDef tile fill:#dbeafe,stroke:#2563eb,color:#111827
  classDef computed fill:#ede9fe,stroke:#7c3aed,color:#111827
  classDef note fill:#f9fafb,stroke:#9ca3af,color:#374151
```

At level 1, each entry is already the root of 32 leaves:

```mermaid
flowchart TB
  L1["tile/1/000<br/>32 serialized subtree roots"]:::tile
  L0A["entry 0<br/>root(tile/0/000) = R[0, 32)"]:::tile
  L0B["entry 1<br/>root(tile/0/001) = R[32, 64)"]:::tile
  L0C["entries 2..30<br/>..."]:::tile
  L0Z["entry 31<br/>root(tile/0/031) = R[992, 1024)"]:::tile
  ROOT["R[0, 1024)<br/>reconstructed from tile/1/000"]:::computed

  L1 --> L0A
  L0A --> L0B
  L0B --> L0C
  L0C --> L0Z
  L0Z --> ROOT

  classDef tile fill:#dbeafe,stroke:#2563eb,color:#111827
  classDef computed fill:#ede9fe,stroke:#7c3aed,color:#111827
```

The production version of the second diagram needs 256 level-0 tile roots, so
its first full level-1 tile appears at 65,536 leaves rather than 1,024.

## On-disk file layout

After an illustrative 1,030-leaf tree is flushed, the full-tile boundary is
1,024:

```text
prefix/
  tile/
    0/
      000        # h0       ... h31
      001        # h32      ... h63
      ...
      031        # h992     ... h1023
    1/
      000        # R[0,32), R[32,64), ... R[992,1024)
```

Leaves `[1024, 1030)` do not appear in a tile file because they do not complete
another 32-entry tile. They remain in memory.

```mermaid
flowchart TB
  N["n = 1,030 leaves"]:::computed
  C["covered = floor(1,030 / 32) * 32 = 1,024"]:::computed
  L0["32 full level-0 files<br/>tile/0/000 through tile/0/031"]:::tile
  L1["1 full level-1 file<br/>tile/1/000"]:::tile
  M["6-leaf frontier<br/>[1024, 1030) in memory"]:::memory

  N --> C
  C --> L0
  L0 --> L1
  L1 --> M

  classDef tile fill:#dbeafe,stroke:#2563eb,color:#111827
  classDef memory fill:#dcfce7,stroke:#16a34a,color:#111827
  classDef computed fill:#ede9fe,stroke:#7c3aed,color:#111827
```

The optional `tile/entries/` bundles are omitted here. They store raw
application entries, not Merkle tree nodes, and do not change proof generation.

## Tree growth, one snapshot at a time

The next snapshots assume `retention_margin = 0`. Where compaction is shown,
merklecpp still retains the final tiled leaf as a boundary leaf. This is why
the "both" range below is one leaf wide.

### Snapshot A: 20 leaves

No full 32-entry tile exists:

```mermaid
flowchart TB
  N["n = 20"]:::computed
  C["full-tile boundary = 0"]:::computed
  M["Memory only<br/>[0, 20)"]:::memory
  D["Disk<br/>no tile files"]:::empty

  N --> C
  C --> M
  M --> D

  classDef memory fill:#dcfce7,stroke:#16a34a,color:#111827
  classDef computed fill:#ede9fe,stroke:#7c3aed,color:#111827
  classDef empty fill:#f9fafb,stroke:#9ca3af,color:#374151
```

Calling `flush()` at this point writes nothing. Every root and proof is served
from the in-memory tree.

### Snapshot B: 40 leaves, before the first flush

The first 32 leaves form a complete tile, but tile creation is explicit:

```mermaid
flowchart TB
  N["n = 40"]:::computed
  M0["[0, 32)<br/>complete and eligible, still memory only"]:::memory
  M1["[32, 40)<br/>incomplete frontier, memory only"]:::memory
  D["Disk<br/>still empty until flush()"]:::empty

  N --> M0
  M0 --> M1
  M1 --> D

  classDef memory fill:#dcfce7,stroke:#16a34a,color:#111827
  classDef computed fill:#ede9fe,stroke:#7c3aed,color:#111827
  classDef empty fill:#f9fafb,stroke:#9ca3af,color:#374151
```

### Snapshot B: 40 leaves, after `flush()`

The default `flush()` writes the full prefix but does not remove it from
memory:

```mermaid
flowchart TB
  F["flush() succeeds<br/>flushed_size() = 32"]:::computed
  B["[0, 32)<br/>tile/0/000 + resident memory"]:::both
  M["[32, 40)<br/>resident memory only"]:::memory

  F --> B
  B --> M

  classDef memory fill:#dcfce7,stroke:#16a34a,color:#111827
  classDef both fill:#f3e8ff,stroke:#9333ea,color:#111827
  classDef computed fill:#ede9fe,stroke:#7c3aed,color:#111827
```

At this point a proof may be answered entirely from memory even though a tile
copy exists.

### Snapshot B: 40 leaves, after compaction

With zero retention, compaction drops old leaf detail while preserving leaf 31
as the rollback boundary:

```mermaid
flowchart TB
  R["R[0, 40)<br/>current in-memory root"]:::computed
  P["R[0, 32)<br/>prefix represented by compacted summaries"]:::summary
  C["[0, 31)<br/>not leaf-addressable in memory"]:::summary
  B["h31<br/>retained boundary leaf"]:::both
  M["R[32, 40)<br/>fully resident frontier"]:::memory
  T["tile/0/000<br/>proof detail for [0, 32)"]:::tile

  R --> P
  P --> C
  P --> B
  R --> M
  P -.->|subtree and leaf detail| T

  classDef tile fill:#dbeafe,stroke:#2563eb,color:#111827
  classDef memory fill:#dcfce7,stroke:#16a34a,color:#111827
  classDef both fill:#f3e8ff,stroke:#9333ea,color:#111827
  classDef summary fill:#e5e7eb,stroke:#6b7280,color:#111827
  classDef computed fill:#ede9fe,stroke:#7c3aed,color:#111827
```

There are now three logical ownership ranges:

| Leaf range | Proof detail available from |
|---|---|
| `[0, 31)` | tiles only |
| `[31, 32)` | tiles and memory |
| `[32, 40)` | memory only |

The compacted in-memory summaries still contribute to `root()`. "Tiles only"
means that a request for a leaf or complete subtree in that range must use the
tile source; it does not mean the in-memory root forgot the prefix hash.

### Snapshot C: grow from 40 to 72 leaves

Assume the tree was flushed and compacted at size 40, then 32 more leaves were
appended.

Before the second flush:

```mermaid
flowchart TB
  N["n = 72<br/>flushed_size() is still 32"]:::computed
  T["[0, 31)<br/>tiles only"]:::tile
  B["[31, 32)<br/>boundary leaf in both"]:::both
  E["[32, 64)<br/>complete and eligible, but still memory only"]:::memory
  F["[64, 72)<br/>incomplete memory frontier"]:::memory

  N --> T
  T --> B
  B --> E
  E --> F

  classDef tile fill:#dbeafe,stroke:#2563eb,color:#111827
  classDef memory fill:#dcfce7,stroke:#16a34a,color:#111827
  classDef both fill:#f3e8ff,stroke:#9333ea,color:#111827
  classDef computed fill:#ede9fe,stroke:#7c3aed,color:#111827
```

After the second flush and compaction:

```mermaid
flowchart TB
  F["flush() writes tile/0/001<br/>flushed_size() = 64"]:::computed
  T0["tile/0/000 covers [0, 32)"]:::tile
  T1["tile/0/001 covers [32, 64)"]:::tile
  C["[0, 63)<br/>tiles only after compaction"]:::tile
  B["[63, 64)<br/>new boundary leaf in both"]:::both
  M["[64, 72)<br/>memory-only frontier"]:::memory

  F --> T0
  T0 --> T1
  T1 --> C
  C --> B
  B --> M

  classDef tile fill:#dbeafe,stroke:#2563eb,color:#111827
  classDef memory fill:#dcfce7,stroke:#16a34a,color:#111827
  classDef both fill:#f3e8ff,stroke:#9333ea,color:#111827
  classDef computed fill:#ede9fe,stroke:#7c3aed,color:#111827
```

### Snapshot D: 1,030 leaves

This is the first snapshot with a full illustrative level-1 tile:

```mermaid
flowchart TB
  N["n = 1,030"]:::computed
  T0["Level 0<br/>32 files cover [0, 1024)"]:::tile
  T1["Level 1<br/>tile/1/000 contains their 32 roots"]:::tile
  C["After compaction<br/>[0, 1023) uses tiles for proof detail"]:::tile
  B["h1023<br/>boundary leaf in both"]:::both
  M["[1024, 1030)<br/>memory-only frontier"]:::memory

  N --> T0
  T0 --> T1
  T1 --> C
  C --> B
  B --> M

  classDef tile fill:#dbeafe,stroke:#2563eb,color:#111827
  classDef memory fill:#dcfce7,stroke:#16a34a,color:#111827
  classDef both fill:#f3e8ff,stroke:#9333ea,color:#111827
  classDef computed fill:#ede9fe,stroke:#7c3aed,color:#111827
```

### Snapshot summary

This table assumes each snapshot has just completed a successful flush and
compaction with zero retention:

| Tree size | `flushed_size()` | Files written | Tiles only | Tiles + memory | Memory only |
|---:|---:|---|---|---|---|
| 20 | 0 | none | none | none | `[0, 20)` |
| 40 | 32 | `tile/0/000` | `[0, 31)` | `[31, 32)` | `[32, 40)` |
| 72 | 64 | `tile/0/000..001` | `[0, 63)` | `[63, 64)` | `[64, 72)` |
| 1,030 | 1,024 | 32 level-0 tiles + `tile/1/000` | `[0, 1023)` | `[1023, 1024)` | `[1024, 1030)` |

Again, multiply the tile geometry back to 256 for production. In particular,
the production level-1 example starts at 65,536 leaves, not 1,024.

## How a proof finds a subtree root

`TiledTree` gives `ProofEngine` a combined source. It tries the resident tree
first because that avoids I/O, then falls back to tiles:

```mermaid
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

  classDef tile fill:#dbeafe,stroke:#2563eb,color:#111827
  classDef memory fill:#dcfce7,stroke:#16a34a,color:#111827
  classDef computed fill:#ede9fe,stroke:#7c3aed,color:#111827
  classDef decision fill:#fff7ed,stroke:#c2410c,color:#111827
  classDef error fill:#fee2e2,stroke:#dc2626,color:#111827
```

For example, after compacting the 40-leaf tree:

- `R[0, 32)` is not fully resident, so memory declines it and tiles return it.
- `R[32, 36)` is resident, so memory returns it without touching disk.
- `R[24, 40)` crosses the boundary and is not one complete aligned subtree.
  The proof engine splits it into resolvable pieces.

## Inclusion proof 1: entirely from one tile

Consider a proof against tree size 32 after `tile/0/000` has been written and
the old leaves have been compacted. This may be the current size or a historical
prefix of a larger tree. We want to prove leaf 5.

Every required hash is reconstructed from `tile/0/000`:

```mermaid
flowchart TB
  R032["R[0, 32)"]:::tile
  R016["R[0, 16)"]:::tile
  P1632["R[16, 32)<br/>proof"]:::proofTile
  R08["R[0, 8)"]:::tile
  P816["R[8, 16)<br/>proof"]:::proofTile
  P04["R[0, 4)<br/>proof"]:::proofTile
  R48["R[4, 8)"]:::tile
  R46["R[4, 6)"]:::tile
  P68["R[6, 8)<br/>proof"]:::proofTile
  P4["h4<br/>proof"]:::proofTile
  X5["h5<br/>target leaf"]:::target

  R032 --> R016
  R032 --> P1632
  R016 --> R08
  R016 --> P816
  R08 --> P04
  R08 --> R48
  R48 --> R46
  R48 --> P68
  R46 --> P4
  R46 --> X5

  classDef tile fill:#dbeafe,stroke:#2563eb,color:#111827
  classDef target fill:#fed7aa,stroke:#ea580c,stroke-width:3px,color:#111827
  classDef proofTile fill:#fef3c7,stroke:#2563eb,stroke-width:3px,color:#111827
```

The proof payload is ordered from the leaf toward the root:

| Order | Proof hash | Position relative to the running hash | Source |
|---:|---|---|---|
| 1 | `h4` | left | `tile/0/000` |
| 2 | `R[6, 8)` | right | `tile/0/000` |
| 3 | `R[0, 4)` | left | `tile/0/000` |
| 4 | `R[8, 16)` | right | `tile/0/000` |
| 5 | `R[16, 32)` | right | `tile/0/000` |

The internal roots in this table are computed on demand from the tile's leaf
hashes. They are not additional files.

Verification starts with `h5`, combines the five proof hashes in order, and
arrives at `R[0, 32)`.

## Inclusion proof 2: tiles and memory together

Return to the compacted 40-leaf tree and prove leaf 36 against the current root
`R[0, 40)`.

The target and its nearby siblings are in the resident frontier. The old
32-leaf prefix is supplied as one tile-backed subtree root:

```mermaid
flowchart TB
  R040["R[0, 40)"]:::computed
  P032["R[0, 32)<br/>proof from tile"]:::proofTile
  R3240["R[32, 40)<br/>resident frontier"]:::memory
  P3236["R[32, 36)<br/>proof from memory"]:::proofMemory
  R3640["R[36, 40)"]:::memory
  R3638["R[36, 38)"]:::memory
  P3840["R[38, 40)<br/>proof from memory"]:::proofMemory
  X36["h36<br/>target leaf"]:::target
  P37["h37<br/>proof from memory"]:::proofMemory

  R040 --> P032
  R040 --> R3240
  R3240 --> P3236
  R3240 --> R3640
  R3640 --> R3638
  R3640 --> P3840
  R3638 --> X36
  R3638 --> P37

  classDef memory fill:#dcfce7,stroke:#16a34a,color:#111827
  classDef computed fill:#ede9fe,stroke:#7c3aed,color:#111827
  classDef target fill:#fed7aa,stroke:#ea580c,stroke-width:3px,color:#111827
  classDef proofTile fill:#fef3c7,stroke:#2563eb,stroke-width:3px,color:#111827
  classDef proofMemory fill:#fef3c7,stroke:#16a34a,stroke-width:3px,color:#111827
```

The mixed proof payload is:

| Order | Proof hash | Position | Source |
|---:|---|---|---|
| 1 | `h37` | right | memory |
| 2 | `R[38, 40)` | right | memory |
| 3 | `R[32, 36)` | left | memory |
| 4 | `R[0, 32)` | left | `tile/0/000` |

The caller sees one ordinary `merkle::Path`. Source selection is internal; the
proof format does not mark some hashes as "tile" and others as "memory".

Proving an old leaf in the current tree is mixed in the opposite direction.
For example, a proof for leaf 5 at size 40 gets its target and lower siblings
from `tile/0/000`, then gets the final sibling `R[32, 40)` from memory.

## Consistency proofs: the idea

An inclusion proof answers:

> Is this leaf part of this tree root?

A consistency proof answers:

> Can the tree with `m` leaves be extended, without changing its first `m`
> leaves, to produce the tree with `n` leaves?

The verifier already knows:

- `m` and the old root `R[0, m)`;
- `n` and the new root `R[0, n)`.

The proof supplies enough complete subtree roots to reconstruct both roots
through a shared history.

The producer recursively follows the part of the new tree that contains the
old boundary and emits the sibling subtree at each split:

```mermaid
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

  classDef computed fill:#ede9fe,stroke:#7c3aed,color:#111827
  classDef decision fill:#fff7ed,stroke:#c2410c,color:#111827
  classDef proof fill:#fef3c7,stroke:#b45309,color:#111827
```

Each emitted range is resolved through the same memory-first, tile-second
source used by inclusion proofs.

## Consistency proof 1: a perfect old tree

First prove that the 32-leaf tree is a prefix of the 40-leaf tree:

```cpp
auto proof = log.consistency_proof(32, 40);
```

Because 32 is a power of two, the old root is already one complete left
subtree. The proof needs only the new right-hand range:

```mermaid
flowchart TB
  OLD["Known old root<br/>R[0, 32)"]:::tile
  EXT["proof[0]<br/>R[32, 40) from memory"]:::proofMemory
  JOIN["H(R[0, 32), R[32, 40))"]:::computed
  NEW["Expected new root<br/>R[0, 40)"]:::result

  OLD --> JOIN
  EXT --> JOIN
  JOIN --> NEW

  classDef tile fill:#dbeafe,stroke:#2563eb,color:#111827
  classDef proofMemory fill:#fef3c7,stroke:#16a34a,stroke-width:3px,color:#111827
  classDef computed fill:#ede9fe,stroke:#7c3aed,color:#111827
  classDef result fill:#dcfce7,stroke:#15803d,stroke-width:3px,color:#111827
```

The old root may have been calculated from `tile/0/000`; the extension is
resident in memory. Verification combines the known old root with the single
proof hash and compares the result with the known new root.

## Consistency proof 2: a non-perfect old tree

Now prove that the 20-leaf tree is a prefix of the 40-leaf tree:

```cpp
auto proof = log.consistency_proof(20, 40);
```

Size 20 is not a power of two, so the old root does not line up with a single
node in the 40-leaf tree. The proof decomposes the relevant ranges:

```mermaid
flowchart TB
  R040["R[0, 40)"]:::computed
  R032["R[0, 32)"]:::tile
  P3240["P4 = R[32, 40)<br/>memory"]:::proofMemory
  P016["P3 = R[0, 16)<br/>tile"]:::proofTile
  R1632["R[16, 32)"]:::tile
  R1624["R[16, 24)"]:::tile
  P2432["P2 = R[24, 32)<br/>tile"]:::proofTile
  P1620["P0 = R[16, 20)<br/>tile seed"]:::proofTile
  P2024["P1 = R[20, 24)<br/>tile"]:::proofTile

  R040 --> R032
  R040 --> P3240
  R032 --> P016
  R032 --> R1632
  R1632 --> R1624
  R1632 --> P2432
  R1624 --> P1620
  R1624 --> P2024

  classDef tile fill:#dbeafe,stroke:#2563eb,color:#111827
  classDef computed fill:#ede9fe,stroke:#7c3aed,color:#111827
  classDef proofTile fill:#fef3c7,stroke:#2563eb,stroke-width:3px,color:#111827
  classDef proofMemory fill:#fef3c7,stroke:#16a34a,stroke-width:3px,color:#111827
```

The proof vector contains hashes only; the range labels are shown here to make
the algorithm visible. Given `m = 20` and `n = 40`, the verifier derives where
each hash belongs.

| Order | Illustrative range | Source | Why it is needed |
|---:|---|---|---|
| `P0` | `R[16, 20)` | tile | Seed shared by old and new reconstructions |
| `P1` | `R[20, 24)` | tile | Extend only the new reconstruction |
| `P2` | `R[24, 32)` | tile | Extend only the new reconstruction |
| `P3` | `R[0, 16)` | tile | Complete both the old and new left sides |
| `P4` | `R[32, 40)` | memory | Extend the new reconstruction to size 40 |

Verification evolves two accumulators:

The verifier uses the bit structure of `m` and `n` to decide which accumulator
each proof hash updates. Intuitively, `P0` seeds a subtree shared by both
histories and `P3` completes that shared old-tree boundary. `P1`, `P2`, and
`P4` cover leaves at or beyond the old size, so they extend only the new
accumulator.

```mermaid
flowchart TB
  S["Seed both accumulators with P0<br/>old = new = R[16, 20)"]:::proofTile
  A["Combine P1 on the right<br/>new = R[16, 24)"]:::tile
  B["Combine P2 on the right<br/>new = R[16, 32)"]:::tile
  C["Combine P3 on the left<br/>old = R[0, 20)<br/>new = R[0, 32)"]:::tile
  D["Combine P4 on the right<br/>new = R[0, 40)"]:::memory
  V["Compare both reconstructed roots<br/>with the caller's old and new roots"]:::result

  S --> A
  A --> B
  B --> C
  C --> D
  D --> V

  classDef tile fill:#dbeafe,stroke:#2563eb,color:#111827
  classDef memory fill:#dcfce7,stroke:#16a34a,color:#111827
  classDef proofTile fill:#fef3c7,stroke:#2563eb,stroke-width:3px,color:#111827
  classDef result fill:#dcfce7,stroke:#15803d,stroke-width:3px,color:#111827
```

This example is mixed in a useful way:

- The old 20-leaf state can be reconstructed from the first tile even though
  the live in-memory tree has compacted those leaves.
- The newly appended range `[32, 40)` comes from memory.
- The proof is still an ordinary vector of hashes, independent of where each
  hash was found.

## The complete mental model

```mermaid
flowchart TB
  A["Append leaf hashes"]:::memory
  B["In-memory left-balanced Merkle tree"]:::memory
  C["A 32-entry range becomes complete<br/>(256 entries in production)"]:::both
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

  classDef tile fill:#dbeafe,stroke:#2563eb,color:#111827
  classDef memory fill:#dcfce7,stroke:#16a34a,color:#111827
  classDef both fill:#f3e8ff,stroke:#9333ea,color:#111827
  classDef summary fill:#e5e7eb,stroke:#6b7280,color:#111827
  classDef computed fill:#ede9fe,stroke:#7c3aed,color:#111827
  classDef result fill:#dcfce7,stroke:#15803d,stroke-width:3px,color:#111827
```

The important boundary is always the last successfully flushed full tile:

- Below it, immutable tiles can preserve proof detail after compaction.
- Above it, the incomplete frontier must remain resident in memory.
- A proof may resolve several component subtrees from either side of the
  boundary, but the caller receives one normal proof.
- None of these rules depends on the illustrative width of 32. Production uses
  the same model with 256-entry tiles.
