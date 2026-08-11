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
using merkle::tiles::MemoryHashSource;
using merkle::tiles::ProofEngine;
using merkle::tiles::TILE_WIDTH;
using merkle::tiles::TileHashSource;
using merkle::tiles::TileStore;
using merkle::tiles::TileWriter;

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
    size_t lo;
    size_t hi;
    size_t depth;
    int64_t parent;
    const char* source;
    bool overlap = false;
    bool proof = false;
    char endpoint = '\0';
  };
  std::vector<Node> nodes;
};

// NOLINTNEXTLINE(misc-no-recursion) -- mirrors the bounded binary node tree.
static void add_node(
  Scenario& scenario, size_t lo, size_t hi, size_t depth, int64_t parent)
{
  const size_t width = hi - lo;
  const bool complete = std::has_single_bit(width) && lo % width == 0;
  const bool in_frontier = complete && lo >= scenario.frontier_start;
  const bool in_tiles = complete && hi <= scenario.covered;
  const char* source = "computed";
  if (in_frontier)
  {
    source = "frontier";
  }
  else if (in_tiles)
  {
    source = "tile";
  }
  const size_t index = scenario.nodes.size();
  scenario.nodes.push_back(Scenario::Node{
    lo,
    hi,
    depth,
    parent,
    source,
    in_frontier && in_tiles});

  if (width > 1)
  {
    const size_t split = lo + std::bit_floor(width - 1);
    add_node(scenario, lo, split, depth + 1, static_cast<int64_t>(index));
    add_node(scenario, split, hi, depth + 1, static_cast<int64_t>(index));
  }
}

static Scenario::Node* find_node(Scenario& scenario, size_t lo, size_t hi)
{
  const auto found = std::find_if(
    scenario.nodes.begin(),
    scenario.nodes.end(),
    [&](const Scenario::Node& node) { return node.lo == lo && node.hi == hi; });
  return found == scenario.nodes.end() ? nullptr : &*found;
}

static Scenario::Node& node_at(Scenario& scenario, size_t lo, size_t hi)
{
  if (Scenario::Node* node = find_node(scenario, lo, hi))
  {
    return *node;
  }
  throw std::runtime_error(
    "proof range [" + std::to_string(lo) + ", " + std::to_string(hi) +
    ") is absent from node map");
}

// NOLINTNEXTLINE(misc-no-recursion) -- follows one bounded root-to-leaf path.
static void mark_inclusion(
  Scenario& scenario, size_t lo, size_t hi, size_t focus)
{
  if (hi - lo == 1)
  {
    return;
  }

  const size_t split = lo + std::bit_floor(hi - lo - 1);
  if (focus < split)
  {
    node_at(scenario, split, hi).proof = true;
    mark_inclusion(scenario, lo, split, focus);
  }
  else
  {
    node_at(scenario, lo, split).proof = true;
    mark_inclusion(scenario, split, hi, focus);
  }
}

// NOLINTNEXTLINE(misc-no-recursion) -- mirrors the bounded consistency proof.
static void mark_consistency(
  Scenario& scenario,
  size_t first_size,
  size_t lo,
  size_t hi,
  bool complete)
{
  if (first_size == hi - lo)
  {
    if (!complete)
    {
      node_at(scenario, lo, hi).proof = true;
    }
    return;
  }

  const size_t split = lo + std::bit_floor(hi - lo - 1);
  const size_t left_size = split - lo;
  if (first_size <= left_size)
  {
    mark_consistency(scenario, first_size, lo, split, complete);
    node_at(scenario, split, hi).proof = true;
  }
  else
  {
    mark_consistency(
      scenario, first_size - left_size, split, hi, false);
    node_at(scenario, lo, split).proof = true;
  }
}

