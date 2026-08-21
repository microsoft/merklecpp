// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "tiles_test_util.h"
#include "util.h"

#include <cstddef>
#include <filesystem>
#include <iostream>
#include <merklecpp.h>
#include <merklecpp_tiles.h>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using merkle::Hash;
using merkle::tiles::TileRef;

static void ccf_hash(const Hash& left, const Hash& right, Hash& out)
{
  merkle::Tree::hash_function(left, right, out);
}

using CcfTiledTree = merkle::tiles::TiledTreeT<Hash::size_bytes, ccf_hash>;
using SmallCcfTiledTree =
  merkle::tiles::TiledTreeT<Hash::size_bytes, ccf_hash, 2>;

static void expect(bool condition, const std::string& what)
{
  if (!condition)
  {
    throw std::runtime_error("check failed: " + what);
  }
}

template <typename Fn>
static void expect_throws(Fn&& fn, const std::string& what)
{
  try
  {
    std::forward<Fn>(fn)();
  }
  catch (const std::exception&)
  {
    return;
  }
  throw std::runtime_error("expected exception: " + what);
}

static std::vector<uint8_t> serialise(CcfTiledTree::Tree& tree)
{
  std::vector<uint8_t> bytes;
  tree.serialise(bytes);
  return bytes;
}

