"""Plotting helpers for indoor-positioning-engine outputs.

Reads the venue JSON, the estimates CSV and the particle snapshot CSV that
`ipe-replay` writes, and renders maps, error curves and animations.
"""

from __future__ import annotations

import csv
import json
from dataclasses import dataclass
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
import numpy as np  # noqa: E402
from matplotlib.animation import FuncAnimation, PillowWriter  # noqa: E402

COLORS = {
    "truth": "#222222",
    "pf": "#d62728",
    "pf-nomap": "#ff7f0e",
    "ekf": "#1f77b4",
    "wcl": "#2ca02c",
}


@dataclass
class Venue:
    name: str
    bounds: tuple[float, float, float, float]
    walls: list[tuple[float, float, float, float]]
    beacons: list[dict]

    @staticmethod
    def load(path: str | Path, ordinal: int | None = None) -> "Venue":
        """Loads one floor. Multi-level venues default to their lowest level."""
        j = json.loads(Path(path).read_text())
        walls, beacons = j.get("walls", []), j["beacons"]
        if "levels" in j:
            levels = sorted(j["levels"], key=lambda lv: lv["ordinal"])
            level = next((lv for lv in levels if lv["ordinal"] == ordinal), levels[0])
            walls = level.get("walls", [])
            beacons = [b for b in beacons if b.get("level", levels[0]["ordinal"]) == level["ordinal"]]
        return Venue(j.get("name", "venue"), tuple(j["bounds"]), [tuple(w) for w in walls], beacons)


@dataclass
class Estimates:
    t: np.ndarray
    truth: np.ndarray  # (N, 2)
    est: np.ndarray  # (N, 2)
    sigma: np.ndarray
    error: np.ndarray
    has_fix: np.ndarray

    @staticmethod
    def load(path: str | Path) -> "Estimates":
        data = np.genfromtxt(path, delimiter=",", names=True)
        return Estimates(
            t=data["t"],
            truth=np.column_stack([data["truth_x"], data["truth_y"]]),
            est=np.column_stack([data["est_x"], data["est_y"]]),
            sigma=data["sigma_m"],
            error=data["error_m"],
            has_fix=data["has_fix"].astype(bool),
        )


def load_particles(path: str | Path) -> dict[float, np.ndarray]:
    """Returns {t: array of (x, y, w)} from a particle snapshot CSV."""
    snaps: dict[float, list[tuple[float, float, float]]] = {}
    with open(path, newline="") as f:
        for row in csv.DictReader(f):
            snaps.setdefault(float(row["t"]), []).append((float(row["x"]), float(row["y"]), float(row["w"])))
    return {t: np.array(v) for t, v in snaps.items()}


def draw_venue(ax, venue: Venue, beacon_labels: bool = True, highlight: dict[str, str] | None = None,
               background: str | Path | None = None, beacon_size: float = 55) -> None:
    """`background` is an image stretched over the venue bounds (e.g. a floor plan raster)."""
    x0, y0, x1, y1 = venue.bounds
    if background is not None:
        ax.imshow(plt.imread(background), extent=(x0, x1, y0, y1), origin="upper", alpha=0.55, zorder=0)
    for w in venue.walls:
        ax.plot([w[0], w[2]], [w[1], w[3]], color="#555555", lw=2, solid_capstyle="round", zorder=1)
    for b in venue.beacons:
        color = (highlight or {}).get(b["id"], "#7f3fbf")
        ax.scatter(b["x"], b["y"], marker="^", s=beacon_size, color=color, edgecolor="white", linewidth=0.8, zorder=4)
        if beacon_labels:
            ax.annotate(b["id"], (b["x"], b["y"]), textcoords="offset points", xytext=(4, 4), fontsize=7, color=color)
    pad = 1.0
    ax.set_xlim(x0 - pad, x1 + pad)
    ax.set_ylim(y0 - pad, y1 + pad)
    ax.set_aspect("equal")
    ax.set_xlabel("x [m]")
    ax.set_ylabel("y [m]")


def plot_trajectories(venue: Venue, runs: dict[str, Estimates], out: str | Path, title: str = "",
                      highlight: dict[str, str] | None = None, warmup_s: float = 5.0) -> None:
    """Estimated tracks after the warm-up period (same window as the metrics)."""
    fig, ax = plt.subplots(figsize=(10, 6.5))
    draw_venue(ax, venue, highlight=highlight)
    first = next(iter(runs.values()))
    ax.plot(first.truth[:, 0], first.truth[:, 1], color=COLORS["truth"], lw=2.2, label="ground truth", zorder=2)
    for name, r in runs.items():
        m = r.has_fix & (r.t - r.t[0] >= warmup_s)
        ax.plot(r.est[m, 0], r.est[m, 1], color=COLORS.get(name, None), lw=1.2, alpha=0.9,
                label=f"{name} (CEP50 {np.median(r.error[m]):.2f} m)", zorder=3)
    ax.legend(loc="upper right", fontsize=8, framealpha=0.9)
    ax.set_title(title or venue.name)
    fig.tight_layout()
    fig.savefig(out, dpi=130)
    plt.close(fig)


