// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "tiles_test_util.h"
#include "util.h"

#include <cstddef>
#include <filesystem>
#include <iostream>
#include <limits>
#include <merklecpp.h>
#include <merklecpp_tiles.h>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using merkle::Hash;
using merkle::tiles::ProofEngine;
using merkle::tiles::TiledTree;
using merkle::tiles::TileStore;

static void custom_hash(const Hash& left, const Hash& right, Hash& out)
{
  merkle::Tree::hash_function(left, right, out);
}

using CustomTiledTree =
  merkle::tiles::TiledTreeT<Hash::size_bytes, custom_hash>;

static_assert(!std::is_copy_constructible_v<TiledTree>);
static_assert(!std::is_copy_assignable_v<TiledTree>);
static_assert(std::is_nothrow_move_constructible_v<TiledTree>);
static_assert(!std::is_move_assignable_v<TiledTree>);

static void expect(bool cond, const std::string& what)
{
  if (!cond)
  {
    throw std::runtime_error("check failed: " + what);
  }
}

static fs::path store_root(const fs::path& prefix)
{
  return prefix / TileStore::storage_directory_name("sha256");
}

int main()
{
  const TemporaryDirectory temporary_directory("merklecpp_tiles_tree");
  const fs::path& base = temporary_directory.path();

  try
  {
    const auto hashes = make_hashes(1500);

    // ---- Part 0: the empty tree. flush()/compact() are no-ops and there is no
    //      root.
    {
      TiledTree::Config ecfg;
      ecfg.prefix = base / "tt_empty";
      TiledTree ett(ecfg);
      expect(ett.size() == 0, "empty: size 0");
      expect(ett.flush().full_written == 0, "empty: flush writes nothing");
      expect(ett.flushed_size() == 0, "empty: flushed size 0");
      expect(ett.immutable_size() == 0, "empty: immutable size 0");
      expect(ett.compact() == 0, "empty: compact no-op");
      bool ethrew = false;
      try
      {
        (void)ett.root();
      }
      catch (const std::exception&)
      {
        ethrew = true;
      }
      expect(ethrew, "empty: root throws");
      std::cout << "empty tree: OK" << '\n';
    }

    // ---- Part 0b: moving a tree rebinds its writer to the destination store.
    {
      const fs::path previous_cwd = fs::current_path();
      try
      {
        fs::create_directories(base);
        fs::current_path(base);
        TiledTree::Config move_cfg;
        move_cfg.prefix = "tt_move_expected";
        const fs::path expected_prefix = fs::absolute(move_cfg.prefix);
        TiledTree source(move_cfg);
        for (size_t i = 0; i < 300; i++)
        {
          source.append(hashes[i]);
        }
        source.flush();

        const fs::path work = base / "tt_move_cwd";
        fs::create_directories(work);
        fs::current_path(work);
        TiledTree moved(std::move(source));
        expect(moved.flushed_size() == 256, "move: flushed size retained");
        expect(moved.immutable_size() == 256, "move: immutable size retained");
        expect(
          moved.store_ref().root() == store_root(expected_prefix),
          "move: destination store retained");
        for (size_t i = 300; i < 512; i++)
        {
          moved.append(hashes[i]);
        }
        expect(moved.flush().full_written == 1, "move: next tile written");
        expect(
          fs::is_regular_file(store_root(expected_prefix) / "tile/0/001"),
          "move: tile written to configured store");
        expect(
          !fs::exists(store_root(work) / "tile/0/001"),
          "move: no tile written relative to current directory");
      }
      catch (...)
      {
        fs::current_path(previous_cwd);
        throw;
      }
      fs::current_path(previous_cwd);
      std::cout << "move construction: OK" << '\n';
    }

    // ---- Part 0c: a TiledTree atomically claims a fresh namespace, so another
    //      tree cannot pass construction before the first one writes tiles.
    {
      TiledTree::Config existing_cfg;
      existing_cfg.prefix = base / "tt_existing";
      TiledTree first(existing_cfg);

      bool construction_threw = false;
      try
      {
        const TiledTree second(existing_cfg);
      }
      catch (const std::exception&)
      {
        construction_threw = true;
      }
      expect(
        construction_threw,
        "existing prefix: second tree rejects claimed namespace");
      for (size_t i = 0; i < 256; i++)
      {
        first.append(hashes[i]);
      }
      expect(
        first.flush().full_written == 1,
        "existing prefix: owning tree writes tile");
      std::cout << "exclusive tile namespace: OK" << '\n';
    }

    // ---- Part 0d: generic tiled trees accept the explicit storage namespace
    //      required by custom hash functions.
    {
      CustomTiledTree::Config custom_cfg;
      custom_cfg.prefix = base / "tt_custom";
      CustomTiledTree custom(custom_cfg, "custom-sha256");
      merkle::Tree reference;
      for (size_t i = 0; i < 300; i++)
      {
        custom.append(hashes[i]);
        reference.insert(hashes[i]);
      }
      expect(
        custom.flush().full_written == 1, "custom hash: full tile written");
      expect(custom.root() == reference.root(), "custom hash: root matches");
      expect(
        custom.store_ref().root() ==
          fs::absolute(custom_cfg.prefix) /
            TileStore::storage_directory_name("custom-sha256"),
        "custom hash: explicit namespace used");
      std::cout << "custom hash namespace: OK" << '\n';
    }

    // ---- Part 0e: proof requests cannot describe a state beyond the current
    //      tree, even when the lower proof engine would not read its source.
    {
      TiledTree::Config bounds_cfg;
      bounds_cfg.prefix = base / "tt_bounds";
      TiledTree bounded(bounds_cfg);
      bounded.append(hashes[0]);

      bool threw = false;
      try
      {
        (void)bounded.consistency_proof(2, 2);
      }
      catch (const std::exception&)
      {
        threw = true;
      }
      expect(threw, "proof bounds: reject future equal sizes");

      threw = false;
      try
      {
        (void)bounded.inclusion_proof(0, 2);
      }
      catch (const std::exception&)
      {
        threw = true;
      }
      expect(threw, "proof bounds: reject future inclusion size");

      threw = false;
      try
      {
        const auto maximum = std::numeric_limits<size_t>::max();
        (void)bounded.consistency_proof_from_indices(maximum, maximum);
      }
      catch (const std::exception&)
      {
        threw = true;
      }
      expect(threw, "proof bounds: reject overflowing indices");

      bounded.append(hashes[1]);
      threw = false;
      try
      {
        (void)bounded.consistency_proof_from_indices(1, 0);
      }
      catch (const std::runtime_error& error)
      {
        threw = std::string(error.what()) ==
          "first consistency proof index exceeds second index";
      }
      expect(threw, "proof bounds: reject reversed indices clearly");
      std::cout << "proof bounds: OK" << '\n';
    }

    {
      TiledTree::Config missing_cfg;
      missing_cfg.prefix = base / "tt_missing_compacted_tile";
      missing_cfg.compact_on_flush = true;
      TiledTree compacted(missing_cfg);
      for (size_t i = 0; i < 300; i++)
      {
        compacted.append(hashes[i]);
      }
      compacted.flush();
      expect(
        compacted.tree_ref().min_index() > 0,
        "missing tile: old leaves compacted");
      fs::remove(store_root(missing_cfg.prefix) / "tile/0/000");

      TiledTree moved(std::move(compacted));
      bool threw_clear_error = false;
      try
      {
        moved.flush();
      }
      catch (const std::runtime_error& error)
      {
        threw_clear_error =
          std::string(error.what()).find("non-resident leaf") !=
          std::string::npos;
      }
      expect(
        threw_clear_error,
        "missing tile: regeneration reports non-resident leaf");
      std::cout << "missing compacted tile: OK" << '\n';
    }

    // ---- Part 2: TiledTree flush, proofs over tiles + memory.
    const size_t n1 = 1000; // first flush size
    const size_t N = 1500; // final size

    // Reference: a plain (never flushed) tree with the same leaves.
    merkle::Tree ref;
    for (size_t i = 0; i < N; i++)
    {
      ref.insert(hashes[i]);
    }
    const Hash ref_root = ref.root();

    // Default mode: flush() writes tiles but drops nothing from memory;
    // an explicit compact() drops only the leaves already covered by a tile.
    {
      TiledTree::Config dcfg;
      dcfg.prefix = base / "tt_default";
      TiledTree dtt(dcfg);
      for (size_t i = 0; i < n1; i++)
      {
        dtt.append(hashes[i]);
      }
      dtt.flush();
      expect(dtt.tree_ref().min_index() == 0, "default: nothing dropped");
      dtt.compact();
      expect(
        dtt.tree_ref().min_index() == 767, "compact() keeps boundary leaf");
    }

    TiledTree::Config cfg;
    cfg.prefix = base / "tt";
    cfg.retention_margin = 0;
    cfg.compact_on_flush = true;
    TiledTree tt(cfg);

    for (size_t i = 0; i < n1; i++)
    {
      tt.append(hashes[i]);
    }
    tt.flush(); // flushes full tiles; flushed_size = covered = 768
    expect(tt.flushed_size() == 768, "flushed size");
    expect(tt.tree_ref().min_index() == 767, "compacted with boundary overlap");

    for (size_t i = n1; i < N; i++)
    {
      tt.append(hashes[i]);
    }
    expect(tt.size() == N, "size after appends");
    expect(tt.root() == ref_root, "tiled root == reference root");

    // Indices that are: flushed (tiles only), in the flushed-but-resident
    // overlap, and on the un-flushed resident frontier.
    for (const size_t i :
         {(size_t)0,
          (size_t)767,
          (size_t)800,
          (size_t)999,
          (size_t)1000,
          (size_t)1499})
    {
      const auto p = tt.inclusion_proof(i, N);
      expect(
        *p == *ref.path(i), "combined inclusion==ref i=" + std::to_string(i));
      expect(
        p->verify(ref_root),
        "combined inclusion verify i=" + std::to_string(i));
    }

    // The resident tree alone cannot prove a flushed index.
    bool threw = false;
    try
    {
      (void)tt.tree_ref().path(0);
    }
    catch (const std::exception&)
    {
      threw = true;
    }
    expect(threw, "memory-only path throws for flushed index");

    // Consistency across the flush boundary: flushed size -> current size,
    // and a flushed-era size -> current size.
    {
      const auto cp = tt.consistency_proof(n1, N);
      expect(
        ProofEngine::verify_consistency(
          n1, N, *ref.past_root(n1 - 1), ref_root, cp),
        "consistency n1->N across flush");

      const size_t m = 500; // flushed era
      const auto cp2 = tt.consistency_proof(m, N);
      expect(
        ProofEngine::verify_consistency(
          m, N, *ref.past_root(m - 1), ref_root, cp2),
        "consistency m->N across flush");

      // Index-based variant: consistency_proof_from_indices(i, j) ==
      // consistency_proof(i + 1, j + 1), spanning tiles and the live tree.
      const auto cpi = tt.consistency_proof_from_indices(m - 1, N - 1);
      expect(cpi == cp2, "consistency index variant across flush");
      expect(
        ProofEngine::verify_consistency(
          m, N, *ref.past_root(m - 1), ref_root, cpi),
        "consistency index variant verifies");
    }

    // A second flush writes further tiles; proofs for now-flushed indices still
    // work (this also confirms the writer never reads a flushed leaf).
    tt.flush(); // flushes full tiles; flushed_size = covered = 1280
    expect(tt.flushed_size() == 1280, "second flushed size");
    expect(tt.tree_ref().min_index() == 1279, "second boundary leaf retained");

    for (const size_t i :
         {(size_t)0,
          (size_t)1000,
          (size_t)1279,
          (size_t)1280,
          (size_t)1499})
    {
      const auto p = tt.inclusion_proof(i, N);
      expect(
        *p == *ref.path(i), "post-2nd inclusion==ref i=" + std::to_string(i));
      expect(
        p->verify(ref_root),
        "post-2nd inclusion verify i=" + std::to_string(i));
    }

    std::cout << "tiled tree (flush + combination): OK" << '\n';

    // ---- Part 2b: compaction when the size is an exact multiple of TILE_WIDTH
    //      (here 512). flush_to cannot drain the whole tree, so compact() keeps
    //      one resident leaf rather than throwing; proofs still resolve.
    {
      TiledTree::Config mcfg;
      mcfg.prefix = base / "tt_multiple";
      mcfg.compact_on_flush = true;
      TiledTree mtt(mcfg);
      for (size_t i = 0; i < 512; i++)
      {
        mtt.append(hashes[i]);
      }
      mtt.flush(); // covered == size == 512; compaction must not throw
      expect(mtt.flushed_size() == 512, "multiple: flushed size 512");
      expect(mtt.tree_ref().min_index() == 511, "multiple: one leaf retained");

      merkle::Tree mref;
      for (size_t i = 0; i < 512; i++)
      {
        mref.insert(hashes[i]);
      }
      const Hash mroot = mref.root();
      expect(mtt.root() == mroot, "multiple: root matches reference");
      for (const size_t i : {(size_t)0, (size_t)256, (size_t)511})
      {
        const auto p = mtt.inclusion_proof(i, 512);
        expect(*p == *mref.path(i), "multiple: inclusion==ref");
        expect(p->verify(mroot), "multiple: inclusion verify");
      }
      std::cout << "tiled tree (exact-multiple compaction): OK" << '\n';
    }

    // ---- Part 2c: compaction with a non-zero retention_margin keeps the most
    //      recent leaves resident while flushed indices are still served from
    //      tiles. The immutable prefix remains the full-tile prefix, regardless
    //      of the margin.
    {
      TiledTree::Config rcfg;
      rcfg.prefix = base / "tt_margin";
      rcfg.retention_margin = 300;
      rcfg.compact_on_flush = true;
      TiledTree rtt(rcfg);
      for (size_t i = 0; i < n1; i++) // n1 == 1000
      {
        rtt.append(hashes[i]);
      }
      rtt.flush(); // covered = 768; target = floor((768 - 300) / 256) * 256 =
                   // 256
      expect(rtt.flushed_size() == 768, "margin: flushed size 768");
      expect(
        rtt.tree_ref().min_index() == 256,
        "margin: retained >= 300 recent leaves");

      for (size_t i = n1; i < N; i++)
      {
        rtt.append(hashes[i]);
      }
      expect(rtt.root() == ref_root, "margin: root matches reference");

      // Flushed-only (0, 255), flushed-but-resident overlap (256, 767), and the
      // un-flushed frontier (1000, 1499).
      for (const size_t i :
           {(size_t)0,
            (size_t)255,
            (size_t)256,
            (size_t)767,
            (size_t)1000,
            (size_t)1499})
      {
        const auto p = rtt.inclusion_proof(i, N);
        expect(
          *p == *ref.path(i), "margin inclusion==ref i=" + std::to_string(i));
        expect(
          p->verify(ref_root),
          "margin inclusion verify i=" + std::to_string(i));
      }

      bool threw = false;
      try
      {
        rtt.retract_to(700); // size 701 < immutable_size 768
      }
      catch (const std::exception&)
      {
        threw = true;
      }
      expect(threw, "margin: retract below flushed prefix throws");

      std::cout << "tiled tree (retention margin): OK" << '\n';
    }

    // ---- Part 3: rollback. Tiles are immutable, so only un-tiled
    //      (post-flush) entries may be rolled back.
    {
      // 3a. Before any flush nothing is tiled, so rollback is
      // unrestricted.
      {
        TiledTree::Config cfg;
        cfg.prefix = base / "rb_pre";
        TiledTree rb(cfg);
        for (size_t i = 0; i < 50; i++)
        {
          rb.append(hashes[i]);
        }
        rb.retract_to(29); // tiles_size == 0 -> allowed
        expect(rb.size() == 30, "rb pre-flush retract allowed");
      }

      // 3b. After a flush, the un-tiled frontier can be rolled back and
      // the
      //     tiled region stays consistent and provable; committed entries
      //     can't.
      {
        TiledTree::Config cfg;
        cfg.prefix = base / "rb";
        TiledTree rb(cfg);
        for (size_t i = 0; i < 300; i++)
        {
          rb.append(hashes[i]);
        }
        rb.flush(); // flushed_size = covered = 256
        for (size_t i = 300; i < 400; i++)
        {
          rb.append(hashes[i]);
        }
        rb.retract_to(349); // keep [0,349]
        expect(rb.size() == 350, "rb retracted to 350");
        for (size_t i = 350; i < 400; i++)
        {
          rb.append(hashes[1000 + i]); // re-append DIFFERENT leaves
        }
        rb.flush(); // flushed_size = covered = 256 (tile [0,256) already
                    // written)

        // Reference tree of the exact post-rollback state.
        merkle::Tree exp_tree;
        for (size_t i = 0; i < 350; i++)
        {
          exp_tree.insert(hashes[i]);
        }
        for (size_t i = 350; i < 400; i++)
        {
          exp_tree.insert(hashes[1000 + i]);
        }
        const Hash exp_root = exp_tree.root();
        expect(rb.root() == exp_root, "rb root matches reference");

        // Proofs for a tiled index and a frontier index match the reference.
        for (const size_t i : {(size_t)100, (size_t)299, (size_t)399})
        {
          const auto p = rb.inclusion_proof(i, 400);
          expect(
            *p == *exp_tree.path(i),
            "rb inclusion==ref i=" + std::to_string(i));
          expect(
            p->verify(exp_root), "rb inclusion verify i=" + std::to_string(i));
        }
        const auto cp = rb.consistency_proof(300, 400);
        expect(
          ProofEngine::verify_consistency(
            300, 400, *exp_tree.past_root(299), exp_root, cp),
          "rb consistency 300->400");

        // Only the immutable full-tile prefix [0,256) is protected: rolling
        // back into it is refused, while rolling back within the un-tiled
        // frontier (>= immutable_size()) is allowed.
        expect(rb.flushed_size() == 256, "rb flushed to full-tile prefix");

        rb.retract_to(std::numeric_limits<size_t>::max());
        expect(rb.size() == 400, "rb past-current index is a no-op");

        bool threw = false;
        try
        {
          rb.retract_to(100); // size 101 < 256
        }
        catch (const std::exception&)
        {
          threw = true;
        }
        expect(threw, "rb retract below tiled prefix throws");
        threw = false;
        try
        {
          rb.retract_to(254); // size 255 < 256
        }
        catch (const std::exception&)
        {
          threw = true;
        }
        expect(threw, "rb retract just below tiled prefix throws");
        rb.retract_to(255); // size 256 == immutable_size(): drops only frontier
        expect(rb.size() == 256, "rb retract to tiled prefix allowed");
      }

      // 3c. Compaction keeps the final tiled leaf resident, so rollback to a
      // tree size of exactly immutable_size() remains representable.
      {
        TiledTree::Config cfg;
        cfg.prefix = base / "rb_exact_boundary";
        cfg.compact_on_flush = true;
        TiledTree rb(cfg);
        for (size_t i = 0; i < 1000; i++)
        {
          rb.append(hashes[i]);
        }
        rb.flush();
        expect(rb.flushed_size() == 768, "rb boundary flushed size");
        expect(rb.immutable_size() == 768, "rb boundary immutable size");
        expect(
          rb.tree_ref().min_index() == 767,
          "rb boundary final tiled leaf retained");

        rb.retract_to(767);
        expect(rb.size() == 768, "rb exact immutable boundary allowed");

        merkle::Tree expected;
        for (size_t i = 0; i < 768; i++)
        {
          expected.insert(hashes[i]);
        }
        const Hash expected_root = expected.root();
        expect(
          rb.root() == expected_root, "rb boundary root matches reference");
        const auto proof = rb.inclusion_proof(0, 768);
        expect(
          *proof == *expected.path(0),
          "rb boundary inclusion matches reference");
        expect(proof->verify(expected_root), "rb boundary inclusion verifies");
      }

      // 3d. Rollback interacts correctly with compaction (flushed + tiled
      // past).
      {
        TiledTree::Config cfg;
        cfg.prefix = base / "rb_compact";
        cfg.compact_on_flush = true;
        TiledTree rb(cfg);
        for (size_t i = 0; i < 1000; i++)
        {
          rb.append(hashes[i]);
        }
        rb.flush(); // flushed_size = covered = 768; retain boundary leaf 767
        expect(
          rb.tree_ref().min_index() == 767,
          "rb compact boundary leaf retained");
        for (size_t i = 1000; i < 1200; i++)
        {
          rb.append(hashes[i]);
        }
        // Frontier rollback (>= immutable_size()) is allowed.
        rb.retract_to(1099);
        expect(rb.size() == 1100, "rb compact frontier retract ok");

        merkle::Tree exp_tree;
        for (size_t i = 0; i < 1100; i++)
        {
          exp_tree.insert(hashes[i]);
        }
        const Hash exp_root = exp_tree.root();
        const auto p = rb.inclusion_proof(0, 1100); // leaf 0 is flushed + tiled
        expect(*p == *exp_tree.path(0), "rb compact flushed inclusion==ref");
        expect(p->verify(exp_root), "rb compact flushed inclusion verify");

        bool threw = false;
        try
        {
          rb.retract_to(500);
        }
        catch (const std::exception&)
        {
          threw = true;
        }
        expect(threw, "rb compact retract below tiled throws");
      }

      // 3e. A failed flush may publish an immutable full tile before a later
      // write fails. The attempted full-tile boundary is sealed against
      // rollback, while flushed_size advances only after a successful retry.
      {
        TiledTree::Config cfg;
        cfg.prefix = base / "rb_interrupted";
        TiledTree interrupted(cfg);
        for (size_t i = 0; i < 512; i++)
        {
          interrupted.append(hashes[i]);
        }

        const fs::path blocker = store_root(cfg.prefix) / "tile/0/001";
        fs::create_directories(blocker);
        bool flush_threw = false;
        try
        {
          interrupted.flush();
        }
        catch (const std::exception&)
        {
          flush_threw = true;
        }

        expect(flush_threw, "interrupted flush throws");
        expect(
          fs::is_regular_file(store_root(cfg.prefix) / "tile/0/000"),
          "interrupted flush published first full tile");
        expect(
          interrupted.flushed_size() == 0,
          "interrupted flush does not advance flushed size");
        expect(
          interrupted.immutable_size() == 512,
          "interrupted flush seals attempted boundary");
        expect(
          interrupted.compact() == 0,
          "interrupted flush does not permit compaction");

        TiledTree recovered(std::move(interrupted));
        expect(
          recovered.immutable_size() == 512,
          "interrupted flush seal survives move");

        bool retract_threw = false;
        try
        {
          recovered.retract_to(0);
        }
        catch (const std::exception&)
        {
          retract_threw = true;
        }
        expect(
          retract_threw,
          "interrupted flush rejects rollback across attempted tiles");
        expect(recovered.size() == 512, "interrupted rollback changes nothing");

        fs::remove(blocker);
        expect(
          recovered.flush().full_written == 1,
          "interrupted flush retry writes only missing tile");
        expect(
          recovered.flushed_size() == 512,
          "interrupted flush retry advances flushed size");
        expect(
          recovered.immutable_size() == 512,
          "interrupted flush retry preserves immutable size");
        recovered.compact();

        merkle::Tree expected;
        for (size_t i = 0; i < 512; i++)
        {
          expected.insert(hashes[i]);
        }
        const Hash expected_root = expected.root();
        expect(
          recovered.root() == expected_root,
          "interrupted flush root matches reference");
        for (const size_t i :
             {(size_t)0, (size_t)255, (size_t)256, (size_t)511})
        {
          const auto proof = recovered.inclusion_proof(i, 512);
          expect(
            *proof == *expected.path(i),
            "interrupted flush inclusion matches reference");
          expect(
            proof->verify(expected_root),
            "interrupted flush inclusion verifies");
        }
      }

      // 3f. flushed_size advances only after every required level succeeds.
      // Here all 256 level-0 tiles publish before the level-1 tile is blocked.
      {
        constexpr size_t level1_size =
          (size_t)merkle::tiles::TILE_WIDTH * merkle::tiles::TILE_WIDTH;
        const auto level1_hashes = make_hashes(level1_size);

        TiledTree::Config cfg;
        cfg.prefix = base / "rb_interrupted_level1";
        TiledTree interrupted(cfg);
        for (const auto& hash : level1_hashes)
        {
          interrupted.append(hash);
        }

        const fs::path blocker = store_root(cfg.prefix) / "tile/1/000";
        fs::create_directories(blocker);
        bool flush_threw = false;
        try
        {
          interrupted.flush();
        }
        catch (const std::exception&)
        {
          flush_threw = true;
        }

        expect(flush_threw, "level-1 interrupted flush throws");
        expect(
          interrupted.store_ref().has_full_tile(0, 255),
          "level-1 interrupted flush publishes all level-0 tiles");
        expect(
          interrupted.flushed_size() == 0,
          "level-1 interrupted flush does not advance flushed size");
        expect(
          interrupted.immutable_size() == level1_size,
          "level-1 interrupted flush seals attempted boundary");

        fs::remove(blocker);
        expect(
          interrupted.flush().full_written == 1,
          "level-1 interrupted retry writes only the missing roll-up");
        expect(
          interrupted.flushed_size() == level1_size,
          "level-1 interrupted retry advances flushed size");
        expect(
          interrupted.store_ref().has_full_tile(1, 0),
          "level-1 interrupted retry publishes roll-up");

        merkle::Tree expected;
        for (const auto& hash : level1_hashes)
        {
          expected.insert(hash);
        }
        const Hash expected_root = expected.root();
        expect(
          interrupted.root() == expected_root,
          "level-1 interrupted root matches reference");
        const auto proof = interrupted.inclusion_proof(0, level1_size);
        expect(
          *proof == *expected.path(0),
          "level-1 interrupted inclusion matches reference");
        expect(
          proof->verify(expected_root),
          "level-1 interrupted inclusion verifies");
      }
    }
    std::cout << "rollback: OK" << '\n';

    std::cout << "tiles_tree: OK" << '\n';
  }
  catch (std::exception& ex)
  {
    std::cout << "Error: " << ex.what() << '\n';
    return 1;
  }
  catch (...)
  {
    std::cout << "Error" << '\n';
    return 1;
  }

  return 0;
}
