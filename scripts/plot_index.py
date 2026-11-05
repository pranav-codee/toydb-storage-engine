#!/usr/bin/env python3
"""plot_index.py: figures and a summary table from the index benchmark.

usage: plot_index.py [csv] [outdir]

Reads the CSV written by bench/bench_index (make bench-index), averages
every (method, phase) over its repeats, and writes into outdir:

  index_build.pdf/.png      (a) index page accesses of each build: logical
                            reads, logical writes and physical reads +
                            writes; (b) build time: the index part
                            (idx_ms) and the whole build (ms)
  index_queries.pdf/.png    per query, for point, missing, range and the
                            full scan: (a) index logical reads (page
                            accesses), (b) index physical reads, (c) time
  index_summary.csv/.tex    per method: index pages, leaves, height, leaf
                            fill, build time and build index I/O, and
                            pages and time per query of each query type
                            (the .tex is a booktabs tabular only)

Bars are means over the repeats, error bars go from the min to the max.
The y axes are logarithmic, since the values span several orders of
magnitude (an M2 build against an M3 build, a full scan against a point
query). Only the index file's counters
are plotted; heap-file I/O is in the CSV and the summary table.

Only the standard library and matplotlib are used.
"""

import argparse
import csv
import os
import sys

PROG = "plot_index.py"

METHODS = ["M1", "M2", "M3"]
LABEL = {"M1": "M1 existing", "M2": "M2 incremental", "M3": "M3 bulk"}
QUERIES = ["point", "missing", "range", "fullscan"]
QLABEL = {"point": "point", "missing": "missing", "range": "range",
          "fullscan": "full scan"}

# The three methods look the same in every figure: Okabe-Ito blue,
# vermillion and bluish green (the first two as LRU and MRU in
# plot_buffer.py), told apart with any common colour vision deficiency;
# the hatch keeps them apart in greyscale too.
STYLE = {
    "M1": {"color": "#0072B2", "hatch": "", "marker": "o"},
    "M2": {"color": "#D55E00", "hatch": "//", "marker": "s"},
    "M3": {"color": "#009E73", "hatch": "..", "marker": "^"},
}

# CSV columns this script needs
NEEDED = ["method", "repeat", "phase", "ops", "ms", "ms_per_op", "idx_ms",
          "sort_ms", "idx_logical_reads", "idx_logical_writes",
          "idx_physical_reads", "idx_physical_writes", "heap_logical_reads",
          "heap_physical_reads", "heap_physical_writes", "heap_ms",
          "results", "index_pages", "leaves", "height", "leaf_fill"]
INTS = ["ops", "idx_logical_reads", "idx_logical_writes",
        "idx_physical_reads", "idx_physical_writes", "heap_logical_reads",
        "heap_physical_reads", "heap_physical_writes", "results",
        "index_pages", "leaves", "height"]
FLOATS = ["ms", "ms_per_op", "idx_ms", "sort_ms", "heap_ms", "leaf_fill"]


def die(msg):
    print("%s: %s" % (PROG, msg), file=sys.stderr)
    sys.exit(1)


# ---------------------------------------------------------------- data

def load(path):
    """Read the benchmark CSV. Return {(method, phase): [per-repeat row
    dict, ...]} with numbers converted and per-query values added."""
    try:
        f = open(path, newline="")
    except OSError as e:
        die("cannot open %s: %s" % (path, e.strerror))
    groups = {}
    with f:
        reader = csv.DictReader(f)
        missing = [c for c in NEEDED if c not in (reader.fieldnames or [])]
        if missing:
            die("%s has no column(s) %s" % (path, ", ".join(missing)))
        for n, row in enumerate(reader, start=2):
            try:
                run = {c: int(row[c]) for c in INTS}
                run.update({c: float(row[c]) for c in FLOATS})
            except (ValueError, TypeError):
                die("%s line %d: bad number" % (path, n))
            ops = run["ops"]
            run["idx_physical_io"] = (run["idx_physical_reads"] +
                                      run["idx_physical_writes"])
            run["heap_physical_io"] = (run["heap_physical_reads"] +
                                       run["heap_physical_writes"])
            # per-query values are computed per run, then averaged
            run["lr_per_op"] = run["idx_logical_reads"] / ops if ops else 0.0
            run["pr_per_op"] = run["idx_physical_reads"] / ops if ops else 0.0
            run["us_per_op"] = 1000.0 * run["ms_per_op"]
            run["heap_pr_per_op"] = (run["heap_physical_reads"] / ops
                                     if ops else 0.0)
            groups.setdefault((row["method"], row["phase"]), []).append(run)
    if not groups:
        die("%s has no data rows" % path)
    for m in METHODS:
        for ph in ["build"] + QUERIES:
            if (m, ph) not in groups:
                die("%s has no %s %s rows" % (path, m, ph))
    return groups


