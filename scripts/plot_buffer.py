#!/usr/bin/env python3
"""plot_buffer.py: figures and a summary table from the buffer benchmark.

usage: plot_buffer.py [csv] [outdir] [--pool N] [--conf FILE]

Reads the CSV written by bench/bench_buffer (make bench), averages every
configuration (policy, pattern, pool, read_pct) over its repeats, and
writes into outdir:

  buffer_hit_ratio.pdf/.png      hit ratio vs pool size, one panel per
                                 pattern, at read_pct 100
  buffer_io_vs_mix.pdf/.png      disk I/O per operation
                                 ((physical_reads + total_physical_writes)
                                 / ops) vs read percentage, at one pool
  buffer_io_breakdown.pdf/.png   the disk I/O of the figure above split
                                 into physical reads per operation (solid)
                                 and total physical writes per operation
                                 (dotted) vs read percentage, same pool
  buffer_summary.csv/.tex        means at that pool and plot_read_pct
                                 (the .tex is a booktabs tabular only)

Points are means over the repeats, error bars go from the min to the max.
The pool comes from --pool, else plot_pool in toydb.conf (default 100);
the table's read percentage from plot_read_pct (default 50). The config
file is --conf, else $TOYDB_CONF, else toydb.conf in the current directory,
the same lookup as Config_Init in common/config.c.

Only the standard library and matplotlib are used.
"""

import argparse
import csv
import os
import sys

PROG = "plot_buffer.py"

# fixed order of patterns and policies in every figure and table
PATTERNS = ["uniform", "hotcold", "sequential"]
POLICIES = ["LRU", "MRU"]

# LRU and MRU look the same in every figure. Okabe-Ito blue and vermillion
# are told apart with any common colour vision deficiency; the marker and
# the dash pattern keep them apart in greyscale too. LRU is drawn above
# MRU (higher zorder), so where the two lines coincide the smaller LRU
# circles stay visible on top of the MRU squares. The legend order is the
# drawing order (LRU, MRU), which zorder does not change.
STYLE = {
    "LRU": {"color": "#0072B2", "marker": "o", "linestyle": "-",
            "zorder": 3},
    "MRU": {"color": "#D55E00", "marker": "s", "linestyle": "--",
            "zorder": 2},
}

# hit ratio in figure 1 is taken at all reads (it does not depend on the
# read percentage anyway, the page stream is the same for every mix)
FIG1_READ_PCT = 100

# CSV columns this script needs
NEEDED = ["policy", "pattern", "pool", "read_pct", "repeat", "ops",
          "hits", "physical_reads", "total_physical_writes", "evictions",
          "dirty_evictions", "hit_ratio"]


def die(msg):
    print("%s: %s" % (PROG, msg), file=sys.stderr)
    sys.exit(1)


# ---------------------------------------------------------------- config

def read_conf(path):
    """Read key = value lines with the rules of cfg_split in config.c:
    everything from the first '#' is a comment, key and value are trimmed,
    the line is split at the first '='. A missing file gives {}."""
    conf = {}
    try:
        with open(path) as f:
            for line in f:
                line = line.split("#", 1)[0].strip()
                if "=" not in line:
                    continue
                key, val = line.split("=", 1)
                if key.strip():
                    conf[key.strip()] = val.strip()
    except OSError:
        pass
    return conf


def conf_int(conf, key, default):
    """Like Config_GetInt: the value, or default if missing or bad."""
    if key not in conf:
        return default
    try:
        return int(conf[key])
    except ValueError:
        print("%s: %s = %s is not an int, using %d"
              % (PROG, key, conf[key], default), file=sys.stderr)
        return default


# ---------------------------------------------------------------- data

def load(path):
    """Read the benchmark CSV. Return {(policy, pattern, pool, read_pct):
    [per-run metric dict, ...]}, one entry per repeat."""
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
                ops = int(row["ops"])
                pr = int(row["physical_reads"])
                tpw = int(row["total_physical_writes"])
                de = int(row["dirty_evictions"])
                key = (row["policy"], row["pattern"], int(row["pool"]),
                       int(row["read_pct"]))
                run = {
                    "hit_ratio": float(row["hit_ratio"]),
                    "physical_reads": pr,
                    "total_physical_writes": tpw,
                    "evictions": int(row["evictions"]),
                    "dirty_evictions": de,
                    # per-op values are computed per run, then averaged
                    "io_per_op": (pr + tpw) / ops if ops else 0.0,
                    "reads_per_op": pr / ops if ops else 0.0,
                    "tpw_per_op": tpw / ops if ops else 0.0,
                }
            except (ValueError, TypeError):
                die("%s line %d: bad number" % (path, n))
            groups.setdefault(key, []).append(run)
    if not groups:
        die("%s has no data rows" % path)
    return groups


def stat(groups, key, metric):
    """(mean, min, max) of one metric over the repeats of one config, or
    None if the config is not in the CSV."""
    runs = groups.get(key)
    if not runs:
        return None
    vals = [r[metric] for r in runs]
    return (sum(vals) / len(vals), min(vals), max(vals))


