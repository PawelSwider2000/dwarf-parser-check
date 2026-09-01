#!/usr/bin/env python3
"""Generate experiment charts from an experiment_summary.json file.

Example:
  python3 data_generation/plot_experiment_summary.py \
      artifacts/experiment_summary.json --adapter rust-gimli
"""

import argparse
import json
import re
import sys
from dataclasses import dataclass
from datetime import UTC, datetime
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as pyplot
from matplotlib.colors import LinearSegmentedColormap
from matplotlib.patches import Patch


EXPERIMENT_NAME = re.compile(
    r"^(?P<workload>[^/]+)/debug=(?P<debug>[^/]+)/opt=(?P<optimization>O[0-2])/"
    r"device-code=(?P<device_code>jit|aot)(?:/device-target=(?P<target>[^/]+))?$"
)
WORKLOAD_ORDER = ["gemm", "control_flow", "multifile", "inline_chain", "multifile_inline_chain"]
CONFIGURATION_ORDER = [("jit", "O0"), ("aot", "O0"), ("jit", "O1"), ("aot", "O1"), ("jit", "O2"), ("aot", "O2")]
MISMATCH_COMPONENTS = (
    ("file_mismatches", "File mismatch", "#d55e00"),
    ("line_mismatches", "Line mismatch", "#cc79a7"),
    ("missing_in_reference", "Missing in reference", "#0072b2"),
    ("missing_in_backend", "Missing in backend", "#009e73"),
    ("column_mismatches", "Column mismatch", "#e69f00"),
)
VTUNE_COLOR = "#0072b2"
UNITRACE_COLOR = "#d55e00"
STATUS_COLORS = {"PASS": "#009e73", "SKIP": "#e69f00", "FAIL": "#d55e00"}


@dataclass(frozen=True)
class Experiment:
    workload: str
    optimization: str
    device_code: str
    status: str
    comparison: dict
    unitrace_comparison: dict

    @property
    def configuration_label(self) -> str:
        return f"{self.device_code.upper()} {self.optimization}"

    @property
    def chart_label(self) -> str:
        return f"{self.workload.replace('_', ' ')} | {self.configuration_label}"


@dataclass(frozen=True)
class ExperimentGrid:
    workloads: list[str]
    configurations: list[tuple[str, str]]
    rows: list[list[Experiment]]


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("summary", type=Path, help="experiment_summary.json input file")
    parser.add_argument(
        "--output-dir",
        type=Path,
        help="directory for generated charts (default: SUMMARY parent/charts)",
    )
    parser.add_argument(
        "--adapter",
        help="adapter used for the summary; overrides the summary's adapters field",
    )
    return parser.parse_args()


def read_experiments(summary_path: Path, adapter: str) -> tuple[dict, list[Experiment]]:
    document = json.loads(summary_path.read_text(encoding="utf-8"))
    if not isinstance(document.get("summary"), dict) or not isinstance(document.get("experiments"), list):
        raise ValueError("summary must contain object fields 'summary' and 'experiments'")

    experiments = []
    for record in document["experiments"]:
        name = record.get("name", "")
        match = EXPERIMENT_NAME.fullmatch(name)
        if match is None:
            raise ValueError(f"unrecognized experiment name: {name}")
        adapter_results = record.get("adapter_results")
        if isinstance(adapter_results, dict):
            adapter_result = adapter_results.get(adapter)
            if not isinstance(adapter_result, dict):
                raise ValueError(f"{name} has no results for adapter '{adapter}'")
            comparison = adapter_result.get("comparison")
            unitrace_comparison = adapter_result.get("unitrace_comparison")
            status = adapter_result.get("status", record.get("status", "UNKNOWN"))
        else:
            comparison = record.get("comparison")
            unitrace_comparison = record.get("unitrace_comparison")
            status = record.get("status", "UNKNOWN")
        if not isinstance(comparison, dict) or not isinstance(unitrace_comparison, dict):
            raise ValueError(f"{name} has no complete VTune and Unitace comparisons")
        experiments.append(
            Experiment(
                workload=match["workload"],
                optimization=match["optimization"],
                device_code=match["device_code"],
                status=status,
                comparison=comparison,
                unitrace_comparison=unitrace_comparison,
            )
        )
    return document, experiments


