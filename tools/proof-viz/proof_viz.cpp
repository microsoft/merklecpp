#include "tiles_test_util.h"
#include "util.h"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <merklecpp.h>
#include <merklecpp_tiles.h>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

using merkle::Hash;
using merkle::tiles::CombinedHashSource;
using merkle::tiles::HashSource;
using merkle::tiles::MAX_TILE_LEVEL;
using merkle::tiles::MemoryHashSource;
using merkle::tiles::ProofEngine;
using merkle::tiles::TILE_WIDTH;
using merkle::tiles::TileHashSource;
using merkle::tiles::TileStore;
using merkle::tiles::TileWriter;

// Node roles, packed into one integer per node so the JSON stays compact.
static constexpr uint32_t FLAG_TILE = 1U << 0;
static constexpr uint32_t FLAG_FRONTIER = 1U << 1;
static constexpr uint32_t FLAG_PROOF = 1U << 2;
static constexpr uint32_t FLAG_FIRST = 1U << 3;
static constexpr uint32_t FLAG_SECOND = 1U << 4;

struct Scenario
{
  std::string id;
  std::string title;
  std::string description;
  std::string takeaway;
  size_t order;
  size_t leaves;
  size_t focus;
  std::optional<size_t> second_index;
  size_t covered = 0;
  size_t frontier_start = 0;
  size_t map_leaves = 0;

  struct Node
  {
    uint64_t lo = 0;
    uint64_t hi = 0;
    uint32_t depth = 0;
    int64_t parent = -1;
    int64_t left = -1;
    int64_t right = -1;
    uint32_t flags = 0;
    Hash digest;
    bool resolved = false;
  };
  std::vector<Node> nodes;

  /// Node ids of the returned proof elements, in the order the proof lists
  /// them.
  std::vector<size_t> proof_nodes;

  /// The full tiles that reached disk, as (level, index) pairs.
  std::vector<std::pair<uint8_t, uint64_t>> tile_files;
};

/// @brief Lays out the RFC 6962 decomposition of [lo, hi), splitting at the
/// largest power of two below the width.
/// @note Shape only: which nodes a proof uses, and which store can serve them,
/// are read back from the library rather than predicted here.
// NOLINTNEXTLINE(misc-no-recursion) -- mirrors the bounded binary node tree.
static size_t build_nodes(
  std::vector<Scenario::Node>& nodes,
  uint64_t lo,
  uint64_t hi,
  uint32_t depth,
  int64_t parent)
{
  const size_t index = nodes.size();
  nodes.push_back(Scenario::Node{lo, hi, depth, parent});

  if (hi - lo > 1)
  {
    const uint64_t split = lo + std::bit_floor(hi - lo - 1);
    const auto left =
      build_nodes(nodes, lo, split, depth + 1, static_cast<int64_t>(index));
    const auto right =
      build_nodes(nodes, split, hi, depth + 1, static_cast<int64_t>(index));
    nodes[index].left = static_cast<int64_t>(left);
    nodes[index].right = static_cast<int64_t>(right);
  }
  return index;
}

/// @brief Records which store can serve each node, by asking both of them.
/// @note The proof engine runs on a CombinedHashSource, which hides the answer
/// behind a short-circuiting fallback. Querying the tile and frontier sources
/// separately reports what each one actually holds, so a node that both can
/// serve is observed rather than inferred from the flush boundary.
static void classify_nodes(
  Scenario& scenario, const HashSource& tiles, const HashSource& frontier)
{
  for (Scenario::Node& node : scenario.nodes)
  {
    const uint64_t width = node.hi - node.lo;
    if (!std::has_single_bit(width) || node.lo % width != 0)
    {
      continue;
    }

    const auto level = static_cast<uint8_t>(std::countr_zero(width));
    const uint64_t index = node.lo >> level;
    Hash from_tiles;
    Hash from_frontier;
    const bool in_tiles = tiles.subtree_root(level, index, from_tiles);
    const bool in_frontier = frontier.subtree_root(level, index, from_frontier);

    if (in_tiles && in_frontier && from_tiles != from_frontier)
    {
      throw std::runtime_error(
        "tile and frontier sources disagree in " + scenario.id);
    }
    if (in_tiles)
    {
      node.digest = from_tiles;
      node.flags |= FLAG_TILE;
      node.resolved = true;
    }
    if (in_frontier)
    {
      node.digest = from_frontier;
      node.flags |= FLAG_FRONTIER;
      node.resolved = true;
    }
  }
}

