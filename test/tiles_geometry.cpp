// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN

#include "tiles_test_util.h"
#include "util.h"

#include <cstdint>
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <merklecpp.h>
#include <merklecpp_tiles.h>
#include <string>
#include <type_traits>
#include <vector>

namespace fs = std::filesystem;
using merkle::Hash;
using merkle::tiles::CombinedHashSource;
using merkle::tiles::MemoryHashSource;
using merkle::tiles::ProofEngine;
using merkle::tiles::TileRef;

static constexpr uint8_t SMALL_TILE_HEIGHT = 2;
using SmallStore = merkle::tiles::TileStoreT<
  Hash::size_bytes,
  merkle::Tree::hash_function,
  SMALL_TILE_HEIGHT>;
using SmallWriter = merkle::tiles::TileWriterT<
  Hash::size_bytes,
  merkle::Tree::hash_function,
  SMALL_TILE_HEIGHT>;
using SmallTileHashSource = merkle::tiles::TileHashSourceT<
  Hash::size_bytes,
  merkle::Tree::hash_function,
  SMALL_TILE_HEIGHT>;
using SmallTiledTree = merkle::tiles::TiledTreeT<
  Hash::size_bytes,
  merkle::Tree::hash_function,
  SMALL_TILE_HEIGHT>;
using SmallEntryBundleWriter = merkle::tiles::EntryBundleWriterT<
  Hash::size_bytes,
  merkle::Tree::hash_function,
  SMALL_TILE_HEIGHT>;

static_assert(merkle::tiles::TileStore::TILE_HEIGHT == 8);
static_assert(merkle::tiles::TileStore::TILE_WIDTH == 256);
static_assert(SmallStore::TILE_HEIGHT == SMALL_TILE_HEIGHT);
static_assert(SmallStore::TILE_WIDTH == 4);
static_assert(std::is_same_v<SmallWriter::Store, SmallStore>);
static_assert(std::is_same_v<SmallTileHashSource::Store, SmallStore>);
static_assert(std::is_same_v<SmallTiledTree::Store, SmallStore>);
static_assert(std::is_same_v<SmallEntryBundleWriter::Store, SmallStore>);

static constexpr uint64_t SMALL_LEVEL1_SIZE =
  SmallStore::TILE_WIDTH * SmallStore::TILE_WIDTH;
static constexpr uint64_t SMALL_LEVEL2_SIZE =
  SMALL_LEVEL1_SIZE * SmallStore::TILE_WIDTH;
static constexpr uint64_t SMALL_LEVEL2_TILE_COUNT =
  SMALL_LEVEL1_SIZE + SmallStore::TILE_WIDTH + 1;

static void overwrite_file(const fs::path& path)
{
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  const std::vector<uint8_t> bytes = {0x00, 0x05, 0x01};
  file.write(
    reinterpret_cast<const char*>(bytes.data()),
    static_cast<std::streamsize>(bytes.size()));
}

static void check_proofs(
  const fs::path& prefix,
  uint64_t size,
  const std::vector<Hash>& hashes)
{
  SmallStore store(prefix);
  SmallWriter writer(store);
  const auto leaf_at = [&](uint64_t index) -> const Hash& {
    return hashes[index];
  };
  const auto stats = writer.write_up_to(size, leaf_at);

  merkle::Tree reference;
  merkle::Tree frontier;
  for (uint64_t index = 0; index < size; index++)
  {
    reference.insert(hashes[index]);
    frontier.insert(hashes[index]);
  }

  const uint64_t covered =
    (size / SmallStore::TILE_WIDTH) * SmallStore::TILE_WIDTH;
  uint64_t drop_to = covered;
  if (drop_to >= size)
  {
    drop_to = size - 1;
  }
  if (drop_to > 0)
  {
    frontier.flush_to(static_cast<size_t>(drop_to));
  }

  const MemoryHashSource memory_source(frontier);
  const SmallTileHashSource tile_source(store, covered);
  const CombinedHashSource source(memory_source, tile_source);
  const ProofEngine engine(source);
  const Hash expected_root = reference.root();

  CHECK(engine.root(size) == expected_root);
  for (const uint64_t index : {uint64_t{0}, size - 1})
  {
    const auto proof = engine.inclusion_proof(index, size);
    CHECK(*proof == *reference.path(static_cast<size_t>(index)));
    CHECK(proof->verify(expected_root));
  }

  if (size > 1)
  {
    const uint64_t first_size = size / 2;
    const auto proof = engine.consistency_proof(first_size, size);
    CHECK(ProofEngine::verify_consistency(
      first_size,
      size,
      *reference.past_root(static_cast<size_t>(first_size - 1)),
      expected_root,
      proof));
  }

  if (size == SMALL_LEVEL2_SIZE)
  {
    CHECK(stats.full_written == SMALL_LEVEL2_TILE_COUNT);
    CHECK(store.has_full_tile(2, 0));
    CHECK_FALSE(store.has_full_tile(3, 0));
    const auto level2 = store.read_tile(TileRef{2, 0});
    CHECK(level2.size() == SmallStore::TILE_WIDTH);
  }
}

