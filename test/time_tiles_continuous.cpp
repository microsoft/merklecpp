// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "tiles_test_util.h"
#include "util.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <merklecpp.h>
#include <merklecpp_tiles.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
using merkle::Hash;
using merkle::tiles::TiledTree;
using merkle::tiles::TileRef;
using merkle::tiles::TileStore;

static constexpr size_t CYCLES = 512;
static constexpr size_t BATCH_SIZE = TiledTree::TILE_WIDTH;
static constexpr size_t APPENDS = CYCLES * BATCH_SIZE;
static_assert(
  BATCH_SIZE == 256, "continuous benchmark targets default geometry");

struct Event
{
  std::string_view series;
  uint64_t event_index;
  size_t cycle;
  size_t leaf_count;
  std::string_view operation;
  uint64_t duration_ns;
  bool rollup;
};

struct Cycle
{
  size_t cycle;
  size_t leaf_count;
  bool rollup;
  uint64_t tiled_append_total_ns;
  uint64_t tiled_flush_ns;
  uint64_t tiled_compact_ns;
  uint64_t tiled_fifo_hits;
  uint64_t tiled_fifo_misses;
  uint64_t control_append_total_ns;
  uint64_t control_flush_ns;
};

struct Distribution
{
  uint64_t minimum;
  uint64_t p50;
  uint64_t p99;
  uint64_t maximum;
};

struct RollupBreakdown
{
  uint64_t level0_write_ns;
  uint64_t read_child_tiles_ns;
  uint64_t perfect_root_hashes_ns;
  uint64_t level1_write_ns;
};

static uint64_t elapsed_ns(const Clock::time_point& start)
{
  return static_cast<uint64_t>(
    std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start)
      .count());
}

static Distribution distribution(std::vector<uint64_t> values)
{
  if (values.empty())
  {
    throw std::runtime_error("cannot summarize an empty distribution");
  }
  std::sort(values.begin(), values.end());
  const auto percentile = [&](size_t numerator) {
    const size_t rank =
      (values.size() * numerator + 99) / static_cast<size_t>(100);
    const size_t index = std::max<size_t>(1, rank) - 1;
    return values[index];
  };
  return {values.front(), percentile(50), percentile(99), values.back()};
}

static uint64_t timer_overhead_ns()
{
  constexpr size_t samples = 10000;
  std::vector<uint64_t> values;
  values.reserve(samples);
  for (size_t sample = 0; sample < samples; sample++)
  {
    const auto start = Clock::now();
    values.push_back(elapsed_ns(start));
  }
  return distribution(std::move(values)).p50;
}

static uint64_t adjusted_elapsed_ns(
  const Clock::time_point& start, uint64_t overhead)
{
  const uint64_t observed = elapsed_ns(start);
  return observed > overhead ? observed - overhead : 0;
}

static void require_stream(const std::ofstream& stream, const fs::path& path)
{
  if (!stream)
  {
    throw std::runtime_error("could not write " + path.string());
  }
}

static void write_events(const fs::path& path, const std::vector<Event>& events)
{
  std::ofstream out(path);
  require_stream(out, path);
  out << "series,event_index,cycle,leaf_count,operation,duration_ns,rollup\n";
  for (const auto& event : events)
  {
    out << event.series << ',' << event.event_index << ',' << event.cycle << ','
        << event.leaf_count << ',' << event.operation << ','
        << event.duration_ns << ',' << (event.rollup ? 1 : 0) << '\n';
  }
  require_stream(out, path);
}

static void write_cycles(const fs::path& path, const std::vector<Cycle>& cycles)
{
  std::ofstream out(path);
  require_stream(out, path);
  out << "cycle,leaf_count,rollup,tiled_append_total_ns,tiled_flush_ns,"
         "tiled_compact_ns,tiled_fifo_hits,tiled_fifo_misses,"
         "control_append_total_ns,control_flush_ns\n";
  for (const auto& cycle : cycles)
  {
    out << cycle.cycle << ',' << cycle.leaf_count << ','
        << (cycle.rollup ? 1 : 0) << ',' << cycle.tiled_append_total_ns << ','
        << cycle.tiled_flush_ns << ',' << cycle.tiled_compact_ns << ','
        << cycle.tiled_fifo_hits << ',' << cycle.tiled_fifo_misses << ','
        << cycle.control_append_total_ns << ',' << cycle.control_flush_ns
        << '\n';
  }
  require_stream(out, path);
}

