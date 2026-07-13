// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "tiles_test_util.h"
#include "util.h"

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <merklecpp.h>
#include <merklecpp_tiles.h>
#include <string>

namespace fs = std::filesystem;

static void expect(bool cond, const std::string& what)
{
  if (!cond)
  {
    throw std::runtime_error("check failed: " + what);
  }
}

template <
  size_t HASH_SIZE,
  typename Tree,
  typename TiledTree,
  typename ProofEngine>
static void exercise_tiled_hash(const fs::path& prefix, const std::string& name)
{
  const auto hashes = make_hashesT<HASH_SIZE>(300);
  Tree reference;
  typename TiledTree::Config config;
  config.prefix = prefix;
  config.compact_on_flush = true;
  TiledTree tree(config);

  for (const auto& hash : hashes)
  {
    reference.insert(hash);
    tree.append(hash);
  }

  expect(tree.flush().full_written == 1, name + " full tile written");
  expect(tree.flushed_size() == 256, name + " flushed size");
  expect(tree.root() == reference.root(), name + " root");
  expect(
    fs::file_size(tree.store_ref().root() / "tile" / "0" / "000") ==
      (uintmax_t)merkle::tiles::TILE_WIDTH * HASH_SIZE,
    name + " tile byte size");

  const auto inclusion = tree.inclusion_proof(0, tree.size());
  expect(inclusion->verify(reference.root()), name + " inclusion proof");

  const auto consistency = tree.consistency_proof(256, tree.size());
  expect(
    ProofEngine::verify_consistency(
      256,
      tree.size(),
      *reference.past_root(255),
      reference.root(),
      consistency),
    name + " consistency proof");
}

int main()
{
  const TemporaryDirectory temporary_directory("merklecpp_tiles_hashes");
  const fs::path& base = temporary_directory.path();

  try
  {
    exercise_tiled_hash<
      48,
      merkle::Tree384,
      merkle::tiles::TiledTree384,
      merkle::tiles::ProofEngine384>(base / "sha384", "SHA384");
    exercise_tiled_hash<
      64,
      merkle::Tree512,
      merkle::tiles::TiledTree512,
      merkle::tiles::ProofEngine512>(base / "sha512", "SHA512");

    std::cout << "tiles_hashes: OK" << '\n';
  }
  catch (const std::exception& ex)
  {
    std::cout << "Error: " << ex.what() << '\n';
    return 1;
  }

  return 0;
}
