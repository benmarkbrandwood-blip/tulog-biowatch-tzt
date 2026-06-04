"""Figure builders for Record-session and BP-session analysis.

All functions return matplotlib Figure objects so they can be:
  - shown inline:          fig.show()   (or plt.show())
  - saved to disk:         fig.savefig("out.png", dpi=150)
  - embedded in a dashboard via plotly/Dash (convert with plotly's mpl adapter)

plot_record_session mirrors the 4-panel MATLAB figure layout.
plot_bp_session      shows RR and PAT series with rolling variance.
plot_batch_summary   replicates the grouped-bar batch results figure.
"""

from __future__ import annotations

from typing import Optional

import numpy as np
import matplotlib
import matplotlib.pyplot as plt
import matplotlib.gridspec as gridspec

from analysis.apnea_pipeline import ApneaResult
from analysis.bp_hrv_bpv import BpSessionResult


# ── Record-session 4-panel plot ────────────────────────────────────────────

def plot_record_session(
    result: ApneaResult,
    title: Optional[str] = None,
    figsize: tuple = (14, 12),
) -> plt.Figure:
    """4-panel plot mirroring the MATLAB Apnea_code_all_databases.m figure.

    Panel 1 — Composite respiratory signal with shifted baseline + intersections
              and visual scoring overlay (if actual_events is set).
    Panel 2 — Nasal signal with respiratory (red) and nasal (black) events.
    Panel 3 — SpO2 and heart rate with detected peaks.
    Panel 4 — Actual vs Predicted apnoea events (stem plot).
    """
    fig = plt.figure(figsize=figsize)
    fig.patch.set_facecolor("white")
    gs  = gridspec.GridSpec(4, 1, figure=fig, hspace=0.45)
    axs = [fig.add_subplot(gs[i]) for i in range(4)]
    t   = result.time_s

    if t is None:
        axs[0].text(0.5, 0.5, "No data", ha="center", va="center")
        return fig

    # ---- Panel 1: composite respiratory
    ax = axs[0]
    if result.resp_all is not None:
        ax.plot(t, result.resp_all, "b", lw=0.8, label="Resp (composite)")
    if result.resp_lp_shifted is not None:
        ax.plot(t, result.resp_lp_shifted, "k", lw=1.0, label="Baseline (shifted)")
    if len(result.resp_intersection_times):
        resp_ix = np.searchsorted(t, result.resp_intersection_times).clip(0, len(t) - 1)
        if result.resp_all is not None:
            ax.plot(result.resp_intersection_times, result.resp_all[resp_ix],
                    "go", ms=4, label="Crossings")
    if len(result.actual_events):
        for ev in result.actual_events:
            ax.axvline(ev, color="r", lw=0.8, ls="--", alpha=0.7)
    ax.set_title("Respiratory signal and intersections" + (f" — {title}" if title else ""))
    ax.set_ylabel("Amplitude")
    ax.legend(fontsize=7, loc="upper right")

    # ---- Panel 2: nasal with event markers
    ax = axs[1]
    if result.nas_raw is not None:
        ax.plot(t, result.nas_raw, "b", lw=0.8, label="Nasal")
    if result.nas_lp_shifted is not None:
        ax.plot(t, result.nas_lp_shifted, "k", lw=1.0, label="Baseline (shifted)")
    ylims = ax.get_ylim()
    y_top = ylims[1] if ylims[1] != 0 else 1.0
    for k, (st, en) in enumerate(zip(result.resp_event_starts, result.resp_event_ends)):
        ax.axvline(st, color="r", lw=1.5, ls=":", label="Resp event" if k == 0 else "")
        ax.text(st, y_top * 0.08, f"{en - st:.1f}s", color="r", fontsize=7, va="bottom")
    for k, (st, en) in enumerate(zip(result.nas_event_starts, result.nas_event_ends)):
        ax.axvline(st, color="k", lw=1.5, ls=":", label="Nasal event" if k == 0 else "")
        ax.text(st, y_top * 0.01, f"{en - st:.1f}s", color="k", fontsize=7, va="bottom")
    ax.set_title("Nasal signal with resp (red) and nasal (black) no-intersection events")
    ax.set_ylabel("Amplitude")
    ax.legend(fontsize=7, loc="upper right")

    # ---- Panel 3: SpO2 and HR
    ax = axs[2]
    if result.sao2_signal is not None:
        ax.plot(t, result.sao2_signal, "b", lw=0.8, label="SpO2 (%)")
    if result.heart_rate is not None:
        ax.plot(t, result.heart_rate, "m", lw=0.8, label="HR (bpm)")
    for ev in result.sao2_event_times:
        ax.axvline(ev, color="r", lw=0.8)
    for ev in result.hr_event_times:
        ax.axvline(ev, color="m", lw=0.8, ls="--")
    ax.set_title("SpO2 and HR with detected events")
    ax.set_ylabel("Value")
    ax.legend(fontsize=7, loc="upper right")

    # ---- Panel 4: actual vs predicted events
    ax = axs[3]
    n = len(t)
    if len(result.actual_events):
        ax.stem(result.actual_events, np.ones(len(result.actual_events)),
                linefmt="b-", markerfmt="b.", basefmt=" ", label="Actual")
    if len(result.verified_events):
        ax.stem(result.verified_events, 0.8 * np.ones(len(result.verified_events)),
                linefmt="r-", markerfmt="r.", basefmt=" ", label="Predicted")
    ax.set_title("Actual vs Predicted apnoea events")
    ax.set_xlabel("Time (s)")
    ax.set_ylabel("Event")
    ax.legend(fontsize=7, loc="upper right")

    # Link x-axes
    axs[1].sharex(axs[0])
    axs[2].sharex(axs[0])
    axs[3].sharex(axs[0])

    return fig