def stat(groups, key, metric):
    """(mean, min, max) of one metric over the repeats of (method, phase)."""
    vals = [r[metric] for r in groups[key]]
    return (sum(vals) / len(vals), min(vals), max(vals))


# ---------------------------------------------------------------- plots

def setup_matplotlib():
    try:
        import matplotlib
    except ImportError:
        die("matplotlib not found, install it with: "
            "sudo apt install python3-matplotlib  (or: pip install matplotlib)")
    matplotlib.use("Agg")		# no display needed
    import matplotlib.pyplot as plt
    plt.rcParams.update({
        "font.size": 9,
        "axes.labelsize": 9,
        "axes.titlesize": 9,
        "legend.fontsize": 9,
        "xtick.labelsize": 8,
        "ytick.labelsize": 8,
        "axes.grid": True,
        "grid.linewidth": 0.4,
        "grid.alpha": 0.5,
        "lines.linewidth": 1.2,
        "lines.markersize": 4,
        "errorbar.capsize": 2,
        "pdf.fonttype": 42,		# embed TrueType, text stays text
    })
    return plt


def save(fig, outdir, name):
    for ext in ("pdf", "png"):
        path = os.path.join(outdir, "%s.%s" % (name, ext))
        if ext == "pdf":
            # no creation date, so a rerun on the same data gives the
            # same file
            fig.savefig(path, metadata={"CreationDate": None})
        else:
            fig.savefig(path, dpi=150)
        print("%s: wrote %s" % (PROG, path), file=sys.stderr)


def panel_label(ax, text):
    ax.set_title(text, loc="left")


def grouped_bars(ax, groups, cats, labels):
    """One group of bars per category, one bar per method, with min/max
    error bars and the mean written above each bar. cats is a list of
    (phase, metric), one per group."""
    width = 0.26
    for i, m in enumerate(METHODS):
        xs, mean, lo, hi = [], [], [], []
        for j, (phase, met) in enumerate(cats):
            s = stat(groups, (m, phase), met)
            xs.append(j + (i - 1) * width)
            mean.append(s[0])
            # max(0, ...): when all repeats are equal the float mean can be
            # a rounding error past the min or max
            lo.append(max(0.0, s[0] - s[1]))
            hi.append(max(0.0, s[2] - s[0]))
        st = STYLE[m]
        # on a log axis a bar of 0 cannot be drawn; it is left out and its
        # value still printed at the bottom
        hts = [v if v > 0 else float("nan") for v in mean]
        ax.bar(xs, hts, width, color=st["color"], hatch=st["hatch"],
               edgecolor="white", linewidth=0.5, label=LABEL[m], zorder=2)
        ax.errorbar(xs, hts, yerr=[lo, hi], fmt="none", ecolor="black",
                    elinewidth=0.6, zorder=3)
        for x, v in zip(xs, mean):
            ax.annotate(fmt_num(v), (x, v if v > 0 else 1.0),
                        xytext=(0, 2), textcoords="offset points",
                        ha="center", va="bottom", fontsize=6, rotation=90)
    ax.set_xticks(range(len(cats)))
    ax.set_xticklabels(labels)
    ax.set_yscale("log")
    ax.grid(axis="x", visible=False)
    ax.margins(y=0.25)


def fmt_num(v):
    if v == 0:
        return "0"
    if float(v).is_integer():
        return "%d" % int(v)
    if v >= 100:
        return "%.0f" % v
    if v >= 10:
        return "%.1f" % v
    return "%.2f" % v


def figure_legend(fig, ax):
    handles, labels = ax.get_legend_handles_labels()
    fig.legend(handles, labels, loc="outside upper center",
               ncol=len(labels), frameon=False)


def fig_build(plt, groups, outdir):
    fig, axes = plt.subplots(1, 2, figsize=(7.0, 3.0), layout="constrained")
    ax = axes[0]
    cats = [("build", "idx_logical_reads"), ("build", "idx_logical_writes"),
            ("build", "idx_physical_io")]
    grouped_bars(ax, groups, cats,
                 ["logical reads", "logical writes", "physical\nreads + writes"])
    ax.set_ylabel("index pages per build")
    panel_label(ax, "(a) index page accesses")
    ax = axes[1]
    grouped_bars(ax, groups, [("build", "idx_ms"), ("build", "ms")],
                 ["index part", "whole build"])
    ax.set_ylabel("time per build (ms)")
    panel_label(ax, "(b) build time")
    figure_legend(fig, axes[0])
    save(fig, outdir, "index_build")
    plt.close(fig)