TEST_CASE("Compile-time tile geometry isolates storage formats")
{
  const TemporaryDirectory temporary_directory;
  const fs::path& prefix = temporary_directory.path();
  const merkle::tiles::TileStore default_store(prefix);
  SmallStore small_store(prefix);

  CHECK(
    default_store.root().lexically_relative(prefix).generic_string() ==
    "sha256-256w");
  CHECK(
    small_store.root().lexically_relative(prefix).generic_string() ==
    "sha256-4w");
  CHECK(default_store.root() != small_store.root());

  const auto full = make_hashes(SmallStore::TILE_WIDTH);
  small_store.write_tile(TileRef{0, 0}, full);
  CHECK(small_store.has_full_tile(0, 0));
  CHECK(small_store.read_tile(TileRef{0, 0}) == full);
  CHECK(
    fs::file_size(small_store.tile_path(TileRef{0, 0})) ==
    SmallStore::TILE_WIDTH * Hash::size_bytes);
  CHECK_FALSE(default_store.has_full_tile(0, 0));

  const std::vector<Hash> wrong_width(full.begin(), full.end() - 1);
  CHECK_THROWS_AS(
    (small_store.write_tile(TileRef{0, 1}, wrong_width)),
    std::runtime_error);
}

TEST_CASE("Alternate geometry writes and resolves every tile level")
{
  const TemporaryDirectory temporary_directory;
  const auto hashes = make_hashes(SMALL_LEVEL2_SIZE + 1);

  for (const uint64_t size :
       {SmallStore::TILE_WIDTH - 1,
        SmallStore::TILE_WIDTH,
        SmallStore::TILE_WIDTH + 1,
        SMALL_LEVEL1_SIZE,
        SMALL_LEVEL1_SIZE + 1,
        SMALL_LEVEL2_SIZE,
        SMALL_LEVEL2_SIZE + 1})
  {
    CAPTURE(size);
    check_proofs(
      temporary_directory.path() / ("n" + std::to_string(size)), size, hashes);
  }
}

TEST_CASE("Alternate geometry controls entry bundle boundaries and recovery")
{
  const TemporaryDirectory temporary_directory;
  SmallStore store(temporary_directory.path());
  SmallEntryBundleWriter writer(store);
  const auto hashes = make_hashes(10);
  const auto entry_at = [&](uint64_t index) -> std::vector<uint8_t> {
    return hashes[index];
  };

  CHECK(writer.write_up_to(10, entry_at).full_written == 2);
  CHECK(store.has_entry_bundle(0));
  CHECK(store.has_entry_bundle(1));
  CHECK_FALSE(store.has_entry_bundle(2));
  CHECK(store.read_entry_bundle(0).size() == SmallStore::TILE_WIDTH);

  overwrite_file(store.entries_path(0));
  CHECK_FALSE(store.has_entry_bundle(0));
  SmallEntryBundleWriter resumed(store);
  CHECK(resumed.write_up_to(10, entry_at).full_written == 1);
  CHECK(store.has_entry_bundle(0));
  CHECK(store.has_entry_bundle(1));

  const std::vector<std::vector<uint8_t>> wrong_width(
    SmallStore::TILE_WIDTH - 1, {0x42});
  CHECK_THROWS_AS(
    (store.write_entry_bundle(3, wrong_width)), std::runtime_error);
}

TEST_CASE("Alternate geometry drives tiled tree lifecycle boundaries")
{
  const TemporaryDirectory temporary_directory;
  const auto hashes = make_hashes(15);
  SmallTiledTree::Config config;
  config.prefix = temporary_directory.path() / "tree";
  config.retention_margin = 5;
  config.compact_on_flush = true;
  SmallTiledTree tree(config);
  merkle::Tree reference;

  for (const auto& hash : hashes)
  {
    tree.append(hash);
    reference.insert(hash);
  }

  CHECK(tree.flush().full_written == 3);
  CHECK(tree.flushed_size() == 12);
  CHECK(tree.immutable_size() == 12);
  CHECK(tree.tree_ref().min_index() == 4);
  CHECK(tree.root() == reference.root());
  CHECK(*tree.inclusion_proof(0, tree.size()) == *reference.path(0));
  CHECK_THROWS_AS(tree.retract_to(10), std::runtime_error);
  CHECK_NOTHROW(tree.retract_to(11));
  CHECK(tree.size() == 12);
}

TEST_CASE("Alternate geometry preserves interrupted flush seals")
{
  const TemporaryDirectory temporary_directory;
  const auto hashes = make_hashes(8);
  SmallTiledTree::Config config;
  config.prefix = temporary_directory.path() / "interrupted";
  SmallTiledTree tree(config);
  for (const auto& hash : hashes)
  {
    tree.append(hash);
  }

  const fs::path blocker = tree.store_ref().tile_path(TileRef{0, 1});
  fs::create_directories(blocker);
  CHECK_THROWS_AS(tree.flush(), std::runtime_error);
  CHECK(tree.store_ref().has_full_tile(0, 0));
  CHECK(tree.flushed_size() == 0);
  CHECK(tree.immutable_size() == 8);
  CHECK_THROWS_AS(tree.retract_to(0), std::runtime_error);

  fs::remove(blocker);
  CHECK(tree.flush().full_written == 1);
  CHECK(tree.flushed_size() == 8);
  CHECK(tree.immutable_size() == 8);
}
