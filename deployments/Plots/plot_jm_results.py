#!/usr/bin/env python3
from __future__ import annotations

import argparse
import csv
import json
import math
import re
import sys
from dataclasses import dataclass
from datetime import datetime, timedelta
from pathlib import Path
from typing import Iterable, Optional

import matplotlib
matplotlib.use("Agg")
import matplotlib.dates as mdates
import matplotlib.pyplot as plt


@dataclass
class Table:
    time_raw: list[float]
    time_plot: list
    headers: list[str]
    columns: dict[str, list[float]]


def clean_name(value: str) -> str:
    value = re.sub(r"\([^)]*\)", " ", value.strip().lower())
    value = value.replace("_", " ").replace("-", " ")
    return re.sub(r"\s+", " ", value).strip()


def finite_float(value: object) -> float:
    try:
        result = float(str(value).strip())
        return result if math.isfinite(result) else float("nan")
    except Exception:
        return float("nan")


def detect_delimiter(path: Path) -> str:
    sample = path.read_text(encoding="utf-8", errors="replace")[:8192]
    try:
        return csv.Sniffer().sniff(sample, delimiters=",;\t").delimiter
    except csv.Error:
        return ","


def find_header_index(headers: list[str], aliases: Iterable[str]) -> Optional[int]:
    cleaned = [clean_name(h) for h in headers]
    aliases_clean = [clean_name(a) for a in aliases]

    for alias in aliases_clean:
        for i, header in enumerate(cleaned):
            if header == alias:
                return i

    for alias in aliases_clean:
        if len(alias) < 3:
            continue
        for i, header in enumerate(cleaned):
            if alias in header:
                return i
    return None


def excel_serial_to_datetime(value: float) -> datetime:
    return datetime(1899, 12, 30) + timedelta(days=value)


def convert_time(values: list[float]) -> tuple[list, str]:
    valid = [v for v in values if math.isfinite(v)]
    if not valid:
        return values, "numeric"

    median = sorted(valid)[len(valid) // 2]

    # Excel serial dates normally used by OpenHydroQual selected_output.csv.
    if 20000 <= median <= 80000:
        return [
            excel_serial_to_datetime(v) if math.isfinite(v) else None
            for v in values
        ], "datetime"

    # Unix seconds.
    if median > 100_000_000:
        return [
            datetime.fromtimestamp(v) if math.isfinite(v) else None
            for v in values
        ], "datetime"

    return values, "numeric"


def read_table(path: Path, config: dict) -> Table:
    delimiter = detect_delimiter(path)
    with path.open(newline="", encoding="utf-8", errors="replace") as stream:
        rows = list(csv.reader(stream, delimiter=delimiter))

    if not rows:
        raise ValueError(f"Empty CSV: {path}")

    headers = rows[0]
    data_rows = [row for row in rows[1:] if row]

    time_index = find_header_index(headers, config.get("time_aliases", []))
    if time_index is None:
        # OpenHydroQual frequently places a time column immediately before
        # each requested output. Prefer a literal t column, otherwise column 1.
        literal_t = [i for i, h in enumerate(headers) if clean_name(h) == "t"]
        time_index = literal_t[0] if literal_t else 0

    time_raw = [
        finite_float(row[time_index] if time_index < len(row) else "nan")
        for row in data_rows
    ]
    time_plot, _ = convert_time(time_raw)

    columns: dict[str, list[float]] = {}
    for spec in config.get("series", []):
        index = find_header_index(headers, spec.get("aliases", []))
        if index is None:
            continue
        columns[spec["key"]] = [
            finite_float(row[index] if index < len(row) else "nan")
            for row in data_rows
        ]

    return Table(time_raw=time_raw, time_plot=time_plot, headers=headers, columns=columns)


def valid_xy(table: Table, key: str):
    y = table.columns.get(key, [])
    pairs = [
        (x, value)
        for x, value in zip(table.time_plot, y)
        if x is not None and math.isfinite(value)
    ]
    if not pairs:
        return [], []
    return [p[0] for p in pairs], [p[1] for p in pairs]


def common_series(config: dict, truth: Table, assimilation: Optional[Table]):
    output = []
    for spec in config.get("series", []):
        key = spec["key"]
        if key not in truth.columns:
            continue
        if not any(math.isfinite(v) for v in truth.columns[key]):
            continue
        output.append(spec)
    return output


def set_time_axis(axis, datetime_mode: bool):
    if datetime_mode:
        locator = mdates.AutoDateLocator(minticks=4, maxticks=8)
        axis.xaxis.set_major_locator(locator)
        axis.xaxis.set_major_formatter(mdates.ConciseDateFormatter(locator))
    axis.grid(True, alpha=0.25)


def draw_series(axis, spec, truth: Table, assimilation: Optional[Table]):
    key = spec["key"]
    x, y = valid_xy(truth, key)
    axis.plot(x, y, linewidth=1.4, label="Truth")

    if assimilation is not None and key in assimilation.columns:
        xa, ya = valid_xy(assimilation, key)
        if xa:
            axis.plot(xa, ya, linewidth=1.2, linestyle="--", label="Assimilation")

    label = spec.get("label", key)
    unit = spec.get("unit", "")
    axis.set_ylabel(f"{label} ({unit})" if unit else label)
    if assimilation is not None:
        axis.legend(loc="best", frameon=False)


def save_multi_panel(
    output: Path,
    specs: list[dict],
    truth: Table,
    assimilation: Optional[Table],
    title: str,
    xlim=None,
):
    if not specs:
        return

    n = len(specs)
    fig, axes = plt.subplots(
        nrows=n,
        ncols=1,
        figsize=(12, max(3.0 * n, 4.5)),
        sharex=True,
        constrained_layout=True,
    )
    if n == 1:
        axes = [axes]

    datetime_mode = bool(
        truth.time_plot and isinstance(next((x for x in truth.time_plot if x is not None), None), datetime)
    )

    for axis, spec in zip(axes, specs):
        draw_series(axis, spec, truth, assimilation)
        set_time_axis(axis, datetime_mode)
        if xlim is not None:
            axis.set_xlim(*xlim)

    axes[-1].set_xlabel("Time")
    fig.suptitle(title, fontsize=14)
    fig.savefig(output, dpi=240, bbox_inches="tight")
    plt.close(fig)
    print(f"[ok] wrote {output}")


def write_inventory(path: Path, config: dict, truth: Table, assimilation: Optional[Table]):
    configured = {s["key"]: s for s in config.get("series", [])}
    lines = [
        "# JM plot-variable inventory",
        "",
        "## Truth CSV headers",
        "",
    ]
    lines.extend(f"- `{h}`" for h in truth.headers)
    lines += ["", "## Matched configured series", ""]
    for key in truth.columns:
        lines.append(f"- `{key}` → {configured.get(key, {}).get('label', key)}")

    missing = [s for s in config.get("series", []) if s["key"] not in truth.columns]
    lines += ["", "## Configured series not found", ""]
    lines.extend(f"- `{s['key']}`: {', '.join(s.get('aliases', []))}" for s in missing)

    if assimilation is not None:
        lines += ["", "## Assimilation overlay", "", "Enabled."]
    else:
        lines += ["", "## Assimilation overlay", "", "Disabled or unavailable."]

    path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"[ok] wrote {path}")


