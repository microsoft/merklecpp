# Merkle proof atlas

This exploratory tool visualizes which subtree roots an inclusion or consistency
proof resolves from durable tiles and which it resolves from the resident
in-memory frontier.

The C++ harness uses the real `TileWriter`, hash sources, and `ProofEngine`. Thin
wrappers record each `subtree_root(level, index)` attempt without changing its
result. Inclusion proofs are checked against `Tree::path()`, and every traced
proof is compared with an unwrapped control proof before data is emitted.
Consistency scenes call `consistency_proof_from_indices()` with explicit first
and second leaf indices, including historical pairs that end before the backing
tree's current size.
The canvas marks those two last leaves directly as A (circle) and B (diamond);
the red subtree nodes remain the actual RFC 6962 proof components between the
tree states ending at A and B.

## Run

From the repository root:

```sh
mkdir -p build/proof-viz
c++ -std=c++20 -O2 -I. -Itest \
  tools/proof-viz/proof_viz.cpp -o build/proof-viz/proof_viz
build/proof-viz/proof_viz tools/proof-viz/data.js
python3 -m http.server 4173 --directory tools/proof-viz
```

Open <http://localhost:4173/>. The generated `data.js` is intentionally ignored.

Green nodes are tile-backed, blue nodes are resident in memory, and red marks
the proof route and proof components. Amber outlines and the resolver timeline
come from the calls observed by the C++ harness; the browser derives only the
static drawing geometry.