static void derive_presentation(Scenario& scenario, size_t proof_size)
{
  scenario.map_leaves =
    scenario.second_index ? *scenario.second_index + 1 : scenario.leaves;
  scenario.nodes.clear();
  scenario.nodes.reserve(scenario.map_leaves * 2 - 1);
  add_node(scenario, 0, scenario.map_leaves, 0, -1);

  if (scenario.second_index)
  {
    mark_consistency(
      scenario, scenario.focus + 1, 0, scenario.map_leaves, true);
    node_at(scenario, scenario.focus, scenario.focus + 1).endpoint = 'A';
    node_at(
      scenario, *scenario.second_index, *scenario.second_index + 1)
      .endpoint = 'B';
  }
  else
  {
    mark_inclusion(scenario, 0, scenario.map_leaves, scenario.focus);
    node_at(scenario, scenario.focus, scenario.focus + 1).endpoint = 'T';
  }

  const auto marked_proof_nodes = static_cast<size_t>(std::count_if(
    scenario.nodes.begin(),
    scenario.nodes.end(),
    [](const Scenario::Node& node) { return node.proof; }));
  if (marked_proof_nodes != proof_size)
  {
    throw std::runtime_error(
      "proof role mismatch for " + scenario.id + ": expected " +
      std::to_string(proof_size) + ", marked " +
      std::to_string(marked_proof_nodes));
  }
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
    std::nullopt,
    {}};
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
  scenario.frontier_start =
    std::min(scenario.covered, scenario.leaves - 1);

  merkle::Tree oracle;
  merkle::Tree frontier;
  for (size_t index = 0; index < scenario.leaves; index++)
  {
    oracle.insert(hashes.at(index));
    frontier.insert(hashes.at(index));
  }
  if (scenario.frontier_start > 0)
  {
    frontier.flush_to(scenario.frontier_start);
  }

  const MemoryHashSource memory(frontier);
  const TileHashSource tiles(store, scenario.covered);
  const CombinedHashSource combined(memory, tiles);
  const ProofEngine engine(combined);

  if (!scenario.second_index)
  {
    const Hash root = oracle.root();
    const auto proof = engine.inclusion_proof(scenario.focus, scenario.leaves);
    if (!proof->verify(root) || *proof != *oracle.path(scenario.focus))
    {
      throw std::runtime_error("inclusion proof mismatch for " + scenario.id);
    }
    derive_presentation(scenario, proof->size());
  }
  else
  {
    const size_t second_index = *scenario.second_index;
    const Hash first_root = *oracle.past_root(scenario.focus);
    const Hash second_root = *oracle.past_root(second_index);
    const auto proof =
      engine.consistency_proof_from_indices(scenario.focus, second_index);
    if (
      !ProofEngine::verify_consistency(
        scenario.focus + 1, second_index + 1, first_root, second_root, proof))
    {
      throw std::runtime_error("consistency proof mismatch for " + scenario.id);
    }
    derive_presentation(scenario, proof.size());
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

  stream << R"({"schemaVersion":3,"tileWidth":)" << TILE_WIDTH
         << ",\"scenarios\":[";
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
           << std::quoted(
                scenario.second_index ? "consistency" : "inclusion");
    stream << ",\"leaves\":" << scenario.leaves;
    stream << ",\"focus\":" << scenario.focus;
    if (scenario.second_index)
    {
      stream << ",\"secondIndex\":" << *scenario.second_index;
    }
    stream << ",\"covered\":" << scenario.covered;
    stream << ",\"frontierStart\":" << scenario.frontier_start;
    stream << ",\"mapLeaves\":" << scenario.map_leaves;
    stream << ",\"nodes\":[";
    for (size_t node_index = 0; node_index < scenario.nodes.size(); node_index++)
    {
      const Scenario::Node& node = scenario.nodes[node_index];
      stream << (node_index == 0 ? "{" : ",{");
      stream << "\"lo\":" << node.lo << ",\"hi\":" << node.hi;
      stream << ",\"depth\":" << node.depth << ",\"parent\":" << node.parent;
      stream << ",\"source\":" << std::quoted(node.source);
      stream << ",\"overlap\":" << (node.overlap ? "true" : "false");
      stream << ",\"proof\":" << (node.proof ? "true" : "false");
      stream << ",\"endpoint\":";
      if (node.endpoint == '\0')
      {
        stream << "\"\"";
      }
      else
      {
        stream << '"' << node.endpoint << '"';
      }
      stream << "}";
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
      std::cout << scenario.id << ": " << scenario.nodes.size() << " nodes\n";
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