# ── BP-session plot ────────────────────────────────────────────────────────

def plot_bp_session(
    result: BpSessionResult,
    figsize: tuple = (12, 6),
) -> plt.Figure:
    """Two-panel figure: RR interval series (top) and PAT series (bottom)."""
    fig, (ax_rr, ax_pat) = plt.subplots(2, 1, figsize=figsize, facecolor="white")
    fig.suptitle(f"BP session — {result.session_path}", fontsize=10)

    times = result.beat_times_s
    x_label = "Beat time (s)" if times is not None else "Beat index"

    if result.rr_ms is not None:
        x = times if times is not None else np.arange(len(result.rr_ms))
        ax_rr.plot(x, result.rr_ms, "b.-", ms=3, lw=0.8, label="RR (ms)")
        ax_rr.set_ylabel("RR interval (ms)")
        ax_rr.set_title(
            f"RR series — RMSSD={result.rmssd_ms:.1f} ms  SDNN={result.sdnn_ms:.1f} ms  "
            f"mean HR={result.mean_hr_bpm:.1f} bpm  pNN50={result.pnn50_pct:.1f}%"
        )
        ax_rr.legend(fontsize=7)

    if result.pat_ms is not None:
        x = times if times is not None else np.arange(len(result.pat_ms))
        ax_pat.plot(x, result.pat_ms, "g.-", ms=3, lw=0.8, label="PAT (ms)")
        if result.rolling_pat_var is not None:
            ax2 = ax_pat.twinx()
            valid = ~np.isnan(result.rolling_pat_var)
            x_var = x[valid] if hasattr(x, "__len__") else np.where(valid)[0]
            ax2.plot(x_var, result.rolling_pat_var[valid],
                     "r-", lw=0.8, alpha=0.6, label="Rolling var")
            ax2.set_ylabel("PAT variance (ms²)", color="r")
        ax_pat.set_ylabel("PAT (ms)")
        ax_pat.set_title(
            f"PAT series — mean={result.pat_mean_ms:.2f} ms  SD={result.pat_sd_ms:.2f} ms"
        )
        ax_pat.legend(fontsize=7)

    ax_pat.set_xlabel(x_label)
    fig.tight_layout()
    return fig


# ── Batch summary bar charts ───────────────────────────────────────────────

