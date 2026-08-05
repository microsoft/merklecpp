#include "tiles_test_util.h"
#include "util.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <merklecpp.h>
#include <merklecpp_tiles.h>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

using merkle::Hash;
using merkle::tiles::CombinedHashSource;
using merkle::tiles::HashSource;
using merkle::tiles::MemoryHashSource;
using merkle::tiles::ProofEngine;
using merkle::tiles::TILE_WIDTH;
using merkle::tiles::TileHashSource;
using merkle::tiles::TileStore;
using merkle::tiles::TileWriter;

struct Attempt
{
  std::string source;
  uint8_t level;
  uint64_t index;
  bool success;
};

using Attempts = std::vector<Attempt>;

class TracingSource : public HashSource
{
public:
  TracingSource(
    const HashSource& source, std::string name, Attempts& attempts) :
    source(source), name(std::move(name)), attempts(attempts)
  {}

  bool subtree_root(uint8_t level, uint64_t index, Hash& out) const override
  {
    const bool success = source.subtree_root(level, index, out);
    attempts.push_back(Attempt{name, level, index, success});
    return success;
  }

private:
  const HashSource& source;
  std::string name;
  Attempts& attempts;
};

struct Scenario
{
  std::string id;
  std::string type;
  std::string title;
  std::string description;
  std::string takeaway;
  uint64_t leaves;
  uint64_t focus;
  uint64_t second_index;
  uint64_t covered;
  uint64_t frontier_start;
  Attempts attempts;
};

static void run_scenario(
  const fs::path& directory,
  Scenario& scenario,
  const std::vector<Hash>& hashes)
{
  TileStore store(directory);
  TileWriter writer(store);
  const auto leaf_at = [&](uint64_t index) -> const Hash& {
    return hashes.at(static_cast<size_t>(index));
  };
  writer.write_up_to(scenario.leaves, leaf_at);

  merkle::Tree oracle;
  merkle::Tree frontier;
  for (uint64_t index = 0; index < scenario.leaves; index++)
  {
    oracle.insert(hashes.at(static_cast<size_t>(index)));
    frontier.insert(hashes.at(static_cast<size_t>(index)));
  }
  const Hash root = oracle.root();

  scenario.covered = (scenario.leaves / TILE_WIDTH) * TILE_WIDTH;
  scenario.frontier_start = scenario.covered;
  if (scenario.frontier_start >= scenario.leaves)
  {
    scenario.frontier_start = scenario.leaves - 1;
  }
  if (scenario.frontier_start > 0)
  {
    frontier.flush_to(static_cast<size_t>(scenario.frontier_start));
  }

  const MemoryHashSource memory(frontier);
  const TileHashSource tiles(store, scenario.covered);
  const TracingSource traced_memory(memory, "frontier", scenario.attempts);
  const TracingSource traced_tiles(tiles, "tile", scenario.attempts);
  const CombinedHashSource combined(traced_memory, traced_tiles);
  const ProofEngine engine(combined);
  const CombinedHashSource control_combined(memory, tiles);
  const ProofEngine control_engine(control_combined);

  if (scenario.type == "inclusion")
  {
    const auto proof = engine.inclusion_proof(scenario.focus, scenario.leaves);
    const auto control_proof =
      control_engine.inclusion_proof(scenario.focus, scenario.leaves);
    if (
      !proof->verify(root) || *proof != *oracle.path(scenario.focus) ||
      *proof != *control_proof)
    {
      throw std::runtime_error("inclusion proof mismatch for " + scenario.id);
    }
  }
  else
  {
    if (
      scenario.focus >= scenario.second_index ||
      scenario.second_index >= scenario.leaves)
    {
      throw std::runtime_error(
        "invalid consistency indices for " + scenario.id);
    }
    const Hash first_root = *oracle.past_root(scenario.focus);
    const Hash second_root = *oracle.past_root(scenario.second_index);
    const auto proof = engine.consistency_proof_from_indices(
      scenario.focus, scenario.second_index);
    const auto control_proof = control_engine.consistency_proof_from_indices(
      scenario.focus, scenario.second_index);
    if (
      proof != control_proof ||
      !ProofEngine::verify_consistency(
        scenario.focus + 1,
        scenario.second_index + 1,
        first_root,
        second_root,
        proof))
    {
      throw std::runtime_error("consistency proof mismatch for " + scenario.id);
    }
  }
}

static void write_data(
  const fs::path& output, const std::vector<Scenario>& scenarios)
{
  std::ofstream stream(output);
  if (!stream)
  {
    throw std::runtime_error("could not open " + output.string());
  }

  stream << "window.PROOF_VIZ_DATA = {\n";
  stream << "  tileWidth: " << TILE_WIDTH << ",\n";
  stream << "  scenarios: [\n";
  for (size_t scenario_index = 0; scenario_index < scenarios.size();
       scenario_index++)
  {
    const Scenario& scenario = scenarios[scenario_index];
    stream << "    {\n";
    stream << "      id: " << std::quoted(scenario.id) << ",\n";
    stream << "      type: " << std::quoted(scenario.type) << ",\n";
    stream << "      title: " << std::quoted(scenario.title) << ",\n";
    stream << "      description: " << std::quoted(scenario.description)
           << ",\n";
    stream << "      takeaway: " << std::quoted(scenario.takeaway) << ",\n";
    stream << "      leaves: " << scenario.leaves << ",\n";
    stream << "      covered: " << scenario.covered << ",\n";
    stream << "      frontierStart: " << scenario.frontier_start << ",\n";
    stream << "      focus: " << scenario.focus << ",\n";
    stream << "      secondIndex: " << scenario.second_index << ",\n";
    stream << "      attempts: [\n";
    for (size_t attempt_index = 0; attempt_index < scenario.attempts.size();
         attempt_index++)
    {
      const Attempt& attempt = scenario.attempts[attempt_index];
      stream << "        {source:" << std::quoted(attempt.source)
             << ",level:" << static_cast<unsigned>(attempt.level)
             << ",index:" << attempt.index
             << ",success:" << (attempt.success ? "true" : "false") << "}";
      stream << (attempt_index + 1 == scenario.attempts.size() ? "\n" : ",\n");
    }
    stream << "      ]\n";
    stream << "    }";
    stream << (scenario_index + 1 == scenarios.size() ? "\n" : ",\n");
  }
  stream << "  ]\n";
  stream << "};\n";
}