def setup_figure(title: str, adapter: str, width: float = 13, height: float = 7):
    figure, axes = pyplot.subplots(figsize=(width, height), layout="constrained")
    figure.suptitle(title, fontsize=16, fontweight="bold", x=0.01, ha="left")
    axes.set_title(f"Adapter: {adapter}", fontsize=10, color="#555555", loc="left", pad=12)
    return figure, axes


def write_figure(figure, output_dir: Path, filename: str) -> None:
    figure.savefig(output_dir / filename, dpi=180, facecolor="white")
    pyplot.close(figure)


def comparison_value(comparison: dict, field: str) -> int:
    value = comparison.get(field, 0)
    if not isinstance(value, int):
        raise ValueError(f"comparison field '{field}' must be an integer")
    return value


def coverage_percent(comparison: dict) -> float:
    coverage = comparison.get("coverage")
    if not isinstance(coverage, dict):
        raise ValueError("comparison has no coverage object")
    sampled = coverage.get("sampled_ips", 0)
    mapped = coverage.get("mapped_ips", 0)
    if not isinstance(sampled, int) or not isinstance(mapped, int) or sampled < 0 or mapped < 0:
        raise ValueError("coverage values must be non-negative integers")
    return 100 * mapped / sampled if sampled else 0


def coverage_label(comparison: dict, percent: float) -> str:
    coverage = comparison.get("coverage")
    if not isinstance(coverage, dict):
        raise ValueError("comparison has no coverage object")
    mapped = coverage.get("mapped_ips", 0)
    sampled = coverage.get("sampled_ips", 0)
    if not isinstance(mapped, int) or not isinstance(sampled, int):
        raise ValueError("coverage values must be integers")
    return f"{percent:.1f}%\n{mapped}/{sampled}"


def mismatch_percent(comparison: dict) -> float:
    summary = comparison.get("summary")
    if not isinstance(summary, dict):
        raise ValueError("comparison has no summary object")
    compared = comparison_value(summary, "compared_offsets")
    mismatches = comparison_value(comparison, "mismatch_count")
    return 100 * mismatches / compared if compared else 0


def mismatch_label(comparison: dict) -> str:
    summary = comparison.get("summary")
    if not isinstance(summary, dict):
        raise ValueError("comparison has no summary object")
    categories = (
        ("file_mismatches", "F"),
        ("line_mismatches", "L"),
        ("column_mismatches", "C"),
        ("missing_in_reference", "R"),
        ("missing_in_backend", "B"),
    )
    labels = [f"{short_name}:{comparison_value(summary, field)}" for field, short_name in categories if comparison_value(summary, field)]
    return " ".join(labels) if labels else "0"


def experiment_grid(experiments: list[Experiment]) -> ExperimentGrid:
    by_configuration = {(entry.workload, entry.device_code, entry.optimization): entry for entry in experiments}
    workloads = [workload for workload in WORKLOAD_ORDER if any(entry.workload == workload for entry in experiments)]
    configurations = [
        configuration
        for configuration in CONFIGURATION_ORDER
        if any((entry.device_code, entry.optimization) == configuration for entry in experiments)
    ]
    if not workloads or not configurations:
        raise ValueError("summary contains no experiment records")
    missing = [
        f"{workload}/{device_code}/{optimization}"
        for workload in workloads
        for device_code, optimization in configurations
        if (workload, device_code, optimization) not in by_configuration
    ]
    if missing:
        raise ValueError("experiment matrix is incomplete: " + ", ".join(missing))
    return ExperimentGrid(
        workloads=workloads,
        configurations=configurations,
        rows=[
            [by_configuration[(workload, device_code, optimization)] for device_code, optimization in configurations]
            for workload in workloads
        ],
    )


def plot_status(document: dict, adapter: str, output_dir: Path) -> None:
    summary = document["summary"]
    labels = ["PASS", "SKIP", "FAIL"]
    values = [summary.get(label.lower() + "ed", 0) if label != "PASS" else summary.get("passed", 0) for label in labels]
    values[1] = summary.get("skipped", 0)
    values[2] = summary.get("failed", 0)
    total = summary.get("total", sum(values))

    figure, axes = setup_figure("Experiment Status", adapter, width=8, height=5)
    bars = axes.bar(labels, values, color=[STATUS_COLORS[label] for label in labels], width=0.58)
    axes.set_ylabel("Experiments")
    axes.set_ylim(0, max(1, max(values) * 1.18))
    axes.grid(axis="y", color="#dddddd", linewidth=0.8)
    axes.set_axisbelow(True)
    for bar, value in zip(bars, values):
        axes.text(bar.get_x() + bar.get_width() / 2, value + max(0.2, total * 0.01), str(value), ha="center", va="bottom", fontweight="bold")
    axes.text(0.98, 0.93, f"Total: {total}", transform=axes.transAxes, ha="right", va="top", fontsize=12, fontweight="bold")
    write_figure(figure, output_dir, "01_experiment_status.png")