def main() -> int:
    parser = argparse.ArgumentParser(description="Generate JM truth and optional assimilation plots.")
    parser.add_argument("--truth", type=Path, required=True)
    parser.add_argument("--assimilation", type=Path)
    parser.add_argument("--output-dir", type=Path, default=Path("JM_plot_results"))
    parser.add_argument("--window-days", type=float, default=10.0)
    parser.add_argument("--config", type=Path, default=Path("jm_plot_config.json"))
    args = parser.parse_args()

    config = json.loads(args.config.read_text(encoding="utf-8"))
    truth = read_table(args.truth, config)
    assimilation = read_table(args.assimilation, config) if args.assimilation else None

    args.output_dir.mkdir(parents=True, exist_ok=True)
    specs = common_series(config, truth, assimilation)

    write_inventory(
        args.output_dir / "jm_variable_inventory.md",
        config,
        truth,
        assimilation,
    )

    if not specs:
        print("[error] No configured JM variables matched the truth CSV.", file=sys.stderr)
        print("[error] See jm_variable_inventory.md and update aliases in jm_plot_config.json.", file=sys.stderr)
        return 1

    state_keys = {
        "catch_basin_stage",
        "surface_depth",
        "soil_moisture",
        "groundwater",
    }
    flow_keys = {
        "precipitation",
        "catchment_runoff",
        "gutter_flow",
        "catch_basin_inflow",
        "facility_inflow",
        "underdrain_flow",
        "infiltration",
        "overflow",
    }

    states = [s for s in specs if s["key"] in state_keys]
    flows = [s for s in specs if s["key"] in flow_keys]
    remaining = [s for s in specs if s not in states and s not in flows]

    suffix = "truth_vs_assimilation" if assimilation else "truth"

    save_multi_panel(
        args.output_dir / f"paper_jm_states_full_{suffix}.png",
        states or specs,
        truth,
        assimilation,
        "JM hydraulic states — full period",
    )
    save_multi_panel(
        args.output_dir / f"paper_jm_flows_full_{suffix}.png",
        flows or specs,
        truth,
        assimilation,
        "JM hydrologic and hydraulic fluxes — full period",
    )
    if remaining:
        save_multi_panel(
            args.output_dir / f"paper_jm_additional_full_{suffix}.png",
            remaining,
            truth,
            assimilation,
            "JM additional outputs — full period",
        )

    valid_times = [v for v in truth.time_raw if math.isfinite(v)]
    if valid_times:
        minimum = min(valid_times)
        maximum = max(valid_times)
        width = min(args.window_days, max(0.0, maximum - minimum))
        early_raw = (minimum, minimum + width)
        late_raw = (maximum - width, maximum)

        plot_times, mode = convert_time(
            [early_raw[0], early_raw[1], late_raw[0], late_raw[1]]
        )
        early_xlim = (plot_times[0], plot_times[1])
        late_xlim = (plot_times[2], plot_times[3])

        selected = states + flows
        selected = selected[:8] if selected else specs[:8]

        save_multi_panel(
            args.output_dir / f"paper_jm_early_window_{suffix}.png",
            selected,
            truth,
            assimilation,
            f"JM early window — first {width:g} days",
            early_xlim,
        )
        save_multi_panel(
            args.output_dir / f"paper_jm_late_window_{suffix}.png",
            selected,
            truth,
            assimilation,
            f"JM late window — last {width:g} days",
            late_xlim,
        )

        window_file = args.output_dir / "jm_plot_windows.json"
        window_file.write_text(
            json.dumps(
                {
                    "minimum_time": minimum,
                    "maximum_time": maximum,
                    "window_days": width,
                    "early": list(early_raw),
                    "late": list(late_raw),
                },
                indent=2,
            )
            + "\n",
            encoding="utf-8",
        )
        print(f"[ok] wrote {window_file}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
