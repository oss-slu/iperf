#!/usr/bin/env python3
"""
Read QUIC stress-test JSON files from stress_results/, write a CSV summary,
and save two PNG figures under stress_results/graphs/.
"""

import json
import os
import re
import csv
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

# Repo root is parent of stress/; works in Docker (/iperf) and locally.
_HERE = os.path.dirname(os.path.abspath(__file__))
_REPO = os.path.dirname(_HERE)
RESULTS = os.environ.get("STRESS_RESULTS_DIR",
                         os.path.join(_REPO, "stress_results"))
GRAPHS = os.path.join(RESULTS, "graphs")
os.makedirs(GRAPHS, exist_ok=True)

# --- helpers ---

def parse_json(path):
    with open(path) as f:
        return json.load(f)

def throughput_mbps(data):
    end = data.get("end", {})
    sent = end.get("sum_sent", {}).get("bits_per_second", 0)
    recv = end.get("sum_received", {}).get("bits_per_second", 0)
    return max(sent, recv) / 1e6

def parse_filename(fname):
    m = re.match(
        r"(par|stress)_quic_p(\d+)_bw(\d+)_d(\d+)_l(\d+)\.json", fname
    )
    if not m:
        return None
    return {
        "tag":   m.group(1),
        "par":   int(m.group(2)),
        "bw":    int(m.group(3)),
        "delay": int(m.group(4)),
        "loss":  int(m.group(5)),
    }

def load_all():
    rows = []
    for fname in sorted(os.listdir(RESULTS)):
        if not fname.endswith(".json"):
            continue
        info = parse_filename(fname)
        if not info:
            continue
        path = os.path.join(RESULTS, fname)
        try:
            data = parse_json(path)
        except (json.JSONDecodeError, OSError, KeyError):
            continue
        info["mbps"] = throughput_mbps(data)
        info["file"] = fname

        end = data.get("end", {})
        streams = end.get("streams", [])
        per_stream = []
        for s in streams:
            sender = s.get("sender", {})
            bps = sender.get("bits_per_second", 0)
            per_stream.append(bps / 1e6)
        info["per_stream"] = per_stream
        rows.append(info)
    return rows

# --- CSV summary ---

def write_csv(rows):
    path = os.path.join(RESULTS, "stress_summary.csv")
    with open(path, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["tag", "parallel", "bw_mbit", "delay_ms",
                     "loss_pct", "throughput_mbps"])
        for r in rows:
            w.writerow([r["tag"], r["par"], r["bw"],
                        r["delay"], r["loss"], f'{r["mbps"]:.1f}'])
    print(f"  wrote {path}")

# --- Graph 1: parallelism scaling ---

def plot_parallelism(rows):
    par_rows = sorted(
        [r for r in rows if r["tag"] == "par"],
        key=lambda r: r["par"]
    )
    if not par_rows:
        print("  skip parallelism graph (no data)")
        return

    pvals  = [r["par"]  for r in par_rows]
    totals = [r["mbps"] for r in par_rows]
    per_stream_avg = [
        r["mbps"] / r["par"] if r["par"] > 0 else 0 for r in par_rows
    ]

    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(14, 5.5))
    fig.patch.set_facecolor("white")

    # left: total throughput line
    ax1.plot(pvals, totals, "s-", color="#c0392b", linewidth=2,
             markersize=8, zorder=5)
    for x, y in zip(pvals, totals):
        ax1.annotate(f"{y:.0f}", (x, y), textcoords="offset points",
                     xytext=(0, 10), ha="center", fontsize=10,
                     fontweight="bold", color="#c0392b")

    peak_idx = int(np.argmax(totals))
    ax1.axhline(totals[peak_idx], color="#c0392b", alpha=0.2, linewidth=8)
    ax1.axvline(pvals[peak_idx], color="#c0392b", alpha=0.15,
                linewidth=6, linestyle=":")
    ax1.annotate(f"Peak at P={pvals[peak_idx]}\n{totals[peak_idx]:.0f} Mbit/s",
                 (pvals[peak_idx], totals[peak_idx]),
                 textcoords="offset points", xytext=(-60, -30),
                 fontsize=9, color="#c0392b",
                 bbox=dict(boxstyle="round,pad=0.3", fc="white",
                           ec="#c0392b", alpha=0.8))

    ax1.set_xlabel("Parallel Streams (-P)", fontsize=12)
    ax1.set_ylabel("Total Throughput (Mbit/s)", fontsize=12)
    ax1.set_title("QUIC: Scalability vs Parallel Streams",
                  fontsize=13, fontweight="bold")
    ax1.set_xticks(pvals)
    ax1.set_xticklabels([str(p) for p in pvals])
    ax1.set_ylim(0, max(totals) * 1.15)
    ax1.grid(axis="y", alpha=0.3)

    # right: per-stream bar chart
    bars = ax2.bar(range(len(pvals)), per_stream_avg, color="#e74c3c",
                   alpha=0.85, edgecolor="white", linewidth=0.5)
    for bar, val in zip(bars, per_stream_avg):
        ax2.text(bar.get_x() + bar.get_width() / 2, bar.get_height() + 1,
                 f"{val:.0f}", ha="center", fontsize=10,
                 fontweight="bold", color="#c0392b")

    ax2.set_xlabel("Parallel Streams (-P)", fontsize=12)
    ax2.set_ylabel("Avg Per-Stream Throughput (Mbit/s)", fontsize=12)
    ax2.set_title("QUIC: Per-Stream Share as P Increases",
                  fontsize=13, fontweight="bold")
    ax2.set_xticks(range(len(pvals)))
    ax2.set_xticklabels([str(p) for p in pvals])
    ax2.set_ylim(0, max(per_stream_avg) * 1.15)
    ax2.grid(axis="y", alpha=0.3)

    plt.tight_layout()
    out = os.path.join(GRAPHS, "1_quic_parallelism_scaling.png")
    fig.savefig(out, dpi=150, bbox_inches="tight")
    plt.close(fig)
    print(f"  wrote {out}")

