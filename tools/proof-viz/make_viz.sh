#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(cd "$script_dir/../.." && pwd)
output_dir=${1:-$script_dir}
build_dir=${BUILD_DIR:-$repo_root/build/proof-viz}
cxx=${CXX:-c++}

mkdir -p "$build_dir" "$output_dir"
"$cxx" -std=c++20 -O2 \
  -I"$repo_root" \
  -I"$repo_root/test" \
  "$script_dir/proof_viz.cpp" \
  -o "$build_dir/proof_viz"
"$build_dir/proof_viz" "$output_dir/data.js"

node --check "$script_dir/app.js"
node --check "$output_dir/data.js"
node - "$output_dir/data.js" <<'NODE'
const path = require("node:path");

global.window = {};
require(path.resolve(process.argv[2]));

const data = window.PROOF_VIZ_DATA;
const validAttempt = (attempt) =>
  ["frontier", "tile"].includes(attempt.source) &&
  Number.isInteger(attempt.level) && attempt.level >= 0 &&
  Number.isInteger(attempt.index) && attempt.index >= 0 &&
  typeof attempt.success === "boolean";
const validScenario = (scenario) =>
  ["id", "title", "description", "takeaway"].every(
    (field) => typeof scenario[field] === "string"
  ) &&
  Number.isInteger(scenario.leaves) && scenario.leaves > 0 &&
  Number.isInteger(scenario.focus) &&
  scenario.focus >= 0 && scenario.focus < scenario.leaves &&
  (scenario.secondIndex === undefined ||
    (Number.isInteger(scenario.secondIndex) &&
      scenario.secondIndex > scenario.focus &&
      scenario.secondIndex < scenario.leaves)) &&
  Array.isArray(scenario.attempts) &&
  scenario.attempts.length > 0 &&
  scenario.attempts.every(validAttempt);

if (
  !data ||
  !Number.isInteger(data.tileWidth) || data.tileWidth < 1 ||
  !Array.isArray(data.scenarios) || data.scenarios.length === 0 ||
  !data.scenarios.every(validScenario)
) {
  throw new Error("invalid proof visualization data");
}

console.log(`validated ${data.scenarios.length} proof visualization scenarios`);
NODE

cp "$script_dir/index.html" "$script_dir/styles.css" "$script_dir/app.js" \
  "$output_dir/"