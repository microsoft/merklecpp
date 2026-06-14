// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "util.h"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <merklecpp.h>
#include <merklecpp_tiles.h>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using merkle::Hash;
using merkle::tiles::TiledTree;

static double secs_since(
  const std::chrono::high_resolution_clock::time_point& start)
{
  const auto stop = std::chrono::high_resolution_clock::now();
  return (double)std::chrono::duration_cast<std::chrono::nanoseconds>(
           stop - start)
           .count() /
    1e9;
}

// Uniform random in [0, bound).
static uint64_t rand_below(uint64_t bound)
{
  if (bound == 0)
  {
    return 0;
  }
  return (uint64_t)((std::rand() / (RAND_MAX + 1.0)) * (double)bound);
}

int main()
{
  const auto seed = std::time(nullptr);
  std::srand((unsigned)seed);
  std::cout << "seed=" << seed << '\n';

#ifndef NDEBUG
  const uint64_t num_leaves = 50000;
  const uint64_t num_proofs = 100;
#else
  const uint64_t num_leaves = 1000000;
  const uint64_t num_proofs = 10000;
#endif

  const fs::path dir = fs::temp_directory_path() /
    ("merklecpp_time_tiles_" + std::to_string((unsigned long long)seed));

  // Accumulator that consumes each proof, so the work is not optimised away.
  volatile uint64_t sink = 0;

  std::cout << std::fixed << std::setprecision(3);

  try
  {
    const auto hashes = make_hashes(num_leaves);

    TiledTree::Config cfg;
    cfg.prefix = dir;
    TiledTree log(cfg);

    // 1. Append: grow the in-memory tree.
    auto t = std::chrono::high_resolution_clock::now();
    for (uint64_t i = 0; i < num_leaves; i++)
    {
      log.append(hashes[i]);
    }
    log.root();
    const double append_s = secs_since(t);
    std::cout << "append             : " << num_leaves << " leaves in "
              << append_s << " sec ("
              << (uint64_t)((double)num_leaves / append_s) << " leaves/sec)\n";

    // 2. Flush: write newly-complete tiles to disk.
    t = std::chrono::high_resolution_clock::now();
    const auto stats = log.flush();
    const double flush_s = secs_since(t);
    const uint64_t tiles = stats.full_written + stats.partial_written;
    std::cout << "flush (to disk)    : " << stats.full_written << " full + "
              << stats.partial_written << " partial tiles in " << flush_s
              << " sec (" << (uint64_t)((double)tiles / flush_s)
              << " tiles/sec)\n";

    const uint64_t n = log.size();

    // 3. Inclusion proofs while everything is still resident (memory path).
    t = std::chrono::high_resolution_clock::now();
    for (uint64_t k = 0; k < num_proofs; k++)
    {
      sink += log.inclusion_proof(rand_below(n), n)->size();
    }
    const double inc_mem_s = secs_since(t);
    std::cout << "inclusion (memory) : " << num_proofs << " proofs in "
              << inc_mem_s << " sec ("
              << (uint64_t)((double)num_proofs / inc_mem_s) << " proofs/sec)\n";

    // 4. Consistency proofs while resident (memory path).
    t = std::chrono::high_resolution_clock::now();
    for (uint64_t k = 0; k < num_proofs; k++)
    {
      sink += log.consistency_proof(1 + rand_below(n - 1), n).size();
    }
    const double con_mem_s = secs_since(t);
    std::cout << "consistency(memory): " << num_proofs << " proofs in "
              << con_mem_s << " sec ("
              << (uint64_t)((double)num_proofs / con_mem_s) << " proofs/sec)\n";

    // 5. Compact: drop tiled leaves from memory.
    t = std::chrono::high_resolution_clock::now();
    const uint64_t min_idx = log.compact();
    const double compact_s = secs_since(t);
    std::cout << "compact            : dropped " << min_idx
              << " leaves from memory in " << compact_s << " sec\n";

    // 6. Inclusion proofs for evicted leaves: served from the on-disk tiles.
    t = std::chrono::high_resolution_clock::now();
    for (uint64_t k = 0; k < num_proofs; k++)
    {
      sink += log.inclusion_proof(rand_below(min_idx), n)->size();
    }
    const double inc_tile_s = secs_since(t);
    std::cout << "inclusion (tiles)  : " << num_proofs << " proofs in "
              << inc_tile_s << " sec ("
              << (uint64_t)((double)num_proofs / inc_tile_s)
              << " proofs/sec)\n";

    // 7. Consistency proofs spanning the tiled (evicted) past.
    t = std::chrono::high_resolution_clock::now();
    for (uint64_t k = 0; k < num_proofs; k++)
    {
      sink += log.consistency_proof(1 + rand_below(min_idx - 1), n).size();
    }
    const double con_tile_s = secs_since(t);
    std::cout << "consistency(tiles) : " << num_proofs << " proofs in "
              << con_tile_s << " sec ("
              << (uint64_t)((double)num_proofs / con_tile_s)
              << " proofs/sec)\n";

    // 8. Baseline: identical inclusion proofs from a plain in-memory Tree.
    merkle::Tree ref;
    for (uint64_t i = 0; i < num_leaves; i++)
    {
      ref.insert(hashes[i]);
    }
    ref.root();
    t = std::chrono::high_resolution_clock::now();
    for (uint64_t k = 0; k < num_proofs; k++)
    {
      sink += ref.path(rand_below(n))->size();
    }
    const double inc_ref_s = secs_since(t);
    std::cout << "inclusion (Tree)   : " << num_proofs << " proofs in "
              << inc_ref_s << " sec ("
              << (uint64_t)((double)num_proofs / inc_ref_s) << " proofs/sec)\n";

    // Sanity: a tile-served proof still verifies against the reference root.
    if (!log.inclusion_proof(0, n)->verify(ref.root()))
    {
      throw std::runtime_error("benchmark proof failed to verify");
    }

    std::cout << "time_tiles: OK (checksum " << sink << ")\n";

    std::error_code ec;
    fs::remove_all(dir, ec);
  }
  catch (std::exception& ex)
  {
    std::cout << "Error: " << ex.what() << '\n';
    std::error_code ec;
    fs::remove_all(dir, ec);
    return 1;
  }
  catch (...)
  {
    std::cout << "Error" << '\n';
    std::error_code ec;
    fs::remove_all(dir, ec);
    return 1;
  }

  return 0;
}
