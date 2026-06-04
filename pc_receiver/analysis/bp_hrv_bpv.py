"""BP-session HRV and BPV analysis.

Analyses a BP-session CSV (columns: time_ms, ecg, ppg, drift_ms,
r_peak_ms, rr_us, pat_us) and computes:

  HRV metrics  — RMSSD, SDNN, pNN50, mean HR
  BPV metrics  — PAT mean, SD, variance; rolling PAT variance
  Session info — beat count, valid duration, signal quality flag

Returns a BpSessionResult dataclass suitable for CLI output or dashboard use.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional

import numpy as np
import pandas as pd

import config as _cfg


# ── Result dataclass ───────────────────────────────────────────────────────

@dataclass
class BpSessionResult:
    session_path: str = ""

    # ---- HRV
    rmssd_ms:  float = float("nan")
    sdnn_ms:   float = float("nan")
    pnn50_pct: float = float("nan")
    mean_hr_bpm: float = float("nan")
    beat_count:  int  = 0

    # ---- PAT / BPV
    pat_mean_ms: float = float("nan")
    pat_sd_ms:   float = float("nan")
    pat_var_ms2: float = float("nan")

    # ---- Session quality
    valid_duration_sec: float = float("nan")
    quality_ok:         bool  = False
    error: Optional[str] = None

    # ---- Raw series (for visualisation)
    rr_ms:        Optional[np.ndarray] = None
    pat_ms:       Optional[np.ndarray] = None
    rolling_pat_var: Optional[np.ndarray] = None
    beat_times_s: Optional[np.ndarray] = None


# ── Main entry point ───────────────────────────────────────────────────────

def run_bp_session_analysis(
    csv_path: str | Path,
    nn50_threshold_ms: float = _cfg.HRV_NN50_THRESHOLD_MS,
    min_beats: int = 10,
    pat_variance_window_sec: float = _cfg.PAT_VARIANCE_WINDOW_SEC,
) -> BpSessionResult:
    r = BpSessionResult(session_path=str(csv_path))
    try:
        _run(r, csv_path, nn50_threshold_ms, min_beats, pat_variance_window_sec)
    except Exception as exc:
        r.error = str(exc)
    return r


# ── Internals ──────────────────────────────────────────────────────────────

def _run(r, csv_path, nn50_thresh, min_beats, pat_win_sec):
    df = pd.read_csv(csv_path)

    # ---- extract beat timing
    rr_us  = _col(df, "rr_us")
    pat_us = _col(df, "pat_us")
    time_ms = _col(df, "time_ms")

    if rr_us is None:
        raise ValueError("CSV has no rr_us column — cannot compute HRV")

    # Drop non-positive (artefact) values
    valid_mask = rr_us > 0
    rr_us  = rr_us[valid_mask]

    if pat_us is not None:
        pat_us = pat_us[valid_mask]
    if time_ms is not None:
        time_ms = time_ms[valid_mask]

    r.beat_count = len(rr_us)
    if r.beat_count < min_beats:
        raise ValueError(f"Only {r.beat_count} valid beats — session too short")

    rr_ms  = rr_us  / 1000.0
    r.rr_ms = rr_ms
    r.valid_duration_sec = float(np.sum(rr_ms) / 1000.0)
    r.quality_ok = r.beat_count >= min_beats and r.valid_duration_sec > 30

    if time_ms is not None:
        r.beat_times_s = time_ms / 1000.0

    # ---- HRV
    r.rmssd_ms   = compute_rmssd(rr_ms)
    r.sdnn_ms    = float(np.std(rr_ms, ddof=1))
    r.mean_hr_bpm = float(60_000.0 / np.mean(rr_ms))
    r.pnn50_pct  = compute_pnn50(rr_ms, threshold_ms=nn50_thresh)

    # ---- PAT / BPV
    if pat_us is not None:
        pat_ms       = pat_us / 1000.0
        r.pat_ms     = pat_ms
        r.pat_mean_ms = float(np.mean(pat_ms))
        r.pat_sd_ms   = float(np.std(pat_ms, ddof=1))
        r.pat_var_ms2 = float(np.var(pat_ms, ddof=1))
        r.rolling_pat_var = _rolling_variance(pat_ms, pat_win_sec)


def _col(df: pd.DataFrame, name: str) -> np.ndarray | None:
    if name in df.columns:
        return df[name].to_numpy(dtype=float)
    return None


# ── HRV metric helpers ─────────────────────────────────────────────────────

def compute_rmssd(rr_ms: np.ndarray) -> float:
    """Root-mean-square of successive differences (ms)."""
    if len(rr_ms) < 2:
        return float("nan")
    diffs = np.diff(rr_ms)
    return float(np.sqrt(np.mean(diffs ** 2)))


def compute_pnn50(rr_ms: np.ndarray, threshold_ms: float = 50.0) -> float:
    """Percentage of successive RR differences > threshold_ms."""
    if len(rr_ms) < 2:
        return float("nan")
    diffs = np.abs(np.diff(rr_ms))
    return float(100.0 * np.sum(diffs > threshold_ms) / len(diffs))


def _rolling_variance(series: np.ndarray, window_sec: float, fs: float = 1.0) -> np.ndarray:
    """Rolling variance over a window of window_sec / fs samples.

    Beat-series data: for a typical ~1 Hz beat rate, fs=1.0 means window_sec
    directly gives the number of beats in the window.
    """
    win = max(2, round(window_sec * fs))
    n   = len(series)
    var = np.full(n, np.nan)
    for i in range(win, n):
        var[i] = np.var(series[i - win : i], ddof=1)
    return var