def plot_heatmap(
    grid: ExperimentGrid,
    adapter: str,
    output_dir: Path,
    filename: str,
    title: str,
    extractor,
    scale_label: str,
    labeler=None,
    caption: str | None = None,
) -> None:
    values = [[extractor(entry) for entry in row] for row in grid.rows]
    figure, axes = setup_figure(
        title,
        adapter,
        width=max(8, len(grid.configurations) * 2),
        height=max(4, len(grid.workloads) * 1.2 + 2),
    )
    image = axes.imshow(values, aspect="auto", cmap=LinearSegmentedColormap.from_list("mapping", ["#f7fbff", "#6baed6", "#08519c"]))
    axes.set_xticks(range(len(grid.configurations)), [f"{mode.upper()}\n{optimization}" for mode, optimization in grid.configurations])
    axes.set_yticks(range(len(grid.workloads)), [workload.replace("_", " ") for workload in grid.workloads])
    for row_index, row in enumerate(values):
        for column_index, value in enumerate(row):
            entry = grid.rows[row_index][column_index]
            label = labeler(entry, value) if labeler else f"{value:.1f}%" if scale_label.endswith("(%)") else str(value)
            color = "white" if image.norm(value) > 0.58 else "#202020"
            axes.text(column_index, row_index, label, ha="center", va="center", color=color, fontsize=9)
    if caption:
        axes.set_xlabel(caption, labelpad=12)
    colorbar = figure.colorbar(image, ax=axes, shrink=0.9)
    colorbar.set_label(scale_label)
    write_figure(figure, output_dir, filename)


def plot_mismatch_comparison(grid: ExperimentGrid, adapter: str, output_dir: Path) -> None:
    figure, axes = setup_figure("VTune and Unitace Mismatch Counts", adapter, width=15, height=14)
    labels = []
    vtune_values = []
    unitrace_values = []
    for row in grid.rows:
        for entry in row:
            labels.append(entry.chart_label)
            vtune_values.append(comparison_value(entry.comparison, "mismatch_count"))
            unitrace_values.append(comparison_value(entry.unitrace_comparison, "mismatch_count"))
    positions = list(range(len(labels)))
    axes.barh([position - 0.2 for position in positions], vtune_values, height=0.38, color=VTUNE_COLOR, label="VTune")
    axes.barh([position + 0.2 for position in positions], unitrace_values, height=0.38, color=UNITRACE_COLOR, label="Unitace")
    axes.set_yticks(positions, labels, fontsize=9)
    axes.invert_yaxis()
    axes.set_xscale("symlog", linthresh=1)
    axes.set_xlabel("Mismatch count (symmetric log scale)")
    axes.grid(axis="x", color="#dddddd", linewidth=0.8)
    axes.set_axisbelow(True)
    axes.legend(loc="lower right")
    write_figure(figure, output_dir, "05_vtune_unitrace_mismatches.png")


def plot_mismatch_composition(
    grid: ExperimentGrid,
    adapter: str,
    output_dir: Path,
    comparison_field: str,
    tool_name: str,
    filename: str,
) -> None:
    figure, axes = setup_figure(f"{tool_name} Mismatch Composition", adapter, width=15, height=14)
    labels = []
    entries = [entry for row in grid.rows for entry in row]
    cumulative = [0] * len(entries)
    for row in grid.rows:
        labels.extend(entry.chart_label for entry in row)
    positions = list(range(len(labels)))
    for field, label, color in MISMATCH_COMPONENTS:
        values = [comparison_value(getattr(entry, comparison_field).get("summary", {}), field) for entry in entries]
        axes.barh(positions, values, left=cumulative, height=0.72, label=label, color=color)
        cumulative = [left + value for left, value in zip(cumulative, values)]
    axes.set_yticks(positions, labels, fontsize=9)
    axes.invert_yaxis()
    axes.set_xlabel("Mismatch count")
    axes.grid(axis="x", color="#dddddd", linewidth=0.8)
    axes.set_axisbelow(True)
    axes.legend(handles=[Patch(color=color, label=label) for _, label, color in MISMATCH_COMPONENTS], ncols=3, loc="lower right", fontsize=8)
    write_figure(figure, output_dir, filename)