int main(int argc, char** argv)
{
  try
  {
    const fs::path output = argc > 1 ? argv[1] : "data.js";
    const TemporaryDirectory temporary_directory("merklecpp_proof_viz");
    const auto hashes = make_hashes(513);
    std::vector<Scenario> scenarios = {
      {"frontier-only",
       "inclusion",
       "Before the first tile",
       "With 192 leaves, no full 256-entry tile exists. Every subtree request "
       "is answered by the resident tree.",
       "The red route never changes source: the target, proof siblings, and "
       "root reduction all stay in the blue frontier.",
       192,
       37,
       0,
       0,
       0,
       {}},
      {"tile-to-frontier",
       "inclusion",
       "A proof leaves the tiled past",
       "Leaf 42 is already represented by the first full tile, while leaves "
       "256-299 remain resident in memory.",
       "The proof starts in green tile storage, then needs blue frontier "
       "subtrees to complete the current 300-leaf root.",
       300,
       42,
       0,
       0,
       0,
       {}},
      {"frontier-to-tile",
       "inclusion",
       "A frontier proof reaches backward",
       "Leaf 271 sits in the 44-leaf resident frontier beyond one completed "
       "tile.",
       "Most local siblings are blue, but the final left sibling is the entire "
       "256-leaf tiled prefix, resolved in one green lookup.",
       300,
       271,
       0,
       0,
       0,
       {}},
      {"near-boundary",
       "inclusion",
       "One leaf short of the next tile",
       "At 511 leaves, the first tile is durable and the next 255 leaves still "
       "form the frontier.",
       "The long blue frontier is not itself a perfect subtree. The engine "
       "assembles it from smaller resident ranges before joining the green "
       "past.",
       511,
       510,
       0,
       0,
       0,
       {}},
      {"boundary-overlap",
       "inclusion",
       "The exact two-tile boundary",
       "At 512 leaves, both tiles are complete. Compaction deliberately "
       "retains the final leaf in memory because merklecpp never flushes the "
       "entire tree.",
       "The blue target pixel has a green overlap mark: both sources can "
       "answer it, but CombinedHashSource chooses memory first; its siblings "
       "come from tiles.",
       512,
       511,
       0,
       0,
       0,
       {}},
      {"next-frontier",
       "inclusion",
       "A new frontier after two tiles",
       "Leaf 512 is the first resident leaf after a 512-leaf tiled prefix.",
       "The proof begins with one blue leaf and crosses immediately to a "
       "single green 512-leaf sibling that represents both completed tiles.",
       513,
       512,
       0,
       0,
       0,
       {}},
      {"consistency-boundary",
       "consistency",
       "Consistency across the flush line",
       "Leaf indices 255 and 299 identify the 256- and 300-leaf checkpoints "
       "on opposite sides of the flush line.",
       "There is no target leaf. Red marks the SUBPROOF recursion and its "
       "proof components, combining the old green root with blue hashes from "
       "the new frontier.",
       300,
       255,
       299,
       0,
       0,
       {}},
      {"consistency-frontier-only",
       "consistency",
       "Two leaves before tiling",
       "Leaves A=47 and B=149 end the 48- and 150-leaf tree states inside a "
       "192-leaf backing tree that has not completed its first tile.",
       "Every consistency component is answered by the resident frontier, "
       "even though neither selected leaf is the backing tree's current end.",
       192,
       47,
       149,
       0,
       0,
       {}},
      {"consistency-tiled-history",
       "consistency",
       "Two leaves in tiled history",
       "Leaves A=127 and B=399 end historical tree states inside a 513-leaf "
       "backing tree with two durable tiles.",
       "The current frontier begins after both checkpoints, so the complete "
       "consistency proof is recovered from green tile storage.",
       513,
       127,
       399,
       0,
       0,
       {}},
      {"consistency-arbitrary-crossing",
       "consistency",
       "An arbitrary leaf pair crosses the boundary",
       "Leaves A=91 and B=287 end the 92- and 288-leaf tree states in a "
       "300-leaf backing tree.",
       "The tree ending at A sits wholly in the tiled past, while the tree "
       "ending at B requires both the durable tile and resident frontier.",
       300,
       91,
       287,
       0,
       0,
       {}},
      {"consistency-frontier-pair",
       "consistency",
       "Two leaves inside one frontier",
       "Leaves A=269 and B=493 end two tree states beyond the first tile in "
       "a 511-leaf backing tree.",
       "Both roots include tiled history, but their changing suffixes are "
       "assembled from different portions of the same blue frontier.",
       511,
       269,
       493,
       0,
       0,
       {}}};

    for (Scenario& scenario : scenarios)
    {
      run_scenario(temporary_directory.path() / scenario.id, scenario, hashes);
      std::cout << scenario.id << ": " << scenario.attempts.size()
                << " source attempts\n";
    }
    write_data(output, scenarios);
    std::cout << "wrote " << output << '\n';
  }
  catch (const std::exception& error)
  {
    std::cerr << "proof visualization failed: " << error.what() << '\n';
    return 1;
  }
  return 0;
}