static void write_distribution(
  std::ofstream& out, std::string_view name, const Distribution& value)
{
  out << R"(    ")" << name << R"(": {"min_ns": )" << value.minimum
      << R"(, "p50_ns": )" << value.p50 << R"(, "p99_ns": )" << value.p99
      << R"(, "max_ns": )" << value.maximum << '}';
}

static RollupBreakdown benchmark_rollup(const fs::path& prefix)
{
  TileStore store(prefix);
  const auto hashes = make_hashes(BATCH_SIZE * BATCH_SIZE);
  std::vector<std::vector<Hash>> children;
  children.reserve(BATCH_SIZE);
  for (size_t tile = 0; tile < BATCH_SIZE; tile++)
  {
    const auto begin =
      hashes.begin() + static_cast<std::ptrdiff_t>(tile * BATCH_SIZE);
    children.emplace_back(
      begin, begin + static_cast<std::ptrdiff_t>(BATCH_SIZE));
  }

  store.write_tile(TileRef{0, 0}, children[0]);
  for (size_t tile = 1; tile + 1 < BATCH_SIZE; tile++)
  {
    store.write_tile(TileRef{0, static_cast<uint64_t>(tile)}, children[tile]);
  }
  auto start = Clock::now();
  store.write_tile(
    TileRef{0, static_cast<uint64_t>(BATCH_SIZE - 1)}, children.back());
  const uint64_t level0_write_ns = elapsed_ns(start);

  std::vector<std::vector<Hash>> read_children;
  read_children.reserve(BATCH_SIZE);
  start = Clock::now();
  for (size_t tile = 0; tile < BATCH_SIZE; tile++)
  {
    read_children.push_back(
      store.read_tile(TileRef{0, static_cast<uint64_t>(tile)}));
  }
  const uint64_t read_child_tiles_ns = elapsed_ns(start);

  std::vector<Hash> roots;
  roots.reserve(BATCH_SIZE);
  start = Clock::now();
  for (const auto& child : read_children)
  {
    roots.push_back(merkle::tiles::perfect_root<
                    merkle::Tree::Hash::size_bytes,
                    merkle::Tree::hash_function>(child));
  }
  const uint64_t perfect_root_hashes_ns = elapsed_ns(start);

  start = Clock::now();
  store.write_tile(TileRef{1, 0}, roots);
  const uint64_t level1_write_ns = elapsed_ns(start);

  return {
    level0_write_ns,
    read_child_tiles_ns,
    perfect_root_hashes_ns,
    level1_write_ns};
}

