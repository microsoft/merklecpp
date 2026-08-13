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
  TiledTree::Config config;
  config.prefix = storage_directory;
  config.compact_on_flush = true;

  TiledTree log(config);
  for (const auto& leaf_hash : batch)
  {
    log.append(leaf_hash);
  }

  log.flush();

  const auto root = log.root();
  const auto proof = log.inclusion_proof(0, log.size());
  if (!proof->verify(root))
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