def plot_error_over_time(runs: dict[str, Estimates], out: str | Path, title: str = "",
                         colors: dict[str, str] | None = None) -> None:
    fig, ax = plt.subplots(figsize=(10, 3.4))
    palette = {**COLORS, **(colors or {})}
    for name, r in runs.items():
        ax.plot(r.t, np.where(r.has_fix, r.error, np.nan), color=palette.get(name), lw=1.1, label=name)
    ax.set_xlabel("time [s]")
    ax.set_ylabel("horizontal error [m]")
    ax.set_ylim(bottom=0)
    ax.grid(alpha=0.3)
    ax.legend(fontsize=8, ncol=len(runs))
    ax.set_title(title)
    fig.tight_layout()
    fig.savefig(out, dpi=130)
    plt.close(fig)


def plot_error_cdf(runs: dict[str, Estimates], out: str | Path, title: str = "", warmup_s: float = 5.0) -> None:
    fig, ax = plt.subplots(figsize=(6, 4))
    for name, r in runs.items():
        m = r.has_fix & (r.t - r.t[0] >= warmup_s)
        e = np.sort(r.error[m])
        if e.size == 0:
            continue
        ax.plot(e, np.arange(1, e.size + 1) / e.size, color=COLORS.get(name), lw=1.6, label=name)
    ax.axhline(0.5, color="#999999", lw=0.8, ls="--")
    ax.axhline(0.95, color="#999999", lw=0.8, ls="--")
    ax.set_xlabel("horizontal error [m]")
    ax.set_ylabel("fraction of samples")
    ax.set_xlim(left=0)
    ax.set_ylim(0, 1.01)
    ax.grid(alpha=0.3)
    ax.legend(fontsize=8)
    ax.set_title(title or "Error CDF")
    fig.tight_layout()
    fig.savefig(out, dpi=130)
    plt.close(fig)


def plot_benchmark(results: dict, out: str | Path, metric: str = "cep95_m") -> None:
    """Grouped bar chart from `ipe-regress --json` output."""
    scenarios = sorted(results["results"].keys())
    presets = [p for p in ["pf", "pf-nomap", "ekf", "wcl"] if any(p in results["results"][s] for s in scenarios)]
    x = np.arange(len(scenarios))
    width = 0.8 / len(presets)
    fig, ax = plt.subplots(figsize=(11, 4.2))
    for i, p in enumerate(presets):
        vals = [results["results"][s].get(p, {}).get(metric, np.nan) for s in scenarios]
        ax.bar(x + (i - (len(presets) - 1) / 2) * width, vals, width, label=p, color=COLORS.get(p))
    ax.set_xticks(x)
    ax.set_xticklabels([s.replace("_", "\n", 1) for s in scenarios], fontsize=8)
    ax.set_ylabel(metric.removesuffix("_m").upper() + " [m]")
    ax.grid(axis="y", alpha=0.3)
    ax.legend(fontsize=8, ncol=len(presets))
    ax.set_title(f"{metric.replace('_m', '').upper()} per scenario (mean of {results.get('seeds', '?')} seeds)")
    fig.tight_layout()
    fig.savefig(out, dpi=130)
    plt.close(fig)


def animate_particles(venue: Venue, est: Estimates, particles: dict[float, np.ndarray], out: str | Path,
                      t_max: float | None = None, fps: int = 10, title: str = "") -> None:
    """GIF of the particle cloud, estimate and ground truth over time."""
    times = sorted(t for t in particles if t_max is None or t <= t_max)
    fig, ax = plt.subplots(figsize=(8, 5.2))
    draw_venue(ax, venue, beacon_labels=False)
    cloud = ax.scatter([], [], s=3, color=COLORS["pf"], alpha=0.35, zorder=2)
    (trail,) = ax.plot([], [], color=COLORS["truth"], lw=1.5, zorder=3)
    (est_trail,) = ax.plot([], [], color=COLORS["pf"], lw=1.2, alpha=0.8, zorder=3)
    truth_dot = ax.scatter([], [], s=50, color=COLORS["truth"], zorder=5, label="truth")
    est_dot = ax.scatter([], [], s=50, color=COLORS["pf"], edgecolor="white", zorder=6, label="PF estimate")
    label = ax.text(0.01, 0.99, "", transform=ax.transAxes, va="top", fontsize=9,
                    bbox=dict(facecolor="white", alpha=0.8, edgecolor="none"))
    ax.legend(loc="upper right", fontsize=8)
    ax.set_title(title)
    fig.tight_layout()

    def frame(k: int):
        t = times[k]
        p = particles[t]
        cloud.set_offsets(p[:, :2])
        idx = int(np.searchsorted(est.t, t))
        idx = min(idx, len(est.t) - 1)
        trail.set_data(est.truth[: idx + 1, 0], est.truth[: idx + 1, 1])
        fix = est.has_fix[: idx + 1]
        est_trail.set_data(est.est[: idx + 1][fix, 0], est.est[: idx + 1][fix, 1])
        truth_dot.set_offsets([est.truth[idx]])
        est_dot.set_offsets([est.est[idx]])
        label.set_text(f"t = {t:5.1f} s   error = {est.error[idx]:4.2f} m")
        return cloud, trail, est_trail, truth_dot, est_dot, label

    anim = FuncAnimation(fig, frame, frames=len(times), blit=True)
    anim.save(out, writer=PillowWriter(fps=fps), dpi=80)
    plt.close(fig)