# --- Graph 2: combined stress ---

def plot_combined(rows):
    stress = [r for r in rows if r["tag"] == "stress"]
    if not stress:
        print("  skip combined-stress graph (no data)")
        return

    delays = sorted(set(r["delay"] for r in stress))
    losses = sorted(set(r["loss"]  for r in stress))

    lookup = {}
    for r in stress:
        lookup[(r["delay"], r["loss"])] = r["mbps"]

    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(14, 5.5))
    fig.patch.set_facecolor("white")

    colors = {10: "#2980b9", 100: "#c0392b"}
    markers = {10: "o", 100: "s"}

    # left: line chart
    for d in delays:
        vals = [lookup.get((d, l), 0) for l in losses]
        ax1.plot(range(len(losses)), vals,
                 marker=markers.get(d, "o"), linewidth=2,
                 markersize=8, label=f"delay={d}ms",
                 color=colors.get(d, "#333"), zorder=5)
        for i, v in enumerate(vals):
            ax1.annotate(f"{v:.0f}", (i, v), textcoords="offset points",
                         xytext=(0, 10), ha="center", fontsize=9,
                         fontweight="bold", color=colors.get(d, "#333"))

    ax1.axvline(1, color="red", alpha=0.3, linewidth=6, linestyle=":")
    for d in delays:
        v0 = lookup.get((d, 0), 0)
        v1 = lookup.get((d, 1), 0)
        if v0 > 0 and v1 > 0:
            drop = (1 - v1 / v0) * 100
            ax1.annotate(f"{drop:.0f}% drop\nat 1% loss",
                         (1, v1), textcoords="offset points",
                         xytext=(-50, -15), fontsize=8,
                         color=colors.get(d, "#333"), alpha=0.8)

    ax1.set_xlabel("Packet Loss (%)", fontsize=12)
    ax1.set_ylabel("Throughput (Mbit/s)", fontsize=12)
    ax1.set_title("QUIC Under Combined Stress\n(delay x loss, P=4)",
                  fontsize=13, fontweight="bold")
    ax1.set_xticks(range(len(losses)))
    ax1.set_xticklabels([f"{l}%" for l in losses])
    ax1.legend(fontsize=10)
    ax1.grid(axis="y", alpha=0.3)

    # right: heatmap
    matrix = np.zeros((len(delays), len(losses)))
    for i, d in enumerate(delays):
        for j, l in enumerate(losses):
            matrix[i, j] = lookup.get((d, l), 0)

    im = ax2.imshow(matrix, cmap="RdYlGn", aspect="auto",
                    vmin=0, vmax=max(r["mbps"] for r in stress))
    ax2.set_xticks(range(len(losses)))
    ax2.set_xticklabels([f"{l}%" for l in losses])
    ax2.set_yticks(range(len(delays)))
    ax2.set_yticklabels([f"{d}ms" for d in delays])
    ax2.set_xlabel("Packet Loss (%)", fontsize=12)
    ax2.set_ylabel("Link Delay (ms)", fontsize=12)
    ax2.set_title("QUIC Throughput Heatmap\n(Mbit/s, P=4)",
                  fontsize=13, fontweight="bold")

    for i in range(len(delays)):
        for j in range(len(losses)):
            val = matrix[i, j]
            color = "white" if val < (matrix.max() / 2) else "black"
            ax2.text(j, i, f"{val:.0f}", ha="center", va="center",
                     fontsize=12, fontweight="bold", color=color)

    cbar = fig.colorbar(im, ax=ax2, shrink=0.8)
    cbar.set_label("Throughput (Mbit/s)", fontsize=10)

    plt.tight_layout()
    out = os.path.join(GRAPHS, "2_quic_combined_stress.png")
    fig.savefig(out, dpi=150, bbox_inches="tight")
    plt.close(fig)
    print(f"  wrote {out}")

# --- main ---

if __name__ == "__main__":
    print("Loading results ...")
    rows = load_all()
    print(f"  found {len(rows)} result files")

    write_csv(rows)
    plot_parallelism(rows)
    plot_combined(rows)
    print("Done.")