def fig_queries(plt, groups, outdir):
    fig, axes = plt.subplots(1, 3, figsize=(9.0, 3.0), layout="constrained")
    labels = [QLABEL[q] for q in QUERIES]
    panels = [("lr_per_op", "index logical reads per query",
               "(a) page accesses"),
              ("pr_per_op", "index physical reads per query",
               "(b) pages read from disk"),
              ("us_per_op", "time per query (\u00b5s)", "(c) time")]
    for ax, (metric, ylabel, title) in zip(axes, panels):
        grouped_bars(ax, groups, [(q, metric) for q in QUERIES], labels)
        ax.set_ylabel(ylabel)
        panel_label(ax, title)
    figure_legend(fig, axes[0])
    save(fig, outdir, "index_queries")
    plt.close(fig)


# ---------------------------------------------------------------- table

def summary_rows(groups):
    """(label, tex label, [M1, M2, M3] means, format, rule before) for the
    table."""
    def mean(phase, metric):
        return [stat(groups, (m, phase), metric)[0] for m in METHODS]

    rows = [
        ("index pages", "Index pages", mean("build", "index_pages"), "%.0f",
         False),
        ("leaves", "Leaves", mean("build", "leaves"), "%.0f", False),
        ("height", "Height", mean("build", "height"), "%.0f", False),
        ("leaf fill (%)", "Leaf fill (\\%)",
         [100.0 * v for v in mean("build", "leaf_fill")], "%.1f", False),
        ("build time (ms)", "Build time (ms)", mean("build", "ms"), "%.1f",
         True),
        ("index part (ms)", "\\quad index part (ms)",
         mean("build", "idx_ms"), "%.1f", False),
        ("heap scan + sort (ms)", "\\quad heap scan + sort (ms)",
         mean("build", "sort_ms"), "%.1f", False),
        ("build index logical reads", "Build index logical reads",
         mean("build", "idx_logical_reads"), "%.0f", True),
        ("build index logical writes", "Build index logical writes",
         mean("build", "idx_logical_writes"), "%.0f", False),
        ("build index physical reads", "Build index physical reads",
         mean("build", "idx_physical_reads"), "%.0f", False),
        ("build index physical writes", "Build index physical writes",
         mean("build", "idx_physical_writes"), "%.0f", False),
        ("build heap physical reads + writes",
         "Build heap physical reads + writes",
         mean("build", "heap_physical_io"), "%.0f", False),
    ]
    for q in QUERIES:
        name = QLABEL[q]
        rows.append(("%s: index logical reads / query" % name,
                     "%s: logical reads / query" % name.capitalize(),
                     mean(q, "lr_per_op"), "%.2f", True))
        rows.append(("%s: index physical reads / query" % name,
                     "\\quad physical reads / query",
                     mean(q, "pr_per_op"), "%.3f", False))
        rows.append(("%s: time / query (us)" % name,
                     "\\quad time / query ($\\mu$s)",
                     mean(q, "us_per_op"), "%.2f", False))
        if q == "point":
            rows.append(("point: heap physical reads / query (record fetch)",
                         "\\quad heap physical reads / query",
                         mean("point", "heap_pr_per_op"), "%.3f", False))
    return rows


def summary(groups, csv_path, outdir):
    rows = summary_rows(groups)
    nrep = len(groups[("M1", "build")])
    path = os.path.join(outdir, "index_summary.csv")
    with open(path, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["metric"] + METHODS)
        for label, _, vals, fmt, _ in rows:
            w.writerow([label] + [fmt % v for v in vals])
    print("%s: wrote %s" % (PROG, path), file=sys.stderr)

    path = os.path.join(outdir, "index_summary.tex")
    with open(path, "w") as f:
        f.write("%% from %s: means over %d repeats\n"
                % (os.path.basename(csv_path), nrep))
        f.write("\\begin{tabular}{lrrr}\n\\toprule\n")
        f.write(" & M1 existing & M2 incremental & M3 bulk \\\\\n")
        f.write("\\midrule\n")
        for _, tex, vals, fmt, rule in rows:
            if rule:
                f.write("\\midrule\n")
            f.write("%s & %s \\\\\n"
                    % (tex, " & ".join(fmt % v for v in vals)))
        f.write("\\bottomrule\n\\end{tabular}\n")
    print("%s: wrote %s" % (PROG, path), file=sys.stderr)


# ---------------------------------------------------------------- main

def main():
    ap = argparse.ArgumentParser(prog=PROG, description=__doc__.split("\n")[0])
    ap.add_argument("csv", nargs="?", default="results/bench_index.csv")
    ap.add_argument("outdir", nargs="?", default="results")
    args = ap.parse_args()

    plt = setup_matplotlib()
    groups = load(args.csv)
    os.makedirs(args.outdir, exist_ok=True)
    fig_build(plt, groups, args.outdir)
    fig_queries(plt, groups, args.outdir)
    summary(groups, args.csv, args.outdir)
    return 0


if __name__ == "__main__":
    sys.exit(main())
