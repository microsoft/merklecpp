#!/usr/bin/env python3
# Copyright (c) Microsoft Corporation.
# Licensed under the MIT License.

"""Validate tile-performance artifacts and render a GitHub job summary."""

from __future__ import annotations

import argparse
import csv
import json
from pathlib import Path
from typing import Any


EVENT_COLUMNS = [
    "series",
    "event_index",
    "cycle",
    "leaf_count",
    "operation",
    "duration_ns",
    "rollup",
]
CYCLE_COLUMNS = [
    "cycle",
    "leaf_count",
    "rollup",
    "tiled_append_total_ns",
    "tiled_flush_ns",
    "tiled_compact_ns",
    "tiled_fifo_hits",
    "tiled_fifo_misses",
    "control_append_total_ns",
    "control_flush_ns",
]


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def as_nonnegative_int(value: Any, name: str) -> int:
    require(isinstance(value, int) and value >= 0, f"{name} must be nonnegative")
    return value


def validate_distribution(value: Any, name: str) -> dict[str, int]:
    require(isinstance(value, dict), f"{name} must be an object")
    keys = ("min_ns", "p50_ns", "p99_ns", "max_ns")
    result = {key: as_nonnegative_int(value.get(key), f"{name}.{key}") for key in keys}
    require(
        result["min_ns"] <= result["p50_ns"] <= result["p99_ns"] <= result["max_ns"],
        f"{name} percentiles are not ordered",
    )
    return result


def load_summary(path: Path) -> dict[str, Any]:
    with path.open(encoding="utf-8") as source:
        summary = json.load(source)
    require(summary.get("schema_version") == 1, "unsupported summary schema")
    width = as_nonnegative_int(summary.get("tile_width"), "tile_width")
    cycles = as_nonnegative_int(summary.get("cycles"), "cycles")
    appends = as_nonnegative_int(summary.get("appends"), "appends")
    as_nonnegative_int(summary.get("append_timer_overhead_ns"), "append_timer_overhead_ns")
    require(width > 1 and width & (width - 1) == 0, "tile_width must be a power of two")
    require(cycles == 512, "continuous benchmark must contain 512 cycles")
    require(appends == cycles * width == 131_072, "unexpected append count")
    require(
        isinstance(summary.get("root"), str) and len(summary["root"]) == 64,
        "root must be a full SHA-256 hash",
    )

    tiled = summary.get("tiled")
    control = summary.get("tree_control")
    breakdown = summary.get("rollup_breakdown")
    require(isinstance(tiled, dict), "tiled summary is missing")
    require(isinstance(control, dict), "tree_control summary is missing")
    require(isinstance(breakdown, dict), "rollup_breakdown is missing")
    for key in ("append", "flush_normal", "flush_rollup", "compact"):
        validate_distribution(tiled.get(key), f"tiled.{key}")
    for key in ("append", "flush"):
        validate_distribution(control.get(key), f"tree_control.{key}")
    for key in (
        "level0_write_ns",
        "read_child_tiles_ns",
        "perfect_root_hashes_ns",
        "level1_write_ns",
    ):
        as_nonnegative_int(breakdown.get(key), f"rollup_breakdown.{key}")
    require(breakdown.get("child_tiles") == width, "breakdown child tile count mismatch")
    require(
        breakdown.get("parent_hashes") == width * (width - 1),
        "breakdown parent hash count mismatch",
    )
    return summary


def read_csv(path: Path, columns: list[str]) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as source:
        reader = csv.DictReader(source)
        require(reader.fieldnames == columns, f"{path.name} columns do not match schema")
        rows = list(reader)
    return rows


def parse_event_number(row: dict[str, str], key: str) -> int:
    try:
        value = int(row[key])
    except (KeyError, ValueError) as exc:
        raise ValueError(f"invalid event {key}") from exc
    require(value >= 0, f"event {key} must be nonnegative")
    return value


