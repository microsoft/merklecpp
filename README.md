[![Continuous Integration](https://github.com/microsoft/merklecpp/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/microsoft/merklecpp/actions/workflows/ci.yml)
[![Documentation](https://github.com/microsoft/merklecpp/actions/workflows/build-docs.yml/badge.svg?branch=main)](https://github.com/microsoft/merklecpp/actions/workflows/build-docs.yml)

# merklecpp

A header-only C++ library for creation and manipulation of Merkle trees. It supports the usual
operations, like hash insertion, root computation, and path extraction, as well as some more
unusual features like flushing, retracting, and tree segment serialisation.

## Usage

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
tree, or from a combination of the two. The hashing is unchanged: tiles and tile-derived proofs are templated on
the tree's existing hash function, so a tile-derived inclusion proof is
byte-identical to one from `merkle::Tree::path()` and verifies with the same
`merkle::Path::verify()`.

    #include <merklecpp_tiles.h>

    merkle::tiles::TiledTree::Config cfg;
    cfg.prefix = "/var/log/mylog";       // tile files and checkpoint live here
    cfg.retention_margin = 1024;         // keep the most recent leaves in memory
    cfg.compact_on_checkpoint = true;    // opt in to dropping already-tiled leaves

    merkle::tiles::TiledTree log(cfg);
    for (const auto& leaf_hash : batch)
      log.append(leaf_hash);

    // Write newly-complete tiles (and a checkpoint). With compaction enabled
    // this also drops from memory the leaves already covered by a full tile;
    // otherwise the tree keeps every leaf and you can call log.compact() later.
    log.checkpoint();

    // Proofs are served from tiles + the resident tree, even for flushed leaves.
    auto inclusion = log.inclusion_proof(/*index=*/0, log.size());
    assert(inclusion->verify(log.root()));

    auto consistency = log.consistency_proof(/*m=*/100, /*n=*/log.size());

See [doc/design/tlog-tiles.md](doc/design/tlog-tiles.md) for the full design,
file/directory layout, and the proof algorithms.


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
