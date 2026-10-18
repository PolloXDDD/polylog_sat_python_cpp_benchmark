#!/usr/bin/env python3
"""Optional export of benchmark plots; requires matplotlib."""
import json
from pathlib import Path
import statistics
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.ticker import FuncFormatter

ROOT = Path(__file__).resolve().parent
rows = [json.loads(line) for line in (ROOT/"results/raw.jsonl").read_text().splitlines()]
summary = json.loads((ROOT/"results/summary.json").read_text())
plt.rcParams.update({"font.family": "DejaVu Sans", "font.size": 11,
                     "axes.spines.top": False, "axes.spines.right": False,
                     "axes.labelcolor": "#263444", "text.color": "#17283B",
                     "axes.edgecolor": "#BAC4CE", "xtick.color": "#43556B",
                     "ytick.color": "#43556B", "svg.fonttype": "none"})
fig, axes = plt.subplots(1, 2, figsize=(13.8, 5.5))
colors = {"python": "#D66B26", "cpp": "#077E86"}
labels = {"python": "Python", "cpp": "C++"}
for engine in ("python", "cpp"):
    selected = sorted((r for r in summary if r["family"] == "random3sat" and r["engine"] == engine), key=lambda r: r["n"])
    xs = [r["n"] for r in selected]
    ys = [r["graph_seconds_median"]*1000 for r in selected]
    axes[0].plot(xs, ys, "o-", color=colors[engine], lw=2.2, label=labels[engine], zorder=3)
    for n in xs:
        by_seed = {}
        for r in rows:
            if r["family"] == "random3sat" and r["engine"] == engine and r["n"] == n:
                by_seed.setdefault(r["seed"], []).append(r["graph_seconds"]*1000)
        medians = [statistics.median(v) for v in by_seed.values()]
        axes[0].scatter([n]*len(medians), medians, s=12, color=colors[engine], alpha=.27, zorder=2)
axes[0].set(title="Tiempo en 3-SAT aleatorio", xlabel="Variables", ylabel="Búsqueda del grafo (ms, escala log)", yscale="log")
axes[0].set_xticks(xs)
axes[0].legend(frameon=False, loc="upper left")
axes[0].text(0, -0.23, "8 semillas por tamaño · 3 repeticiones · m ≈ 4.26n\nLíneas: medianas agrupadas. Puntos: medianas por semilla.",
             transform=axes[0].transAxes, fontsize=9, color="#5B6A7A")

for family, label, color in [("separated_equalities", "x₁…xₕ, y₁…yₕ", "#7B4DA1"),
                              ("interleaved_equalities", "x₁,y₁, x₂,y₂…", "#077E86")]:
    selected = sorted((r for r in summary if r["family"] == family and r["engine"] == "python"), key=lambda r: r["n"])
    xs = [r["n"] for r in selected]
    ys = [r["peak_frontier_median"] for r in selected]
    axes[1].plot(xs, ys, "o-", color=color, lw=2.2, label=label)
    axes[1].annotate(f"{int(ys[-1]):,}", (xs[-1], ys[-1]), textcoords="offset points", xytext=(-4, 12), ha="right", color=color, weight="bold")
axes[1].set(title="La misma relación, distinto orden", xlabel="Variables (n = 2h)",
            ylabel="Máximo de estados por capa (escala log)", yscale="log")
axes[1].set_xticks(xs)
axes[1].set_ylim(1, 1_500_000)
axes[1].legend(frameon=False, loc="upper left")
axes[1].text(0, -0.23, "h igualdades xᵢ = yᵢ. Python y C++ coinciden.\nOrden separado: 2ʰ estados. Orden intercalado: 2.",
             transform=axes[1].transAxes, fontsize=9, color="#5B6A7A")
for ax in axes:
    ax.grid(axis="y", alpha=.15)
    ax.set_axisbelow(True)
    ax.yaxis.set_major_formatter(FuncFormatter(lambda y, _: f"{y:g}"))
fig.suptitle("Búsqueda por configuraciones · benchmark reproducible", fontsize=18, weight="bold", x=.065, ha="left", y=.98)
fig.subplots_adjust(left=.065, right=.98, top=.83, bottom=.25, wspace=.28)
fig.savefig(ROOT/"benchmark.png", dpi=180, facecolor="white")
fig.savefig(ROOT/"benchmark.svg", facecolor="white")
print("Wrote benchmark.png and benchmark.svg")