/// @brief Fills in the digests of the ranges no store can serve directly.
/// @note These are exactly the ranges the proof engine has to recombine. Nodes
/// are laid out in pre-order, so a reverse pass always sees children first.
static void fill_digests(Scenario& scenario)
{
  for (size_t index = scenario.nodes.size(); index-- > 0;)
  {
    Scenario::Node& node = scenario.nodes[index];
    if (node.resolved || node.left < 0)
    {
      continue;
    }

    const Scenario::Node& left = scenario.nodes[node.left];
    const Scenario::Node& right = scenario.nodes[node.right];
    if (!left.resolved || !right.resolved)
    {
      throw std::runtime_error("unresolved child range in " + scenario.id);
    }
    merkle::sha256(left.digest, right.digest, node.digest);
    node.resolved = true;
  }
}

/// @brief Lists the full tiles that reached disk.
static std::vector<std::pair<uint8_t, uint64_t>> list_tiles(
  const TileStore& store)
{
  std::vector<std::pair<uint8_t, uint64_t>> refs;
  for (uint8_t level = 0; level <= MAX_TILE_LEVEL; level++)
  {
    uint64_t index = 0;
    while (store.has_full_tile(level, index))
    {
      refs.emplace_back(level, index);
      index++;
    }
    if (index == 0)
    {
      break;
    }
  }
  return refs;
}

static Scenario read_scenario(const fs::path& path)
{
  static constexpr std::array<std::string_view, 9> keys = {
    "name",
    "proof",
    "title",
    "description",
    "takeaway",
    "order",
    "leaves",
    "focus",
    "second"};
  std::ifstream stream(path);
  if (!stream)
  {
    throw std::runtime_error("could not open " + path.string());
  }
  const auto error = [&](size_t line, const std::string& message) {
    return std::runtime_error(
      path.string() + ":" + std::to_string(line) + ": " + message);
  };

  std::array<std::string, keys.size()> values;
  for (size_t index = 0; index < keys.size(); index++)
  {
    std::string line;
    if (!std::getline(stream, line))
    {
      throw error(index + 1, "expected key " + std::string(keys[index]));
    }
    if (!line.empty() && line.back() == '\r')
    {
      line.pop_back();
    }
    const std::string prefix = std::string(keys[index]) + ": ";
    if (line.compare(0, prefix.size(), prefix) != 0)
    {
      throw error(index + 1, "expected key " + std::string(keys[index]));
    }
    values[index] = line.substr(prefix.size());
    if (
      values[index].empty() || values[index].front() == ' ' ||
      values[index].back() == ' ' ||
      std::any_of(
        values[index].begin(),
        values[index].end(),
        [](unsigned char character) { return character < 0x20; }))
    {
      throw error(index + 1, "invalid value for " + std::string(keys[index]));
    }
  }
  std::string extra;
  if (std::getline(stream, extra))
  {
    throw error(keys.size() + 1, "unexpected extra line");
  }

  const auto number = [&](size_t index) {
    size_t value = 0;
    const auto [end, parse_error] = std::from_chars(
      values[index].data(), values[index].data() + values[index].size(), value);
    if (
      parse_error != std::errc{} ||
      end != values[index].data() + values[index].size())
    {
      throw error(
        index + 1, "invalid unsigned integer for " + std::string(keys[index]));
    }
    return value;
  };
  const std::string& proof = values[1];
  if (proof != "inclusion" && proof != "consistency")
  {
    throw error(2, "proof must be inclusion or consistency");
  }
  Scenario scenario{
    std::move(values[0]),
    std::move(values[2]),
    std::move(values[3]),
    std::move(values[4]),
    number(5),
    number(6),
    number(7),
    std::nullopt};
  const std::string& second = values[8];

  if (proof == "inclusion")
  {
    if (second != "none")
    {
      throw error(9, "inclusion proof requires second: none");
    }
  }
  else
  {
    if (second == "none")
    {
      throw error(9, "consistency proof requires a second index");
    }
    scenario.second_index = number(8);
  }

  if (path.stem() != scenario.id)
  {
    throw error(1, "name must match the file name");
  }
  if (scenario.order == 0)
  {
    throw error(6, "order must be greater than zero");
  }
  if (scenario.leaves == 0)
  {
    throw error(7, "leaves must be greater than zero");
  }
  if (scenario.focus >= scenario.leaves)
  {
    throw error(8, "focus must be less than leaves");
  }
  if (
    scenario.second_index &&
    (scenario.focus >= *scenario.second_index ||
     *scenario.second_index >= scenario.leaves))
  {
    throw error(9, "invalid consistency indices");
  }
  return scenario;
}