def values(groups, index):
    """Sorted distinct values of one key field (2 = pool, 3 = read_pct)."""
    return sorted({k[index] for k in groups})


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


def series(ax, groups, policy, xs, keyfn, metric, label=None,
           linestyle=None):
    """One policy's line with min/max error bars, in the policy's colour
    and marker. keyfn(x) gives the CSV config key of the point at x;
    points not in the CSV are skipped. label defaults to the policy name,
    linestyle to the policy's own."""
    px, mean, lo, hi = [], [], [], []
    for x in xs:
        s = stat(groups, keyfn(x), metric)
        if s is None:
            continue
        px.append(x)
        mean.append(s[0])
        # max(0, ...): when all repeats are equal the float mean can be a
        # rounding error past the min or max
        lo.append(max(0.0, s[0] - s[1]))
        hi.append(max(0.0, s[2] - s[0]))
    if px:
        style = dict(STYLE[policy])
        if linestyle is not None:
            style["linestyle"] = linestyle
        ax.errorbar(px, mean, yerr=[lo, hi], label=label or policy, **style)


def panel_label(ax, text):
    """Pattern name above a panel, on the left (a label, not a title)."""
    ax.set_title(text, loc="left")


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


def figure_legend(fig, ax, ncol=None):
    """One legend for the whole figure, above the panels (all entries on
    one line unless ncol is given)."""
    handles, labels = ax.get_legend_handles_labels()
    fig.legend(handles, labels, loc="outside upper center",
               ncol=ncol or len(labels), frameon=False)


def fig_hit_ratio(plt, groups, patterns, outdir, file_pages):
    import matplotlib.ticker as ticker

    pools = values(groups, 2)
    fig, axes = plt.subplots(1, len(patterns), sharey=True,
                             figsize=(6.3, 2.3), layout="constrained")
    for ax, pat in zip(axes, patterns):
        for pol in POLICIES:
            series(ax, groups, pol, pools,
                   lambda p: (pol, pat, p, FIG1_READ_PCT), "hit_ratio")
        if file_pages is not None:
            # where the pool becomes as large as the file
            ax.axvline(file_pages, color="0.5", linestyle=":",
                       linewidth=0.9, label="pool = file size")
        ax.set_xscale("log")
        # fixed round ticks: the real pool sizes 900 and 1200 are too
        # close on a log axis to label both
        ax.set_xticks([t for t in (20, 50, 100, 200, 500, 1000)
                       if pools[0] <= t <= pools[-1]])
        ax.xaxis.set_major_formatter(ticker.ScalarFormatter())
        ax.xaxis.set_minor_formatter(ticker.NullFormatter())
        ax.set_xlabel("pool size (frames)")
        ax.set_ylim(-0.02, 1.02)
        panel_label(ax, pat)
    axes[0].set_ylabel("hit ratio (hits / requests)")
    figure_legend(fig, axes[0])
    save(fig, outdir, "buffer_hit_ratio")
    plt.close(fig)


def fig_io_vs_mix(plt, groups, patterns, pool, outdir):
    pcts = values(groups, 3)
    fig, axes = plt.subplots(1, len(patterns), sharey=True,
                             figsize=(6.3, 2.3), layout="constrained")
    for ax, pat in zip(axes, patterns):
        for pol in POLICIES:
            series(ax, groups, pol, pcts,
                   lambda r: (pol, pat, pool, r), "io_per_op")
        ax.set_xlim(-4, 104)
        ax.set_xticks([0, 25, 50, 75, 100])
        ax.set_xlabel("reads (% of operations)")
        panel_label(ax, pat)
    # only after every panel is drawn: setting a limit freezes the other
    # end of the shared axis at what has been plotted so far
    axes[0].set_ylim(bottom=0)
    axes[0].set_ylabel("disk I/O (pages / operation)")
    figure_legend(fig, axes[0])
    save(fig, outdir, "buffer_io_vs_mix")
    plt.close(fig)


def fig_io_breakdown(plt, groups, patterns, pool, outdir):
    """The two parts of the disk I/O in fig_io_vs_mix: physical reads per
    op (solid) and total physical writes per op (dotted), per policy in
    the policy's colour and marker. Reads stay flat across the mix (the
    page stream is the same), writes grow with the write share."""
    pcts = values(groups, 3)
    fig, axes = plt.subplots(1, len(patterns), sharey=True,
                             figsize=(6.3, 2.3), layout="constrained")
    for ax, pat in zip(axes, patterns):
        for pol in POLICIES:
            series(ax, groups, pol, pcts,
                   lambda r: (pol, pat, pool, r), "reads_per_op",
                   label="%s reads" % pol, linestyle="-")
            series(ax, groups, pol, pcts,
                   lambda r: (pol, pat, pool, r), "tpw_per_op",
                   label="%s writes" % pol, linestyle=":")
        ax.set_xlim(-4, 104)
        ax.set_xticks([0, 25, 50, 75, 100])
        ax.set_xlabel("reads (% of operations)")
        panel_label(ax, pat)
    # after every panel is drawn, see fig_io_vs_mix
    axes[0].set_ylim(bottom=0)
    axes[0].set_ylabel("pages / operation")
    # 2 columns: LRU entries in the first, MRU in the second
    figure_legend(fig, axes[0], ncol=2)
    save(fig, outdir, "buffer_io_breakdown")
    plt.close(fig)


