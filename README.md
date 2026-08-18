[![Continuous Integration](https://github.com/microsoft/merklecpp/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/microsoft/merklecpp/actions/workflows/ci.yml)
[![Documentation](https://github.com/microsoft/merklecpp/actions/workflows/build-docs.yml/badge.svg?branch=main)](https://github.com/microsoft/merklecpp/actions/workflows/build-docs.yml)

# merklecpp

A header-only C++ library for creation and manipulation of Merkle trees. It supports the usual
operations, like hash insertion, root computation, and path extraction, as well as some more
unusual features like flushing, retracting, and tree segment serialisation.

## Usage

merklecpp requires C++20.

    #include <merklecpp.h>

    merkle::Tree::Hash hash("fa8f44eabb728d4020e7f33d1aa973faaef19de6c06679bccdc5100a3c01f54a");

    merkle::Tree tree;
    tree.insert(hash);
    ...
    auto root = tree.root();
    auto path = tree.path(0);
    assert(path->verify(root));


## Tiled storage (tlog-tiles)

The companion header `merklecpp_tiles.h` adds optional, header-only support for
persisting a tree as [tlog-tiles](https://c2sp.org/tlog-tiles) tile files
*progressively* (optionally dropping already-tiled leaves from memory) and for
retrieving inclusion and consistency proofs from those tiles, from the in-memory
tree, or from a combination of the two. The hashing is unchanged: tiles and
tile-derived proofs are templated on the tree's existing hash function, so a
tile-derived inclusion proof is byte-identical to one from
`merkle::Tree::path()` and verifies with the same `merkle::Path::verify()`.

    #include <merklecpp_tiles.h>

    merkle::tiles::TiledTree::Config cfg;
    cfg.prefix = "/var/log/mylog";       // tile files live here
    cfg.retention_margin = 1024;         // retain at least 1024 tiled leaves too
    cfg.compact_on_flush = true;         // opt in to dropping already-tiled leaves

    merkle::tiles::TiledTree log(cfg);
    for (const auto& leaf_hash : batch)
      log.append(leaf_hash);

    // Write newly-complete tiles. With compaction enabled
    // this also drops from memory the leaves already covered by a full tile;
    // otherwise the tree keeps every leaf and you can call log.compact() later.
    log.flush();

    // Proofs are served from tiles + the resident tree, even for flushed leaves.
    assert(log.size() > 0);
    auto inclusion = log.inclusion_proof(/*index=*/0, log.size());
    assert(inclusion->verify(log.root()));

    if (log.size() > 1)
    {
      auto consistency =
        log.consistency_proof(/*m=*/log.size() / 2, /*n=*/log.size());
    }

`TiledTree` creates a new tiled tree. The configured prefix may exist, but the
default alias requires `<prefix>/sha256-256w/tile` not to exist, even as an
empty directory. Construction atomically claims that tile namespace and rejects
an existing one because tile files alone do not identify or restore the tree
that produced them. Applications with externally persisted tree state can use
the lower-level `TileStore` and `TileWriter` APIs to resume a store.

See the [tiled storage guide](doc/tiles-guide.rst) for a how-to covering
flushing, compaction, rollback, proofs, and the lower-level building blocks,
and the [illustrated walkthrough](doc/tiles-illustrated.rst) for the tile layout
and proof algorithms.


## Building and testing

Tests are built by default. Configure, build, and run them with:

    cmake -S . -B build
    cmake --build build
    cmake -E chdir build ctest

Some tile coverage is intentionally long-running. `LONG_TESTS` is off by
default for local builds; turn it on when you want the full tile stress suite,
including level-2 tile coverage and tile proof timing:

    cmake -S . -B build -DLONG_TESTS=ON

CI enables `LONG_TESTS` in Release configurations so pull requests exercise the
full tiled-storage matrix; Debug configurations run the short suite. Every
Release job publishes its `time_tiles` measurements as a table in the GitHub
Actions job summary.

| CMake option | Default | Purpose |
|---|---:|---|
| `BUILD_TESTING` | `ON` | Build tests; set `OFF` for a library-only build |
| `LONG_TESTS` | `OFF` | Include level-2 tile and `time_tiles` coverage |
| `OPENSSL` | `OFF` | Enable OpenSSL hashes, SHA-384/512 tiled aliases, and tests |
| `CLANG_TIDY` | `OFF` | Run clang-tidy while compiling tests |
| `TRACE` | `OFF` | Enable internal Merkle-tree trace output |
| `PROFILE` | `OFF` | Add profiling flags to test targets |


## Contributing

This project welcomes contributions and suggestions.  Most contributions require you to agree to a
Contributor License Agreement (CLA) declaring that you have the right to, and actually do, grant us
the rights to use your contribution. For details, visit https://cla.opensource.microsoft.com.

When you submit a pull request, a CLA bot will automatically determine whether you need to provide
a CLA and decorate the PR appropriately (e.g., status check, comment). Simply follow the instructions
provided by the bot. You will only need to do this once across all repos using our CLA.

This project has adopted the [Microsoft Open Source Code of Conduct](https://opensource.microsoft.com/codeofconduct/).
For more information see the [Code of Conduct FAQ](https://opensource.microsoft.com/codeofconduct/faq/) or
contact [opencode@microsoft.com](mailto:opencode@microsoft.com) with any additional questions or comments.

## Trademarks

This project may contain trademarks or logos for projects, products, or services. Authorized use of Microsoft
trademarks or logos is subject to and must follow
[Microsoft's Trademark & Brand Guidelines](https://www.microsoft.com/en-us/legal/intellectualproperty/trademarks/usage/general).
Use of Microsoft trademarks or logos in modified versions of this project must not cause confusion or imply Microsoft sponsorship.
Any use of third-party trademarks or logos are subject to those third-party's policies.
