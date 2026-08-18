// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "tiles_test_util.h"
#include "util.h"

#include <filesystem>
#include <iostream>
#include <merklecpp_tiles.h>
#include <stdexcept>
#include <vector>

namespace fs = std::filesystem;
using merkle::Hash;
using merkle::tiles::TiledTree;

static void quick_start(
  const fs::path& storage_directory, const std::vector<Hash>& batch)
{
  /// SNIPPET_START: TiledTree-Quick-Start
  TiledTree::Config cfg;
  cfg.prefix = storage_directory;

  TiledTree log(cfg);
  for (const Hash& leaf : batch)
  {
    log.append(leaf);
  }

  // Persist newly-complete tiles to disk.
  log.flush();

  const auto n = log.size();
  if (n == 0)
  {
    throw std::runtime_error("expected at least one leaf");
  }
  const Hash root = log.root();

  // Inclusion proof for leaf 0 in the tree of n leaves.
  const auto inclusion = log.inclusion_proof(0, n);
  if (!inclusion->verify(root))
  {
    throw std::runtime_error("inclusion proof did not verify");
  }
  /// SNIPPET_END: TiledTree-Quick-Start
}

int main()
{
  try
  {
    const TemporaryDirectory temporary_directory("merklecpp_tiles_docs");
    quick_start(temporary_directory.path(), make_hashes(300));
    return 0;
  }
  catch (const std::exception& error)
  {
    std::cerr << "Error: " << error.what() << '\n';
    return 1;
  }
  catch (...)
  {
    std::cerr << "Error" << '\n';
    return 1;
  }
}
