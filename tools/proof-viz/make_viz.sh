#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(cd "$script_dir/../.." && pwd)
output_dir=${1:-$script_dir}
build_dir=${BUILD_DIR:-$repo_root/build/proof-viz}
cxx=${CXX:-c++}

mkdir -p "$build_dir" "$output_dir"
output_dir=$(cd "$output_dir" && pwd)
"$cxx" -std=c++20 -O2 \
  -I"$repo_root" \
  -I"$repo_root/test" \
  "$script_dir/proof_viz.cpp" \
  -o "$build_dir/proof_viz"
"$build_dir/proof_viz" "$script_dir/scenarios" "$output_dir/data.json"

node --check "$script_dir/app.js"

if [[ "$output_dir" != "$script_dir" ]]; then
  cp "$script_dir/index.html" "$script_dir/styles.css" "$script_dir/app.js" \
    "$output_dir/"
fi