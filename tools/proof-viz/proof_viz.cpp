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
static constexpr uint32_t FLAG_IN_FIRST_TREE = 1U << 5;
static constexpr uint32_t FLAG_IN_SECOND_TREE = 1U << 6;

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
    int64_t parent = -1;
    int64_t left = -1;
    int64_t right = -1;
    uint32_t flags = 0;
    Hash digest;
    bool resolved = false;
    uint32_t height = 0;
    bool measured = false;
  };
  std::vector<Node> nodes;

  /// Node ids of the returned proof elements, in the order the proof lists
  /// them.
  std::vector<size_t> proof_nodes;

  /// Node ids of the two roots the proof reconciles. For an inclusion proof
  /// both name the single tree that was proven against.
  size_t first_root = 0;
  size_t second_root = 0;

  /// The full tiles that reached disk, as (level, index) pairs.
  std::vector<std::pair<uint8_t, uint64_t>> tile_files;

  /// Node id of each distinct range, so the two trees share their nodes.
  std::map<std::pair<uint64_t, uint64_t>, size_t> by_range;
};

/// @brief Adds the RFC 6962 decomposition of [lo, hi) to the node map,
/// splitting at the largest power of two below the width.
/// @note A consistency proof reconciles two trees, and the smaller one has a
/// right spine of its own: the ranges the verifier folds proof elements into
/// to recover the first root. Those ranges are absent from the larger tree
/// whenever its size is not an aligned power of two, so both decompositions
/// are merged here and each node records which trees it belongs to. The split
/// point depends only on the range, so a range shared by both trees has the
/// same children in both and is stored once.
// NOLINTNEXTLINE(misc-no-recursion) -- mirrors the bounded binary node tree.
static size_t decompose(
  Scenario& scenario,
  uint64_t lo,
  uint64_t hi,
  int64_t parent,
  uint32_t membership)
{
  const auto key = std::make_pair(lo, hi);
  const auto existing = scenario.by_range.find(key);
  if (existing != scenario.by_range.end())
  {
    Scenario::Node& node = scenario.nodes[existing->second];
    if ((node.flags & membership) != 0)
    {
      return existing->second;
    }
    node.flags |= membership;
  }
  else
  {
    const size_t added = scenario.nodes.size();
    scenario.nodes.push_back(Scenario::Node{lo, hi, parent});
    scenario.nodes[added].flags = membership;
    scenario.by_range.emplace(key, added);
  }

  const size_t index = scenario.by_range.at(key);
  if (hi - lo > 1)
  {
    const uint64_t split = lo + std::bit_floor(hi - lo - 1);
    const auto left = decompose(
      scenario, lo, split, static_cast<int64_t>(index), membership);
    const auto right = decompose(
      scenario, split, hi, static_cast<int64_t>(index), membership);
    scenario.nodes[index].left = static_cast<int64_t>(left);
    scenario.nodes[index].right = static_cast<int64_t>(right);
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

/// @brief Fills in the digest of a range no store could serve directly.
/// @note These are exactly the ranges the proof engine has to recombine. The
/// two merged decompositions share nodes, so a node can precede its children;
/// recursing rather than sweeping the vector keeps the fold order correct.
// NOLINTNEXTLINE(misc-no-recursion) -- bounded by the node tree depth.
static const Hash& fill_digest(Scenario& scenario, size_t index)
{
  if (scenario.nodes[index].resolved)
  {
    return scenario.nodes[index].digest;
  }
  const int64_t left = scenario.nodes[index].left;
  const int64_t right = scenario.nodes[index].right;
  if (left < 0 || right < 0)
  {
    throw std::runtime_error("unresolved leaf range in " + scenario.id);
  }

  const Hash lo = fill_digest(scenario, static_cast<size_t>(left));
  const Hash hi = fill_digest(scenario, static_cast<size_t>(right));
  merkle::sha256(lo, hi, scenario.nodes[index].digest);
  scenario.nodes[index].resolved = true;
  return scenario.nodes[index].digest;
}

/// @brief Measures each node's distance to the leaves below it.
/// @note Positions are drawn from this rather than from depth so that every
/// leaf shares a baseline. The decomposition is unbalanced, so a ragged
/// right-hand range sits fewer splits below the root than a perfect one, and
/// ranking by depth would leave its leaves floating above the others.
// NOLINTNEXTLINE(misc-no-recursion) -- bounded by the node tree depth.
static uint32_t measure_height(Scenario& scenario, size_t index)
{
  if (scenario.nodes[index].measured)
  {
    return scenario.nodes[index].height;
  }
  const int64_t left = scenario.nodes[index].left;
  const int64_t right = scenario.nodes[index].right;

  uint32_t height = 0;
  if (left >= 0 && right >= 0)
  {
    height = 1 +
      std::max(
               measure_height(scenario, static_cast<size_t>(left)),
               measure_height(scenario, static_cast<size_t>(right)));
  }
  scenario.nodes[index].height = height;
  scenario.nodes[index].measured = true;
  return height;
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
  const auto fail = [&](const std::string& message) {
    return std::runtime_error(path.string() + ": " + message);
  };

  // make_viz.sh checks the lexical shape of these files, so only the
  // constraints the model itself depends on are enforced here.
  std::array<std::string, keys.size()> values;
  for (size_t index = 0; index < keys.size(); index++)
  {
    std::string line;
    const std::string prefix = std::string(keys[index]) + ": ";
    if (!std::getline(stream, line) || !line.starts_with(prefix))
    {
      throw fail("expected key " + std::string(keys[index]));
    }
    values[index] = line.substr(prefix.size());
    if (!values[index].empty() && values[index].back() == '\r')
    {
      values[index].pop_back();
    }
  }

  const auto number = [&](size_t index) {
    const std::string& text = values[index];
    size_t value = 0;
    const auto [end, parse_error] =
      std::from_chars(text.data(), text.data() + text.size(), value);
    if (parse_error != std::errc{} || end != text.data() + text.size())
    {
      throw fail("invalid number for " + std::string(keys[index]));
    }
    return value;
  };

  const bool consistency = values[1] == "consistency";
  if (!consistency && values[1] != "inclusion")
  {
    throw fail("proof must be inclusion or consistency");
  }
  if (consistency != (values[8] != "none"))
  {
    throw fail("a second index is required for consistency proofs only");
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

  if (scenario.focus >= scenario.leaves)
  {
    throw fail("focus must be less than leaves");
  }
  if (consistency)
  {
    scenario.second_index = number(8);
    if (
      scenario.focus >= *scenario.second_index ||
      *scenario.second_index >= scenario.leaves)
    {
      throw fail("consistency needs focus < second < leaves");
    }
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
  scenario.by_range.clear();
  scenario.nodes.reserve(scenario.map_leaves * 2);
  scenario.second_root =
    decompose(scenario, 0, scenario.map_leaves, -1, FLAG_IN_SECOND_TREE);
  scenario.first_root = scenario.second_index ?
    decompose(scenario, 0, scenario.focus + 1, -1, FLAG_IN_FIRST_TREE) :
    scenario.second_root;
  classify_nodes(scenario, tiles, memory);
  for (size_t index = 0; index < scenario.nodes.size(); index++)
  {
    fill_digest(scenario, index);
    measure_height(scenario, index);
  }

  // Edges describe the drawn tree only. A node that exists solely in the first
  // tree has a parent there but not here, so it is left unattached and drawn
  // on its own rather than joined into a structure it is not part of.
  for (Scenario::Node& node : scenario.nodes)
  {
    if ((node.flags & FLAG_IN_SECOND_TREE) == 0)
    {
      node.parent = -1;
    }
  }

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
    if (scenario.nodes[scenario.second_root].digest != root)
    {
      throw std::runtime_error("root mismatch for " + scenario.id);
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
    // Both trees are drawn, so both roots must be nodes of the map: the folds
    // a verifier performs all land on ranges that are on screen.
    if (
      scenario.nodes[scenario.first_root].digest != first_root ||
      scenario.nodes[scenario.second_root].digest != second_root)
    {
      throw std::runtime_error("root mismatch for " + scenario.id);
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

  stream << R"({"schemaVersion":5,"tileWidth":)" << TILE_WIDTH
         << R"(,"flags":{"tile":1,"frontier":2,"proof":4,"first":8,)"
         << R"("second":16,"inFirstTree":32,"inSecondTree":64},"scenarios":[)";
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
    stream << ",\"firstRoot\":" << scenario.first_root;
    stream << ",\"secondRoot\":" << scenario.second_root;

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
    column("height", [](const Scenario::Node& node) { return node.height; });
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
