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
  -DBUILD_TESTING=ON \
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

# Lexical checks on the scenario files. proof_viz enforces only what the model
# depends on, so the strict shape (exact key order, no stray whitespace or
# control characters, no extra lines) is verified here.
scenario_keys="name proof title description takeaway order leaves focus second"
for scenario in "$script_dir"/scenarios/*.scenario; do
  mapfile -t lines < <(tr -d '\r' < "$scenario")
  if [[ ${#lines[@]} -ne 9 ]]; then
    echo "$scenario: expected 9 lines, found ${#lines[@]}" >&2
    exit 1
  fi
  index=0
  for key in $scenario_keys; do
    line=${lines[$index]}
    if [[ $line != "$key: "* ]]; then
      echo "$scenario:$((index + 1)): expected key '$key'" >&2
      exit 1
    fi
    value=${line#"$key": }
    if [[ -z $value || $value != "$(printf '%s' "$value" | tr -d '[:cntrl:]')" ]]; then
      echo "$scenario:$((index + 1)): invalid value for '$key'" >&2
      exit 1
    fi
    if [[ $value != "$(printf '%s' "$value" | sed -e 's/^ *//' -e 's/ *$//')" ]]; then
      echo "$scenario:$((index + 1)): '$key' has leading or trailing space" >&2
      exit 1
    fi
    index=$((index + 1))
  done
  if [[ ${lines[0]} != "name: $(basename "$scenario" .scenario)" ]]; then
    echo "$scenario:1: name must match the file name" >&2
    exit 1
  fi
done

node --check "$script_dir/app.js"

if [[ "$output_dir" != "$script_dir" ]]; then
  cp "$script_dir/index.html" "$script_dir/styles.css" "$script_dir/app.js" \
    "$output_dir/"
fi