def validate_events(path: Path, summary: dict[str, Any]) -> None:
    rows = read_csv(path, EVENT_COLUMNS)
    appends = summary["appends"]
    cycles = summary["cycles"]
    expected = appends * 2 + cycles * 3
    require(len(rows) == expected, f"expected {expected} events, got {len(rows)}")

    counts: dict[tuple[str, str], int] = {}
    rollups = 0
    for expected_index, row in enumerate(rows):
        require(
            parse_event_number(row, "event_index") == expected_index,
            "event_index must be contiguous",
        )
        parse_event_number(row, "cycle")
        parse_event_number(row, "leaf_count")
        parse_event_number(row, "duration_ns")
        require(row["series"] in ("tiled", "tree-control"), "unknown event series")
        require(row["operation"] in ("append", "flush", "compact"), "unknown operation")
        require(row["rollup"] in ("0", "1"), "rollup must be 0 or 1")
        key = (row["series"], row["operation"])
        counts[key] = counts.get(key, 0) + 1
        if row["rollup"] == "1":
            require(key == ("tiled", "flush"), "only tiled flushes mark roll-ups")
            rollups += 1

    require(counts.get(("tiled", "append")) == appends, "tiled append count mismatch")
    require(counts.get(("tiled", "flush")) == cycles, "tiled flush count mismatch")
    require(counts.get(("tiled", "compact")) == cycles, "tiled compact count mismatch")
    require(counts.get(("tree-control", "append")) == appends, "control append count mismatch")
    require(counts.get(("tree-control", "flush")) == cycles, "control flush count mismatch")
    require(rollups == cycles // summary["tile_width"], "roll-up marker count mismatch")


def validate_cycles(path: Path, summary: dict[str, Any]) -> None:
    rows = read_csv(path, CYCLE_COLUMNS)
    require(len(rows) == summary["cycles"], "cycle row count mismatch")
    width = summary["tile_width"]
    for index, row in enumerate(rows):
        require(int(row["cycle"]) == index, "cycle indices must be contiguous")
        require(int(row["leaf_count"]) == (index + 1) * width, "cycle leaf count mismatch")
        is_rollup = (index + 1) % width == 0
        require(int(row["rollup"]) == int(is_rollup), "cycle roll-up marker mismatch")
        for key in CYCLE_COLUMNS[3:]:
            require(int(row[key]) >= 0, f"{key} must be nonnegative")
        require(int(row["tiled_fifo_misses"]) == 0, "continuous writer unexpectedly missed FIFO")
        require(
            int(row["tiled_fifo_hits"]) == (width if is_rollup else 0),
            "continuous writer FIFO hit count mismatch",
        )


def validate_viewer(path: Path) -> None:
    source = path.read_text(encoding="utf-8")
    for token in (
        "<canvas",
        "linear",
        "log",
        "rollup",
        "#276be9",
        "#00843f",
        "#f66a0a",
        "#d72d47",
    ):
        require(token in source, f"viewer is missing {token!r}")


def duration(value: int) -> str:
    if value < 1_000:
        return f"{value} ns"
    if value < 1_000_000:
        return f"{value / 1_000:.3f} us"
    return f"{value / 1_000_000:.3f} ms"


def markdown(summary: dict[str, Any]) -> str:
    tiled = summary["tiled"]
    control = summary["tree_control"]
    breakdown = summary["rollup_breakdown"]
    lines = [
        "### Continuous tiled-storage exploration",
        "",
        f"{summary['cycles']} cycles, {summary['appends']:,} appends, "
        f"tile width {summary['tile_width']}. Roots verified against the plain tree control.",
        "",
        "| Series / operation | p50 | p99 |",
        "|---|---:|---:|",
    ]
    for label, value in (
        ("Tiled append", tiled["append"]),
        ("Tiled normal flush", tiled["flush_normal"]),
        ("Tiled roll-up flush", tiled["flush_rollup"]),
        ("Tiled compact", tiled["compact"]),
        ("Tree control append", control["append"]),
        ("Tree control `flush_to(max_index())`", control["flush"]),
    ):
        lines.append(
            f"| {label} | {duration(value['p50_ns'])} | "
            f"{duration(value['p99_ns'])} |"
        )
    lines.extend(
        [
            "",
            "| Roll-up phase | Duration |",
            "|---|---:|",
            f"| Level-0 durable write | {duration(breakdown['level0_write_ns'])} |",
            f"| Read {breakdown['child_tiles']} child tiles | "
            f"{duration(breakdown['read_child_tiles_ns'])} |",
            f"| {breakdown['parent_hashes']:,} SHA-256 parent hashes | "
            f"{duration(breakdown['perfect_root_hashes_ns'])} |",
            f"| Level-1 durable write | {duration(breakdown['level1_write_ns'])} |",
            "",
            f"Per-append values subtract a calibrated "
            f"{summary['append_timer_overhead_ns']} ns steady-clock overhead.",
            "",
            "The downloadable artifact contains the raw CSV/JSON and interactive viewer.",
        ]
    )
    return "\n".join(lines)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("output_dir", type=Path)
    parser.add_argument("--viewer", required=True, type=Path)
    parser.add_argument("--markdown", action="store_true")
    args = parser.parse_args()

    summary = load_summary(args.output_dir / "tile-performance-summary.json")
    validate_events(args.output_dir / "tile-performance-events.csv", summary)
    validate_cycles(args.output_dir / "tile-performance-cycles.csv", summary)
    validate_viewer(args.viewer)
    if args.markdown:
        print(markdown(summary))
    else:
        print("tile performance artifacts: OK")


if __name__ == "__main__":
    main()