def plot_batch_summary(
    results: list[ApneaResult],
    labels: list[str] | None = None,
    figsize: tuple = (14, 9),
) -> plt.Figure:
    """4-panel grouped bar chart mirroring the MATLAB batch summary figure."""
    if not results:
        return plt.figure()

    n = len(results)
    if labels is None:
        labels = [str(i + 1) for i in range(n)]

    precision    = np.array([r.precision   for r in results])
    sensitivity  = np.array([r.sensitivity for r in results])
    specificity  = np.array([r.specificity for r in results])
    f1           = np.array([r.f1_score    for r in results])
    tp_arr       = np.array([r.tp          for r in results])
    fp_arr       = np.array([r.fp          for r in results])
    tn_arr       = np.array([r.tn          for r in results])
    actual_arr   = np.array([r.ann_actual  for r in results])
    pred_arr     = np.array([r.ann_predicted for r in results])

    x = np.arange(n)
    w = 0.2

    fig, axs = plt.subplots(2, 2, figsize=figsize, facecolor="white")
    fig.suptitle("Batch apnoea detection results", fontsize=12)

    # Panel 1: Precision / Sensitivity / Specificity / F1
    ax = axs[0, 0]
    for i, (arr, lbl) in enumerate(zip(
        [precision, sensitivity, specificity, f1],
        ["Precision", "Sensitivity", "Specificity", "F1"],
    )):
        ax.bar(x + i * w, np.nan_to_num(arr), w, label=lbl)
    ax.set_xticks(x + 1.5 * w); ax.set_xticklabels(labels, rotation=45, ha="right")
    ax.set_ylabel("Score"); ax.set_ylim(0, 1.1)
    ax.set_title("Classification metrics"); ax.legend(fontsize=7); ax.grid(axis="y", alpha=0.4)

    # Panel 2: TP / FP / TN grouped
    ax = axs[0, 1]
    for i, (arr, lbl, col) in enumerate(zip(
        [tp_arr, fp_arr, tn_arr],
        ["TP", "FP", "TN"],
        ["tab:green", "tab:red", "tab:blue"],
    )):
        ax.bar(x + i * w, arr, w, label=lbl, color=col)
    ax.set_xticks(x + w); ax.set_xticklabels(labels, rotation=45, ha="right")
    ax.set_ylabel("Count"); ax.set_title("TP / FP / TN per subject")
    ax.legend(fontsize=7); ax.grid(axis="y", alpha=0.4)

    # Panel 3: % actual and predicted events as TP
    ax = axs[1, 0]
    pct_actual = np.where(actual_arr > 0, 100 * tp_arr / actual_arr, np.nan)
    pct_pred   = np.where(pred_arr   > 0, 100 * tp_arr / pred_arr,   np.nan)
    bars = ax.bar(x - w / 2, np.nan_to_num(pct_actual), w, label="% Actual as TP")
    ax.bar(x + w / 2, np.nan_to_num(pct_pred),   w, label="% Predicted as TP")
    for bar, n_ev in zip(bars, actual_arr):
        ax.text(bar.get_x() + bar.get_width() / 2, bar.get_height() + 1,
                str(n_ev), ha="center", fontsize=7, fontweight="bold")
    ax.set_xticks(x); ax.set_xticklabels(labels, rotation=45, ha="right")
    ax.set_ylabel("% events"); ax.set_title("Percentage of events as TP")
    ax.legend(fontsize=7); ax.grid(axis="y", alpha=0.4)

    # Panel 4: TP / FP / TN stacked
    ax = axs[1, 1]
    ax.bar(x, tp_arr, label="TP", color="tab:green")
    ax.bar(x, fp_arr, bottom=tp_arr, label="FP", color="tab:red")
    ax.bar(x, tn_arr, bottom=tp_arr + fp_arr, label="TN", color="tab:blue")
    ax.set_xticks(x); ax.set_xticklabels(labels, rotation=45, ha="right")
    ax.set_ylabel("Count"); ax.set_title("TP / FP / TN stacked")
    ax.legend(fontsize=7); ax.grid(axis="y", alpha=0.4)

    fig.tight_layout()
    return fig
