#!/usr/bin/env python3
"""
Plot ECG and PPG signals from a biowatch CSV recording.

R-peak detection events are drawn as vertical dashed lines on both subplots,
positioned at the r_peak_ms value (recording-relative time of the detected peak,
not the row timestamp).

Usage:
    python3 plot_signals.py recording.csv
    python3 plot_signals.py recording.csv --start 2 --end 10
"""

import sys
import argparse
import pandas as pd
import matplotlib.pyplot as plt
import matplotlib.lines as mlines


def load_csv(path: str) -> pd.DataFrame:
    return pd.read_csv(path, comment="#")


def main() -> None:
    parser = argparse.ArgumentParser(description="Plot biowatch ECG/PPG recording")
    parser.add_argument("csv", help="Path to recording CSV file")
    parser.add_argument("--start", type=float, default=None,
                        help="Start of display window (seconds)")
    parser.add_argument("--end", type=float, default=None,
                        help="End of display window (seconds)")
    args = parser.parse_args()

    df = load_csv(args.csv)

    # X axis: recording-relative time in seconds, derived from time_ms.
    # This is equivalent to sample_index / sample_rate for ideal 100 Hz timing.
    df["time_s"] = df["time_ms"] / 1000.0

    if args.start is not None:
        df = df[df["time_s"] >= args.start]
    if args.end is not None:
        df = df[df["time_s"] <= args.end]

    if df.empty:
        sys.exit("No data in the requested time window.")

    # R-peak events: rows where r_peak_ms is non-zero carry the recording-relative
    # timestamp of the detected R-peak (not the timestamp of the row itself).
    r_peak_s = df.loc[df["r_peak_ms"] > 0, "r_peak_ms"] / 1000.0

    fig, (ax_ecg, ax_ppg) = plt.subplots(
        2, 1, figsize=(14, 7), sharex=True,
        gridspec_kw={"hspace": 0.08}
    )

    # ECG
    ax_ecg.plot(df["time_s"], df["ecg"], color="steelblue", linewidth=0.7, label="ECG")
    for t in r_peak_s:
        ax_ecg.axvline(x=t, color="crimson", linewidth=0.9, linestyle="--", alpha=0.75)
    ax_ecg.set_ylabel("ECG (ADC counts)")
    ax_ecg.grid(True, alpha=0.25)

    # PPG
    ax_ppg.plot(df["time_s"], df["ppg"], color="darkorange", linewidth=0.7, label="PPG")
    for t in r_peak_s:
        ax_ppg.axvline(x=t, color="crimson", linewidth=0.9, linestyle="--", alpha=0.75)
    ax_ppg.set_ylabel("PPG (ADC counts)")
    ax_ppg.set_xlabel("Time (s)  [= sample index / 100 Hz]")
    ax_ppg.grid(True, alpha=0.25)

    # Shared legend entry for R-peak lines
    r_handle = mlines.Line2D(
        [], [], color="crimson", linewidth=0.9, linestyle="--", alpha=0.75,
        label=f"R-peak ({len(r_peak_s)} detected)"
    )
    ax_ecg.legend(handles=[ax_ecg.get_lines()[0], r_handle], loc="upper right")
    ax_ppg.legend(handles=[ax_ppg.get_lines()[0], r_handle], loc="upper right")

    title = args.csv
    if args.start is not None or args.end is not None:
        lo = f"{args.start:.1f}s" if args.start is not None else "start"
        hi = f"{args.end:.1f}s" if args.end is not None else "end"
        title += f"  [{lo} – {hi}]"
    fig.suptitle(title, fontsize=9)

    plt.show()


if __name__ == "__main__":
    main()
