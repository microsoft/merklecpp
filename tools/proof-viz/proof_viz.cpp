#include "tiles_test_util.h"
#include "util.h"

#include <algorithm>
#include <array>
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
  std::string title;
  std::string description;
  std::string takeaway;
  uint64_t order;
  uint64_t leaves;
  uint64_t focus;
  std::optional<uint64_t> second_index;
  Attempts attempts;
};

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
    uint64_t value = 0;
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
      throw std::runtime_error(
        "unexpected scenario directory entry: " + entry.path().string());
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
  const uint64_t covered = (scenario.leaves / TILE_WIDTH) * TILE_WIDTH;
  uint64_t frontier_start = covered;
  if (frontier_start >= scenario.leaves)
  {
    frontier_start = scenario.leaves - 1;
  }
  if (frontier_start > 0)
  {
    frontier.flush_to(static_cast<size_t>(frontier_start));
  }

  const MemoryHashSource memory(frontier);
  const TileHashSource tiles(store, covered);
  const TracingSource traced_memory(memory, "frontier", scenario.attempts);
  const TracingSource traced_tiles(tiles, "tile", scenario.attempts);
  const CombinedHashSource combined(traced_memory, traced_tiles);
  const ProofEngine engine(combined);
  const CombinedHashSource control_combined(memory, tiles);
  const ProofEngine control_engine(control_combined);

  if (!scenario.second_index)
  {
    const Hash root = oracle.root();
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
    const uint64_t second_index = *scenario.second_index;
    const Hash first_root = *oracle.past_root(scenario.focus);
    const Hash second_root = *oracle.past_root(second_index);
    const auto proof =
      engine.consistency_proof_from_indices(scenario.focus, second_index);
    const auto control_proof = control_engine.consistency_proof_from_indices(
      scenario.focus, second_index);
    if (
      proof != control_proof ||
      !ProofEngine::verify_consistency(
        scenario.focus + 1, second_index + 1, first_root, second_root, proof))
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

  stream << "{\"schemaVersion\":1,\"tileWidth\":" << TILE_WIDTH
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
    stream << ",\"leaves\":" << scenario.leaves;
    stream << ",\"focus\":" << scenario.focus;
    if (scenario.second_index)
    {
      stream << ",\"secondIndex\":" << *scenario.second_index;
    }
    stream << ",\"attempts\":[";
    for (size_t attempt_index = 0; attempt_index < scenario.attempts.size();
         attempt_index++)
    {
      const Attempt& attempt = scenario.attempts[attempt_index];
      stream << (attempt_index == 0 ? "{" : ",{");
      stream << "\"source\":" << std::quoted(attempt.source)
             << ",\"level\":" << static_cast<unsigned>(attempt.level)
             << ",\"index\":" << attempt.index
             << ",\"success\":" << (attempt.success ? "true" : "false") << "}";
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
      max_leaves = std::max(max_leaves, static_cast<size_t>(scenario.leaves));
    }
    const TemporaryDirectory temporary_directory("merklecpp_proof_viz");
    const auto hashes = make_hashes(max_leaves);

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