int main()
{
  try
  {
    const char* configured_output = std::getenv("TILE_PERF_OUTPUT_DIR");
    const fs::path output_dir =
      configured_output == nullptr ? fs::current_path() : configured_output;
    fs::create_directories(output_dir);

    const auto hashes = make_hashes(APPENDS);
    const uint64_t append_timer_overhead_ns = timer_overhead_ns();
    std::vector<Event> events;
    events.reserve(APPENDS * 2 + CYCLES * 3);
    std::vector<Cycle> cycles(CYCLES);
    uint64_t event_index = 0;

    const TemporaryDirectory tiled_directory("merklecpp_continuous_tiled");
    TiledTree::Config config;
    config.prefix = tiled_directory.path();
    TiledTree tiled(config);
    merkle::Tree reference;

    for (size_t cycle_index = 0; cycle_index < CYCLES; cycle_index++)
    {
      auto& cycle = cycles[cycle_index];
      cycle.cycle = cycle_index;
      cycle.leaf_count = (cycle_index + 1) * BATCH_SIZE;
      cycle.rollup = (cycle_index + 1) % BATCH_SIZE == 0;
      cycle.tiled_append_total_ns = 0;
      cycle.control_append_total_ns = 0;
      cycle.control_flush_ns = 0;

      for (size_t offset = 0; offset < BATCH_SIZE; offset++)
      {
        const size_t index = cycle_index * BATCH_SIZE + offset;
        auto start = Clock::now();
        tiled.append(hashes[index]);
        const uint64_t duration =
          adjusted_elapsed_ns(start, append_timer_overhead_ns);
        cycle.tiled_append_total_ns += duration;
        events.push_back(
          {"tiled",
           event_index++,
           cycle_index,
           index + 1,
           "append",
           duration,
           false});
        reference.insert(hashes[index]);
      }

      auto start = Clock::now();
      const auto stats = tiled.flush();
      cycle.tiled_flush_ns = elapsed_ns(start);
      cycle.tiled_fifo_hits = stats.root_fifo_hits;
      cycle.tiled_fifo_misses = stats.root_fifo_misses;
      events.push_back(
        {"tiled",
         event_index++,
         cycle_index,
         cycle.leaf_count,
         "flush",
         cycle.tiled_flush_ns,
         cycle.rollup});

      start = Clock::now();
      tiled.compact();
      cycle.tiled_compact_ns = elapsed_ns(start);
      events.push_back(
        {"tiled",
         event_index++,
         cycle_index,
         cycle.leaf_count,
         "compact",
         cycle.tiled_compact_ns,
         false});
    }

    const Hash expected_root = reference.root();
    if (tiled.root() != expected_root)
    {
      throw std::runtime_error("continuous tiled root mismatch");
    }

    merkle::Tree control;
    for (size_t cycle_index = 0; cycle_index < CYCLES; cycle_index++)
    {
      auto& cycle = cycles[cycle_index];
      for (size_t offset = 0; offset < BATCH_SIZE; offset++)
      {
        const size_t index = cycle_index * BATCH_SIZE + offset;
        auto start = Clock::now();
        control.insert(hashes[index]);
        const uint64_t duration =
          adjusted_elapsed_ns(start, append_timer_overhead_ns);
        cycle.control_append_total_ns += duration;
        events.push_back(
          {"tree-control",
           event_index++,
           cycle_index,
           index + 1,
           "append",
           duration,
           false});
      }

      auto start = Clock::now();
      control.flush_to(control.max_index());
      cycle.control_flush_ns = elapsed_ns(start);
      events.push_back(
        {"tree-control",
         event_index++,
         cycle_index,
         cycle.leaf_count,
         "flush",
         cycle.control_flush_ns,
         false});
    }
    if (control.root() != expected_root)
    {
      throw std::runtime_error("plain tree control root mismatch");
    }

    std::vector<uint64_t> tiled_append;
    std::vector<uint64_t> tiled_flush_normal;
    std::vector<uint64_t> tiled_flush_rollup;
    std::vector<uint64_t> tiled_compact;
    std::vector<uint64_t> control_append;
    std::vector<uint64_t> control_flush;
    for (const auto& event : events)
    {
      auto* destination = &tiled_append;
      if (event.series == "tree-control")
      {
        destination =
          event.operation == "append" ? &control_append : &control_flush;
      }
      else if (event.operation == "flush")
      {
        destination = event.rollup ? &tiled_flush_rollup : &tiled_flush_normal;
      }
      else if (event.operation == "compact")
      {
        destination = &tiled_compact;
      }
      destination->push_back(event.duration_ns);
    }

    const Distribution tiled_append_summary = distribution(tiled_append);
    const Distribution tiled_flush_normal_summary =
      distribution(tiled_flush_normal);
    const Distribution tiled_flush_rollup_summary =
      distribution(tiled_flush_rollup);
    const Distribution tiled_compact_summary = distribution(tiled_compact);
    const Distribution control_append_summary = distribution(control_append);
    const Distribution control_flush_summary = distribution(control_flush);

    const TemporaryDirectory breakdown_directory("merklecpp_rollup_breakdown");
    const RollupBreakdown breakdown =
      benchmark_rollup(breakdown_directory.path());

    const fs::path events_path = output_dir / "tile-performance-events.csv";
    const fs::path cycles_path = output_dir / "tile-performance-cycles.csv";
    const fs::path summary_path = output_dir / "tile-performance-summary.json";
    write_events(events_path, events);
    write_cycles(cycles_path, cycles);

    std::ofstream summary(summary_path);
    require_stream(summary, summary_path);
    summary << "{\n"
            << "  \"schema_version\": 1,\n"
            << "  \"tile_width\": " << BATCH_SIZE << ",\n"
            << "  \"cycles\": " << CYCLES << ",\n"
            << "  \"appends\": " << APPENDS << ",\n"
            << "  \"append_timer_overhead_ns\": " << append_timer_overhead_ns
            << ",\n"
            << R"(  "root": ")" << expected_root.to_string() << "\",\n"
            << "  \"tiled\": {\n";
    write_distribution(summary, "append", tiled_append_summary);
    summary << ",\n";
    write_distribution(summary, "flush_normal", tiled_flush_normal_summary);
    summary << ",\n";
    write_distribution(summary, "flush_rollup", tiled_flush_rollup_summary);
    summary << ",\n";
    write_distribution(summary, "compact", tiled_compact_summary);
    summary << "\n  },\n  \"tree_control\": {\n";
    write_distribution(summary, "append", control_append_summary);
    summary << ",\n";
    write_distribution(summary, "flush", control_flush_summary);
    summary << "\n  },\n"
            << "  \"rollup_breakdown\": {\n"
            << "    \"level0_write_ns\": " << breakdown.level0_write_ns << ",\n"
            << "    \"read_child_tiles_ns\": " << breakdown.read_child_tiles_ns
            << ",\n"
            << "    \"perfect_root_hashes_ns\": "
            << breakdown.perfect_root_hashes_ns << ",\n"
            << "    \"level1_write_ns\": " << breakdown.level1_write_ns << ",\n"
            << "    \"child_tiles\": " << BATCH_SIZE << ",\n"
            << "    \"parent_hashes\": " << BATCH_SIZE * (BATCH_SIZE - 1)
            << "\n"
            << "  }\n}\n";
    require_stream(summary, summary_path);

    std::cout << std::fixed << std::setprecision(3)
              << "continuous_tiled: append p50/p99 " << tiled_append_summary.p50
              << '/' << tiled_append_summary.p99 << " ns (timer overhead "
              << append_timer_overhead_ns << " ns), normal flush "
              << static_cast<double>(tiled_flush_normal_summary.p50) / 1e6
              << '/'
              << static_cast<double>(tiled_flush_normal_summary.p99) / 1e6
              << " ms, roll-up flush "
              << static_cast<double>(tiled_flush_rollup_summary.p50) / 1e6
              << '/'
              << static_cast<double>(tiled_flush_rollup_summary.p99) / 1e6
              << " ms\n"
              << "continuous_control: append p50/p99 "
              << control_append_summary.p50 << '/' << control_append_summary.p99
              << " ns, flush "
              << static_cast<double>(control_flush_summary.p50) / 1e6 << '/'
              << static_cast<double>(control_flush_summary.p99) / 1e6 << " ms\n"
              << "rollup_breakdown: level0 write "
              << static_cast<double>(breakdown.level0_write_ns) / 1e6
              << " ms, reads "
              << static_cast<double>(breakdown.read_child_tiles_ns) / 1e6
              << " ms, perfect roots "
              << static_cast<double>(breakdown.perfect_root_hashes_ns) / 1e6
              << " ms, level1 write "
              << static_cast<double>(breakdown.level1_write_ns) / 1e6 << " ms\n"
              << "time_tiles_continuous: OK\n";
  }
  catch (const std::exception& ex)
  {
    std::cerr << "Error: " << ex.what() << '\n';
    return 1;
  }
  return 0;
}