def write_metadata(output_dir: Path, summary_path: Path, adapter: str, document: dict) -> None:
    metadata = {
        "source_summary": str(summary_path.resolve()),
        "adapter": adapter,
        "generated_at_utc": datetime.now(UTC).isoformat(),
        "experiment_status": document["summary"],
        "charts": [
            "01_experiment_status.png",
            "02_vtune_mismatch_heatmap.png",
            "03_vtune_coverage_heatmap.png",
            "04_unitrace_coverage_heatmap.png",
            "05_vtune_unitrace_mismatches.png",
            "06_vtune_mismatch_composition.png",
            "07_unitrace_mismatch_composition.png",
        ],
    }
    (output_dir / "chart_metadata.json").write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")
    (output_dir / "README.md").write_text(
        "# Experiment Charts\n\n"
        f"Adapter: `{adapter}`\n\n"
        "The VTune comparison is the experiment pass/fail basis. Unitace plots are "
        "diagnostic and include source-marker/reference disagreement.\n",
        encoding="utf-8",
    )


def main() -> int:
    arguments = parse_arguments()
    try:
        summary_path = arguments.summary.resolve()
        if not summary_path.is_file():
            raise ValueError(f"summary file not found: {summary_path}")
        document = json.loads(summary_path.read_text(encoding="utf-8"))
        if not isinstance(document.get("summary"), dict) or not isinstance(document.get("experiments"), list):
            raise ValueError("summary must contain object fields 'summary' and 'experiments'")
        adapters = document.get("adapters")
        if arguments.adapter:
            adapter = arguments.adapter
        elif isinstance(adapters, list) and len(adapters) == 1 and isinstance(adapters[0], str):
            adapter = adapters[0]
        elif isinstance(adapters, list) and len(adapters) > 1:
            raise ValueError("summary contains multiple adapters; pass --adapter NAME")
        else:
            adapter = "rust-gimli"
        document, experiments = read_experiments(summary_path, adapter)
        output_dir = (arguments.output_dir or summary_path.parent / "charts").resolve()
        output_dir.mkdir(parents=True, exist_ok=True)
        grid = experiment_grid(experiments)

        plot_status(document, adapter, output_dir)
        plot_heatmap(
            grid,
            adapter,
            output_dir,
            "02_vtune_mismatch_heatmap.png",
            "VTune Resolver Mismatch Rate",
            lambda entry: mismatch_percent(entry.comparison),
            "Mismatched compared IPs (%)",
            lambda entry, value: f"{value:.1f}%\n{mismatch_label(entry.comparison)}" if value else "0%",
            "Cell codes: F file, L line, C column, R missing reference, B missing backend",
        )
        plot_heatmap(
            grid,
            adapter,
            output_dir,
            "03_vtune_coverage_heatmap.png",
            "VTune DWARF Coverage",
            lambda entry: coverage_percent(entry.comparison),
            "Mapped IPs (%)",
            lambda entry, value: coverage_label(entry.comparison, value),
            "Each cell: mapped IPs / sampled IPs",
        )
        plot_heatmap(
            grid,
            adapter,
            output_dir,
            "04_unitrace_coverage_heatmap.png",
            "Unitace DWARF Coverage",
            lambda entry: coverage_percent(entry.unitrace_comparison),
            "Mapped IPs (%)",
            lambda entry, value: coverage_label(entry.unitrace_comparison, value),
            "Each cell: mapped IPs / sampled IPs",
        )
        plot_mismatch_comparison(grid, adapter, output_dir)
        plot_mismatch_composition(grid, adapter, output_dir, "comparison", "VTune", "06_vtune_mismatch_composition.png")
        plot_mismatch_composition(grid, adapter, output_dir, "unitrace_comparison", "Unitace", "07_unitrace_mismatch_composition.png")
        write_metadata(output_dir, summary_path, adapter, document)
        print(f"generated 7 charts in {output_dir}")
        return 0
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print(f"plot_experiment_summary: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())