int main()
{
  const TemporaryDirectory temporary_directory("merklecpp_tiles_resume");
  const fs::path& base = temporary_directory.path();
  const auto hashes = make_hashes(1024);
  constexpr auto hash_namespace = "ccf-sha256";

  try
  {
    // Resume a compacted frontier, retain the trust boundary through a move,
    // and replace a correctly-sized but untrusted suffix tile.
    {
      CcfTiledTree::Config config;
      config.prefix = base / "round_trip";
      config.compact_on_flush = true;

      std::vector<uint8_t> serialised_tree;
      size_t full_tile_boundary = 0;
      Hash root_at_700;
      {
        CcfTiledTree source(config, hash_namespace);
        for (size_t i = 0; i < 700; i++)
        {
          source.append(hashes[i]);
        }
        source.flush();
        full_tile_boundary = source.flushed_size();
        expect(full_tile_boundary == 512, "source boundary");
        expect(source.tree_ref().min_index() == 511, "source compacted");
        root_at_700 = source.root();
        serialised_tree = serialise(source.tree_ref());

        source.store_ref().write_tile(
          TileRef{0, 2}, std::vector<Hash>(CcfTiledTree::TILE_WIDTH));
      }

      auto resumed = CcfTiledTree::resume(
        config, hash_namespace, serialised_tree, full_tile_boundary);
      expect(resumed.size() == 700, "resumed size");
      expect(resumed.root() == root_at_700, "resumed root");
      expect(resumed.flushed_size() == 512, "resumed flushed boundary");
      expect(resumed.immutable_size() == 512, "resumed immutable boundary");
      expect(resumed.flush().full_written == 0, "trusted prefix reused");
      expect(
        resumed.inclusion_proof(0, resumed.size())->verify(root_at_700),
        "resumed tiled proof");
      expect(
        resumed.inclusion_proof(699, resumed.size())->verify(root_at_700),
        "resumed frontier proof");

      CcfTiledTree moved(std::move(resumed));
      for (size_t i = 700; i < 768; i++)
      {
        moved.append(hashes[i]);
      }
      expect(moved.flush().full_written == 1, "untrusted tile replaced");
      expect(moved.flushed_size() == 768, "extended boundary");

      const std::vector<Hash> expected_tile(
        hashes.begin() + 512, hashes.begin() + 768);
      expect(
        moved.store_ref().read_tile(TileRef{0, 2}) == expected_tile,
        "replacement tile contents");

      CcfTiledTree::Tree reference;
      for (size_t i = 0; i < 768; i++)
      {
        reference.insert(hashes[i]);
      }
      const Hash expected_root = reference.root();
      expect(moved.root() == expected_root, "extended root");
      expect(
        moved.inclusion_proof(600, moved.size())->verify(expected_root),
        "extended proof");
    }

    // An existing empty namespace and an empty serialized tree can be resumed.
    std::vector<uint8_t> empty_tree;
    CcfTiledTree::Config empty_config;
    empty_config.prefix = base / "empty";
    {
      CcfTiledTree fresh(empty_config, hash_namespace);
      empty_tree = serialise(fresh.tree_ref());
    }
    {
      const auto empty =
        CcfTiledTree::resume(empty_config, hash_namespace, empty_tree, 0);
      expect(empty.size() == 0, "empty resumed size");
      expect(empty.flushed_size() == 0, "empty resumed boundary");
    }

    const fs::path missing_prefix = base / "missing";
    expect_throws(
      [&]() {
        CcfTiledTree::Config missing;
        missing.prefix = missing_prefix;
        (void)CcfTiledTree::resume(missing, hash_namespace, empty_tree, 0);
      },
      "missing namespace");
    expect(
      !fs::exists(missing_prefix), "failed resume does not create namespace");

    expect_throws(
      [&]() {
        auto trailing = empty_tree;
        trailing.push_back(0);
        (void)CcfTiledTree::resume(empty_config, hash_namespace, trailing, 0);
      },
      "trailing serialized bytes");

    expect_throws(
      [&]() {
        auto truncated = empty_tree;
        truncated.pop_back();
        (void)CcfTiledTree::resume(empty_config, hash_namespace, truncated, 0);
      },
      "truncated serialized tree");

    expect_throws(
      [&]() {
        std::vector<uint8_t> truncated_leaf;
        merkle::serialise_uint64_t(1, truncated_leaf);
        merkle::serialise_uint64_t(0, truncated_leaf);
        (void)CcfTiledTree::resume(
          empty_config, hash_namespace, truncated_leaf, 0);
      },
      "truncated serialized leaf");

    // A sub-tile frontier can resume at boundary zero and later publish its
    // first tile.
    {
      CcfTiledTree::Config config;
      config.prefix = base / "sub_tile";
      std::vector<uint8_t> serialised_tree;
      {
        CcfTiledTree source(config, hash_namespace);
        for (size_t i = 0; i < 100; i++)
        {
          source.append(hashes[i]);
        }
        serialised_tree = serialise(source.tree_ref());
      }

      auto resumed =
        CcfTiledTree::resume(config, hash_namespace, serialised_tree, 0);
      for (size_t i = 100; i < CcfTiledTree::TILE_WIDTH; i++)
      {
        resumed.append(hashes[i]);
      }
      expect(resumed.flush().full_written == 1, "first tile after resume");
      expect(
        resumed.flushed_size() == CcfTiledTree::TILE_WIDTH,
        "first resumed boundary");
    }

    // A compacted frontier can run without tiles while an independent writer
    // repairs the namespace. Once the repair is quiesced, adopting its complete
    // prefix enables proofs and normal incremental flushing.
    {
      SmallCcfTiledTree::Config config;
      config.prefix = base / "frontier_only";

      SmallCcfTiledTree::Tree frontier;
      for (size_t i = 0; i < 40; i++)
      {
        frontier.insert(hashes[i]);
      }
      const Hash frontier_root = frontier.root();
      frontier.flush_to(24);
      const auto serialised_frontier = serialise(frontier);

      auto restored = SmallCcfTiledTree::from_frontier(
        config, hash_namespace, serialised_frontier);
      expect(restored.size() == 40, "frontier-only size");
      expect(restored.root() == frontier_root, "frontier-only root");
      expect(restored.flushed_size() == 0, "frontier-only tile boundary");
      expect(
        restored.immutable_size() == 0, "frontier-only immutable boundary");
      expect(
        !fs::exists(config.prefix), "frontier-only restore performs no I/O");
      expect_throws(
        [&]() { (void)restored.inclusion_proof(0, restored.size()); },
        "proof before tile adoption");
      expect_throws(
        [&]() { (void)restored.flush(); }, "flush before tile repair");
      expect(
        restored.immutable_size() == 0,
        "failed frontier-only flush seals nothing");
      expect(
        !fs::exists(config.prefix),
        "failed frontier-only flush performs no I/O");

      SmallCcfTiledTree::Store repair_store(config.prefix, hash_namespace);
      expect_throws(
        [&]() { (void)SmallCcfTiledTree::Writer::repair(repair_store, 1); },
        "unaligned repair boundary");
      repair_store.write_tile(
        TileRef{0, 0}, std::vector<Hash>(SmallCcfTiledTree::TILE_WIDTH));

      const auto leaf_at = [&](uint64_t index) -> const Hash& {
        return hashes[static_cast<size_t>(index)];
      };
      {
        auto repair = SmallCcfTiledTree::Writer::repair(repair_store, 0);
        expect(
          repair.write_up_to(24, leaf_at).full_written == 7,
          "initial background repair");
      }
      const std::vector<Hash> first_tile(
        hashes.begin(), hashes.begin() + SmallCcfTiledTree::TILE_WIDTH);
      expect(
        repair_store.read_tile(TileRef{0, 0}) == first_tile,
        "repair replaces untrusted tile");
      expect_throws(
        [&]() { restored.adopt_tile_prefix(24); },
        "partial prefix does not reach resident frontier");

      {
        auto repair = SmallCcfTiledTree::Writer::repair(repair_store, 24);
        expect(
          repair.write_up_to(40, leaf_at).full_written == 5,
          "continued background repair");
      }
      repair_store.write_tile(
        TileRef{0, 10}, std::vector<Hash>(SmallCcfTiledTree::TILE_WIDTH));

      restored.adopt_tile_prefix(40);
      expect(restored.flushed_size() == 40, "adopted tile boundary");
      expect(restored.immutable_size() == 40, "adopted immutable boundary");
      expect(
        restored.inclusion_proof(0, restored.size())->verify(frontier_root),
        "proof after tile adoption");
      expect_throws(
        [&]() { restored.adopt_tile_prefix(36); },
        "tile adoption cannot regress");

      for (size_t i = 40; i < 44; i++)
      {
        restored.append(hashes[i]);
      }
      expect(
        restored.flush().full_written == 1,
        "normal flush replaces untrusted suffix");
      const std::vector<Hash> next_tile(
        hashes.begin() + 40, hashes.begin() + 44);
      expect(
        repair_store.read_tile(TileRef{0, 10}) == next_tile,
        "normal flush publishes authoritative suffix");
      const Hash extended_root = restored.root();
      expect(
        restored.inclusion_proof(0, restored.size())->verify(extended_root),
        "proof after resumed growth");
    }

    // A compacted tree must retain the configured overlap with its tile prefix.
    {
      CcfTiledTree::Config config;
      config.prefix = base / "coverage_gap";
      {
        CcfTiledTree source(config, hash_namespace);
        for (size_t i = 0; i < 600; i++)
        {
          source.append(hashes[i]);
        }
        source.flush();
      }

      CcfTiledTree::Tree compacted;
      for (size_t i = 0; i < 600; i++)
      {
        compacted.insert(hashes[i]);
      }
      compacted.flush_to(512);
      const auto compacted_tree = serialise(compacted);

      expect_throws(
        [&]() {
          (void)CcfTiledTree::resume(
            config, hash_namespace, compacted_tree, 512);
        },
        "missing boundary leaf");
      expect_throws(
        [&]() {
          (void)CcfTiledTree::resume(
            config, hash_namespace, compacted_tree, 513);
        },
        "unaligned boundary");
      expect_throws(
        [&]() {
          (void)CcfTiledTree::resume(
            config, hash_namespace, compacted_tree, 768);
        },
        "boundary beyond tree");
    }

    // Resume preserves the configured minimum resident range.
    {
      CcfTiledTree::Config config;
      config.prefix = base / "retention";
      config.compact_on_flush = true;
      std::vector<uint8_t> serialised_tree;
      {
        CcfTiledTree source(config, hash_namespace);
        for (size_t i = 0; i < 700; i++)
        {
          source.append(hashes[i]);
        }
        source.flush();
        serialised_tree = serialise(source.tree_ref());
      }

      config.retention_margin = 300;
      expect_throws(
        [&]() {
          (void)CcfTiledTree::resume(
            config, hash_namespace, serialised_tree, 512);
        },
        "retention margin");
    }

    // Any state retaining the configured margin is valid, even when its
    // compaction point is not tile-aligned.
    {
      CcfTiledTree::Config config;
      config.prefix = base / "exact_retention";
      config.retention_margin = 300;
      {
        CcfTiledTree source(config, hash_namespace);
        for (size_t i = 0; i < 900; i++)
        {
          source.append(hashes[i]);
        }
        source.flush();
      }

      CcfTiledTree::Tree compacted;
      for (size_t i = 0; i < 900; i++)
      {
        compacted.insert(hashes[i]);
      }
      compacted.flush_to(468);
      const auto serialised_tree = serialise(compacted);
      auto resumed =
        CcfTiledTree::resume(config, hash_namespace, serialised_tree, 768);
      expect(resumed.tree_ref().min_index() == 468, "exact retention accepted");
    }

    // The trusted boundary applies independently at every tile level.
    {
      SmallCcfTiledTree::Config config;
      config.prefix = base / "all_levels";
      std::vector<uint8_t> serialised_tree;
      {
        SmallCcfTiledTree source(config, hash_namespace);
        for (size_t i = 0; i < 24; i++)
        {
          source.append(hashes[i]);
        }
        source.flush();
        serialised_tree = serialise(source.tree_ref());

        for (uint64_t index = 6; index < 8; index++)
        {
          source.store_ref().write_tile(
            TileRef{0, index},
            std::vector<Hash>(SmallCcfTiledTree::TILE_WIDTH));
        }
        source.store_ref().write_tile(
          TileRef{1, 1}, std::vector<Hash>(SmallCcfTiledTree::TILE_WIDTH));
      }

      auto resumed =
        SmallCcfTiledTree::resume(config, hash_namespace, serialised_tree, 24);
      for (size_t i = 24; i < 32; i++)
      {
        resumed.append(hashes[i]);
      }
      expect(
        resumed.flush().full_written == 3,
        "untrusted tiles replaced at every level");

      const std::vector<Hash> expected(
        hashes.begin() + 24, hashes.begin() + 28);
      expect(
        resumed.store_ref().read_tile(TileRef{0, 6}) == expected,
        "level-0 suffix replaced");
    }

    // The overlapping boundary leaf catches a mismatched tile namespace.
    {
      CcfTiledTree::Config config;
      config.prefix = base / "mismatch";
      std::vector<uint8_t> serialised_tree;
      {
        CcfTiledTree source(config, hash_namespace);
        for (size_t i = 0; i < 600; i++)
        {
          source.append(hashes[i]);
        }
        source.flush();
        serialised_tree = serialise(source.tree_ref());
        source.store_ref().write_tile(
          TileRef{0, 1}, std::vector<Hash>(CcfTiledTree::TILE_WIDTH));
      }

      expect_throws(
        [&]() {
          (void)CcfTiledTree::resume(
            config, hash_namespace, serialised_tree, 512);
        },
        "mismatched boundary tile");
    }

    std::cout << "tiles_resume: OK" << '\n';
  }
  catch (const std::exception& error)
  {
    std::cout << "Error: " << error.what() << '\n';
    return 1;
  }

  return 0;
}