# outputs of earlier versions of this script, removed on every run so
# results/ only holds what the current version makes
OBSOLETE = ["buffer_writes_vs_mix.pdf", "buffer_writes_vs_mix.png"]


def remove_obsolete(outdir):
    for name in OBSOLETE:
        path = os.path.join(outdir, name)
        if os.path.exists(path):
            os.remove(path)
            print("%s: removed old %s" % (PROG, path), file=sys.stderr)


# ---------------------------------------------------------------- table

SUMMARY_COLS = ["hit_ratio", "physical_reads", "total_physical_writes",
                "evictions", "dirty_evictions"]


def summary(groups, patterns, pool, read_pct, csv_path, outdir):
    """Means over repeats at (pool, read_pct), as CSV and as a booktabs
    tabular for the report."""
    rows = []
    for pat in patterns:
        for pol in POLICIES:
            key = (pol, pat, pool, read_pct)
            if key not in groups:
                continue
            rows.append((pat, pol, [stat(groups, key, c)[0]
                                    for c in SUMMARY_COLS]))

    path = os.path.join(outdir, "buffer_summary.csv")
    with open(path, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["pattern", "policy"] + SUMMARY_COLS)
        for pat, pol, v in rows:
            w.writerow([pat, pol, "%.6f" % v[0]] + ["%.1f" % x for x in v[1:]])
    print("%s: wrote %s" % (PROG, path), file=sys.stderr)

    path = os.path.join(outdir, "buffer_summary.tex")
    with open(path, "w") as f:
        f.write("%% from %s: pool %d, %d%% reads, means over repeats\n"
                % (os.path.basename(csv_path), pool, read_pct))
        f.write("\\begin{tabular}{llrrrrr}\n\\toprule\n")
        f.write("Pattern & Policy & Hit ratio & Phys.\\ reads & "
                "Phys.\\ writes & Evictions & Dirty evict. \\\\\n")
        last = None
        for pat, pol, v in rows:
            if pat != last:
                f.write("\\midrule\n")
            name = pat.replace("_", "\\_") if pat != last else ""
            f.write("%s & %s & %.3f & %s \\\\\n"
                    % (name, pol, v[0], " & ".join("%.1f" % x for x in v[1:])))
            last = pat
        f.write("\\bottomrule\n\\end{tabular}\n")
    print("%s: wrote %s" % (PROG, path), file=sys.stderr)


# ---------------------------------------------------------------- main

def main():
    ap = argparse.ArgumentParser(prog=PROG, description=__doc__.split("\n")[0])
    ap.add_argument("csv", nargs="?", default="results/bench_buffer.csv")
    ap.add_argument("outdir", nargs="?", default="results")
    ap.add_argument("--pool", type=int, default=None,
                    help="pool size for figures 2-3 and the table "
                         "(default: plot_pool in the config, else 100)")
    ap.add_argument("--conf", default=None,
                    help="config file (default: $TOYDB_CONF, else toydb.conf)")
    args = ap.parse_args()

    conf_path = args.conf or os.environ.get("TOYDB_CONF") or "toydb.conf"
    conf = read_conf(conf_path)
    pool = args.pool if args.pool is not None else \
        conf_int(conf, "plot_pool", 100)
    read_pct = conf_int(conf, "plot_read_pct", 50)
    # only for the "pool = file size" marker in figure 1, left out if the
    # config does not say
    file_pages = conf_int(conf, "buf_file_pages", 0) or None

    plt = setup_matplotlib()
    groups = load(args.csv)

    pools = values(groups, 2)
    pcts = values(groups, 3)
    if pool not in pools:
        die("pool %d is not in %s (pools there: %s)"
            % (pool, args.csv, ", ".join(map(str, pools))))
    for pct, what in ((FIG1_READ_PCT, "figure 1"),
                      (read_pct, "plot_read_pct")):
        if pct not in pcts:
            die("read_pct %d (%s) is not in %s (read_pcts there: %s)"
                % (pct, what, args.csv, ", ".join(map(str, pcts))))
    patterns = [p for p in PATTERNS if any(k[1] == p for k in groups)]
    if not patterns:
        die("%s has none of the patterns %s" % (args.csv, ", ".join(PATTERNS)))

    os.makedirs(args.outdir, exist_ok=True)
    fig_hit_ratio(plt, groups, patterns, args.outdir, file_pages)
    fig_io_vs_mix(plt, groups, patterns, pool, args.outdir)
    fig_io_breakdown(plt, groups, patterns, pool, args.outdir)
    remove_obsolete(args.outdir)
    summary(groups, patterns, pool, read_pct, args.csv, args.outdir)
    return 0


if __name__ == "__main__":
    sys.exit(main())
