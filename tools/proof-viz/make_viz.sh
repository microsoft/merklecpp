#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(cd "$script_dir/../.." && pwd)
output_dir=${1:-$script_dir}
build_dir=${BUILD_DIR:-$repo_root/build/proof-viz}
build_type=${BUILD_TYPE:-Release}

mkdir -p "$build_dir" "$output_dir"
output_dir=$(cd "$output_dir" && pwd)
cmake -S "$repo_root" -B "$build_dir" \
  -DBUILD_TESTING=OFF \
  "-DCMAKE_BUILD_TYPE=$build_type"
cmake --build "$build_dir" --target proof_viz --config "$build_type"

proof_viz=
for candidate in \
  "$build_dir/proof_viz" \
  "$build_dir/proof_viz.exe" \
  "$build_dir/$build_type/proof_viz" \
  "$build_dir/$build_type/proof_viz.exe"
do
  if [[ -x "$candidate" ]]; then
    proof_viz=$candidate
    break
  fi
done

if [[ -z "$proof_viz" ]]; then
  echo "proof_viz executable not found in $build_dir" >&2
  exit 1
fi

"$proof_viz" "$script_dir/scenarios" "$output_dir/data.json"

node --check "$script_dir/app.js"

if [[ "$output_dir" != "$script_dir" ]]; then
  cp "$script_dir/index.html" "$script_dir/styles.css" "$script_dir/app.js" \
    "$output_dir/"
fi