static std::vector<Scenario> read_scenarios(const fs::path& directory)
{
  if (!fs::is_directory(directory))
  {
    throw std::runtime_error("not a scenario directory: " + directory.string());
  }

  std::vector<Scenario> scenarios;
  for (const fs::directory_entry& entry : fs::directory_iterator(directory))
  {
    if (!entry.is_regular_file() || entry.path().extension() != ".scenario")
    {
      continue;
    }
    scenarios.push_back(read_scenario(entry.path()));
  }

  if (scenarios.empty())
  {
    throw std::runtime_error(
      "scenario directory is empty: " + directory.string());
  }
  std::sort(
    scenarios.begin(),
    scenarios.end(),
    [](const Scenario& left, const Scenario& right) {
      return left.order < right.order;
    });
  for (size_t index = 0; index < scenarios.size(); index++)
  {
    if (scenarios[index].order != index + 1)
    {
      throw std::runtime_error(
        "scenario order values must be contiguous from 1");
    }
  }
  return scenarios;
}

static void run_scenario(
  const fs::path& directory,
  Scenario& scenario,
  const std::vector<Hash>& hashes)
{
  TileStore store(directory);
  TileWriter writer(store);
  const auto leaf_at = [&](uint64_t index) -> const Hash& {
    if (index >= hashes.size())
    {
      throw std::runtime_error("leaf index exceeds resident hash range");
    }
    return hashes[static_cast<size_t>(index)];
  };
  writer.write_up_to(scenario.leaves, leaf_at);
  scenario.covered = (scenario.leaves / TILE_WIDTH) * TILE_WIDTH;
  scenario.tile_files = list_tiles(store);

  merkle::Tree frontier;
  for (size_t index = 0; index < scenario.leaves; index++)
  {
    frontier.insert(hashes.at(index));
  }
  frontier.flush_to(std::min(scenario.covered, scenario.leaves - 1));
  scenario.frontier_start = frontier.min_index();

  // An unflushed, tile-free tree, kept only as an independent control for the
  // proofs below.
  merkle::Tree control;
  for (size_t index = 0; index < scenario.leaves; index++)
  {
    control.insert(hashes.at(index));
  }

  const MemoryHashSource memory(frontier);
  const TileHashSource tiles(store, scenario.covered);
  const CombinedHashSource combined(memory, tiles);
  const ProofEngine engine(combined);

  scenario.map_leaves =
    scenario.second_index ? *scenario.second_index + 1 : scenario.leaves;
  scenario.nodes.clear();
  scenario.nodes.reserve(scenario.map_leaves * 2 - 1);
  build_nodes(scenario.nodes, 0, scenario.map_leaves, 0, -1);
  classify_nodes(scenario, tiles, memory);
  fill_digests(scenario);

  // SHA-256 digests are unique, so a proof element identifies its node.
  std::map<std::string, size_t> by_digest;
  for (size_t index = 0; index < scenario.nodes.size(); index++)
  {
    by_digest.emplace(scenario.nodes[index].digest.to_string(), index);
  }
  const auto locate = [&](const Hash& digest) {
    const auto found = by_digest.find(digest.to_string());
    if (found == by_digest.end())
    {
      throw std::runtime_error(
        "proof element is absent from the node map in " + scenario.id);
    }
    return found->second;
  };

  if (!scenario.second_index)
  {
    const Hash root = control.root();
    const auto proof = engine.inclusion_proof(scenario.focus, scenario.leaves);
    if (!proof->verify(root) || *proof != *control.path(scenario.focus))
    {
      throw std::runtime_error("inclusion proof mismatch for " + scenario.id);
    }
    for (size_t index = 0; index < proof->size(); index++)
    {
      scenario.proof_nodes.push_back(locate((*proof)[index]));
    }
    scenario.nodes[locate(hashes.at(scenario.focus))].flags |= FLAG_FIRST;
  }
  else
  {
    const size_t second_index = *scenario.second_index;
    const Hash first_root = *control.past_root(scenario.focus);
    const Hash second_root = *control.past_root(second_index);
    const auto proof =
      engine.consistency_proof_from_indices(scenario.focus, second_index);
    if (
      !ProofEngine::verify_consistency(
        scenario.focus + 1, second_index + 1, first_root, second_root, proof))
    {
      throw std::runtime_error("consistency proof mismatch for " + scenario.id);
    }
    for (const Hash& element : proof)
    {
      scenario.proof_nodes.push_back(locate(element));
    }
    scenario.nodes[locate(hashes.at(scenario.focus))].flags |= FLAG_FIRST;
    scenario.nodes[locate(hashes.at(second_index))].flags |= FLAG_SECOND;
  }

  for (const size_t node : scenario.proof_nodes)
  {
    scenario.nodes[node].flags |= FLAG_PROOF;
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

  stream << R"({"schemaVersion":4,"tileWidth":)" << TILE_WIDTH
         << R"(,"flags":{"tile":1,"frontier":2,"proof":4,)"
         << R"("first":8,"second":16},"scenarios":[)";
  for (size_t scenario_index = 0; scenario_index < scenarios.size();
       scenario_index++)
  {
    const Scenario& scenario = scenarios[scenario_index];
    stream << (scenario_index == 0 ? "{" : ",{");
    stream << "\"id\":" << std::quoted(scenario.id);
    stream << ",\"title\":" << std::quoted(scenario.title);
    stream << ",\"description\":" << std::quoted(scenario.description);
    stream << ",\"takeaway\":" << std::quoted(scenario.takeaway);
    stream << ",\"type\":"
           << std::quoted(scenario.second_index ? "consistency" : "inclusion");
    stream << ",\"leaves\":" << scenario.leaves;
    stream << ",\"focus\":" << scenario.focus;
    if (scenario.second_index)
    {
      stream << ",\"secondIndex\":" << *scenario.second_index;
    }
    stream << ",\"covered\":" << scenario.covered;
    stream << ",\"frontierStart\":" << scenario.frontier_start;
    stream << ",\"mapLeaves\":" << scenario.map_leaves;

    stream << ",\"tiles\":[";
    for (size_t index = 0; index < scenario.tile_files.size(); index++)
    {
      const auto& ref = scenario.tile_files[index];
      stream << (index == 0 ? "[" : ",[")
             << static_cast<unsigned>(ref.first) << ',' << ref.second << ']';
    }
    stream << ']';

    const auto column = [&](const char* name, const auto& select) {
      stream << ",\"" << name << "\":[";
      for (size_t index = 0; index < scenario.nodes.size(); index++)
      {
        if (index != 0)
        {
          stream << ',';
        }
        stream << select(scenario.nodes[index]);
      }
      stream << ']';
    };
    column("lo", [](const Scenario::Node& node) { return node.lo; });
    column("hi", [](const Scenario::Node& node) { return node.hi; });
    column("depth", [](const Scenario::Node& node) { return node.depth; });
    column("parent", [](const Scenario::Node& node) { return node.parent; });
    column("flags", [](const Scenario::Node& node) { return node.flags; });

    stream << ",\"proof\":[";
    for (size_t index = 0; index < scenario.proof_nodes.size(); index++)
    {
      stream << (index == 0 ? "" : ",") << scenario.proof_nodes[index];
    }
    stream << "]}";
  }
  stream << "]}\n";
}

int main(int argc, char** argv)
{
  try
  {
    if (argc != 3)
    {
      throw std::runtime_error(
        "usage: proof_viz SCENARIO_DIRECTORY OUTPUT_JSON");
    }
    const fs::path scenario_directory = argv[1];
    const fs::path output = argv[2];
    std::vector<Scenario> scenarios = read_scenarios(scenario_directory);
    size_t max_leaves = 0;
    for (const Scenario& scenario : scenarios)
    {
      max_leaves = std::max(max_leaves, scenario.leaves);
    }
    const TemporaryDirectory temporary_directory("merklecpp_proof_viz");
    const auto hashes = make_hashes(max_leaves);

    for (Scenario& scenario : scenarios)
    {
      run_scenario(temporary_directory.path() / scenario.id, scenario, hashes);
      std::cout << scenario.id << ": " << scenario.nodes.size() << " nodes, "
                << scenario.proof_nodes.size() << " proof elements, "
                << scenario.tile_files.size() << " tiles\n";
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
