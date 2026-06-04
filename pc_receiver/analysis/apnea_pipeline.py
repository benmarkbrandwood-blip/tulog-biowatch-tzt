"""End-to-end Record-session apnoea analysis pipeline.

Orchestrates the Python port of Apnea_code_all_databases.m:

  Step 1  Load signals from a Record CSV; identify available channels.
  Step 2  Detect ECG R-peaks and derive heart-rate signal.
  Step 3  Preprocess respiratory channels (LP → DC → normalise).
  Step 4  Build moving-window enhanced baselines.
  Step 5  Shift baselines by 0.5 s.
  Step 6  Find raw-vs-baseline intersections.
  Step 7  Find no-intersection events > 10 s.
  Step 8  Merge nasal and respiratory candidate events (20 s window).
  Step 9  Detect SpO2 desaturation and HR surge events.
  Step 10 Optionally add PAT variance and RR variance events.
  Step 11 Merge physiological markers (20 s window).
  Step 12 Verify events: require a preceding respiratory event within 0–50 s.
  Step 13 If visual scoring provided, compute TP/FP/FN/TN and metrics.
  Step 14 Return ApneaResult dataclass.

The result object contains all intermediate arrays so they can be inspected
by a CLI tool, saved for debugging, or plotted by visualisation.py.
"""

from __future__ import annotations

import math
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional

import numpy as np
import pandas as pd

import config as _cfg
from analysis.ecg_processing import calculate_heart_rate, detect_r_peaks
from analysis.intersection_detector import (
    find_intersections,
    find_no_intersection_events,
    merge_event_starts,
)
from analysis.physio_markers import (
    detect_hr_surge_events,
    detect_pat_variance_events,
    detect_rr_variance_events,
    detect_sao2_events,
    merge_physio_events,
)
from analysis.signal_processing import (
    build_enhanced_baseline,
    lp_filter,
    normalise,
    shift_signal,
    simple_dc,
)


# ── Column name aliases ────────────────────────────────────────────────────
# Maps canonical channel names to possible CSV column name variants
_CHANNEL_ALIASES: dict[str, list[str]] = {
    "ecg":    ["ecg", "ECG"],
    "nasal":  ["nasal", "NAF", "NAF2P_A1", "naf2p_a1", "airflow"],
    "resp_t": ["resp_t", "VTH", "vth", "thoracic", "effort_t"],
    "resp_a": ["resp_a", "VAB", "vab", "abdominal", "effort_a"],
    "sao2":   ["sao2", "SAO2", "spo2", "SpO2"],
    "time_ms":["time_ms"],
    "rr_us":  ["rr_us"],
    "pat_us": ["pat_us"],
}


def _find_col(df: pd.DataFrame, key: str) -> Optional[str]:
    for alias in _CHANNEL_ALIASES.get(key, [key]):
        if alias in df.columns:
            return alias
    return None


# ── Result dataclass ───────────────────────────────────────────────────────

@dataclass
class ApneaResult:
    # ---- pipeline metadata
    session_path:        str            = ""
    fs:                  float          = 200.0
    duration_sec:        float          = 0.0
    channels_available:  list[str]      = field(default_factory=list)

    # ---- intermediate arrays (all times in seconds)
    nas_intersection_times:   np.ndarray = field(default_factory=lambda: np.array([]))
    resp_intersection_times:  np.ndarray = field(default_factory=lambda: np.array([]))
    nas_event_starts:         np.ndarray = field(default_factory=lambda: np.array([]))
    nas_event_ends:           np.ndarray = field(default_factory=lambda: np.array([]))
    resp_event_starts:        np.ndarray = field(default_factory=lambda: np.array([]))
    resp_event_ends:          np.ndarray = field(default_factory=lambda: np.array([]))
    combined_resp_locs:       np.ndarray = field(default_factory=lambda: np.array([]))

    hr_event_times:           np.ndarray = field(default_factory=lambda: np.array([]))
    sao2_event_times:         np.ndarray = field(default_factory=lambda: np.array([]))
    pat_event_times:          np.ndarray = field(default_factory=lambda: np.array([]))
    rr_event_times:           np.ndarray = field(default_factory=lambda: np.array([]))
    merged_physio_events:     np.ndarray = field(default_factory=lambda: np.array([]))
    verified_events:          np.ndarray = field(default_factory=lambda: np.array([]))

    # ---- evaluation (populated when visual scoring is supplied)
    actual_events:            np.ndarray = field(default_factory=lambda: np.array([]))
    tp:  int = 0
    fp:  int = 0
    fn:  int = 0
    tn:  int = 0
    precision:   float = float("nan")
    sensitivity: float = float("nan")
    specificity: float = float("nan")
    f1_score:    float = float("nan")
    ann_actual:    int = 0
    ann_predicted: int = 0

    # ---- raw signals (kept for visualisation)
    time_s:           Optional[np.ndarray] = None
    ecg_signal:       Optional[np.ndarray] = None
    heart_rate:       Optional[np.ndarray] = None
    heart_rate_dc:    Optional[np.ndarray] = None
    nas_raw:          Optional[np.ndarray] = None
    nas_lp_shifted:   Optional[np.ndarray] = None
    resp_all:         Optional[np.ndarray] = None
    resp_lp_shifted:  Optional[np.ndarray] = None
    sao2_signal:      Optional[np.ndarray] = None

    error: Optional[str] = None


# ── Main pipeline entry point ──────────────────────────────────────────────

def run_record_session_pipeline(
    csv_path: str | Path,
    fs: float = _cfg.APNEA_FS,
    window_sec: float = _cfg.APNEA_WINDOW_SEC,
    min_gap_sec: float = _cfg.APNEA_MIN_GAP_SEC,
    resp_merge_sec: float = _cfg.APNEA_RESP_MERGE_SEC,
    physio_merge_sec: float = _cfg.APNEA_PHYSIO_MERGE_SEC,
    verify_backward_sec: float = _cfg.APNEA_VERIFY_BACKWARD_SEC,
    baseline_shift_sec: float = _cfg.APNEA_BASELINE_SHIFT_SEC,
    o2_thresh: float = _cfg.APNEA_O2_THRESH,
    hr_thresh_factor: float = _cfg.APNEA_HR_THRESH_FACTOR,
    use_pat_variance: bool = True,
    use_rr_variance: bool = True,
    visual_scoring_path: str | Path | None = None,
) -> ApneaResult:
    """Run the full apnoea pipeline on one Record-session CSV.

    Parameters
    ----------
    csv_path             : path to a Record-session CSV from the watch inbox.
    visual_scoring_path  : optional path to a text file with annotated events
                           (same format as the MATLAB Visual_scoring1_excerptN.txt)
                           for TP/FP/FN evaluation.
    """
    result = ApneaResult(session_path=str(csv_path), fs=fs)
    try:
        _run(result, csv_path, fs, window_sec, min_gap_sec, resp_merge_sec,
             physio_merge_sec, verify_backward_sec, baseline_shift_sec,
             o2_thresh, hr_thresh_factor, use_pat_variance, use_rr_variance,
             visual_scoring_path)
    except Exception as exc:
        result.error = str(exc)
    return result


# ── Pipeline internals ─────────────────────────────────────────────────────

def _run(
    r: ApneaResult,
    csv_path, fs, window_sec, min_gap_sec, resp_merge_sec,
    physio_merge_sec, verify_backward_sec, baseline_shift_sec,
    o2_thresh, hr_thresh_factor, use_pat, use_rr, scoring_path,
):
    # Step 1 — load
    df = pd.read_csv(csv_path)
    n  = len(df)
    t  = np.arange(n) / fs
    r.duration_sec = t[-1]
    r.time_s       = t

    available = [k for k in _CHANNEL_ALIASES if _find_col(df, k) is not None]
    r.channels_available = available

    def col(key):
        c = _find_col(df, key)
        return df[c].to_numpy(dtype=float) if c else None

    ecg_raw = col("ecg")
    nasal   = col("nasal")
    resp_t  = col("resp_t")
    resp_a  = col("resp_a")
    sao2    = col("sao2")
    rr_us   = col("rr_us")
    pat_us  = col("pat_us")

    # Step 2 — ECG
    if ecg_raw is not None:
        locs, _ = detect_r_peaks(ecg_raw, fs)
        hr      = calculate_heart_rate(locs, fs, n)
        hr_dc   = simple_dc(fs, hr) if not np.all(np.isnan(hr)) else hr
        r.ecg_signal    = ecg_raw
        r.heart_rate    = hr
        r.heart_rate_dc = hr_dc
        r.hr_event_times = detect_hr_surge_events(
            hr_dc, fs, thresh_factor=hr_thresh_factor
        )

    # Steps 3–7 — respiratory channels
    if nasal is not None:
        nas_raw = normalise(lp_filter(fs, nasal, 0.5))
        nas_lp  = build_enhanced_baseline(nas_raw, window_sec, fs, weight=1.0)
        nas_lp_s = shift_signal(nas_lp, baseline_shift_sec, fs)
        nas_ix   = find_intersections(nas_raw, nas_lp_s)
        nas_times = t[nas_ix]
        r.nas_raw            = nas_raw
        r.nas_lp_shifted     = nas_lp_s
        r.nas_intersection_times = nas_times
        r.nas_event_starts, r.nas_event_ends = find_no_intersection_events(
            nas_times, r.duration_sec, min_gap_sec
        )

    if resp_t is not None and resp_a is not None:
        rt_raw   = normalise(lp_filter(fs, simple_dc(fs, resp_t), 0.5))
        ra_raw   = normalise(lp_filter(fs, simple_dc(fs, resp_a), 0.5))
        resp_all = rt_raw + ra_raw
        resp_lp  = build_enhanced_baseline(resp_all, window_sec, fs, weight=0.5)
        resp_lp_s = shift_signal(resp_lp, baseline_shift_sec, fs)
        resp_ix   = find_intersections(resp_all, resp_lp_s)
        resp_times = t[resp_ix]
        r.resp_all            = resp_all
        r.resp_lp_shifted     = resp_lp_s
        r.resp_intersection_times = resp_times
        r.resp_event_starts, r.resp_event_ends = find_no_intersection_events(
            resp_times, r.duration_sec, min_gap_sec
        )

    # Step 8 — merge nasal + resp candidate starts
    arrays_for_merge = [
        a for a in (r.nas_event_starts, r.resp_event_starts) if len(a) > 0
    ]
    r.combined_resp_locs = (
        merge_event_starts(*arrays_for_merge, merge_window_sec=resp_merge_sec)
        if arrays_for_merge else np.array([])
    )

    # Steps 9–10 — SpO2 / PAT / RR
    if sao2 is not None:
        r.sao2_signal    = sao2
        r.sao2_event_times = detect_sao2_events(sao2, fs, drop_thresh=o2_thresh)

    if use_pat and pat_us is not None:
        r.pat_event_times = detect_pat_variance_events(pat_us, fs)

    if use_rr and rr_us is not None:
        r.rr_event_times = detect_rr_variance_events(rr_us, fs)

    # Step 11 — merge physiological markers
    r.merged_physio_events = merge_physio_events(
        r.hr_event_times,
        r.sao2_event_times,
        r.pat_event_times if use_pat else None,
        r.rr_event_times  if use_rr  else None,
        merge_window_sec=physio_merge_sec,
    )

    # Step 12 — verify: each physio event needs a respiratory event 0–50 s before it
    r.verified_events = _verify_events(
        r.merged_physio_events, r.combined_resp_locs, verify_backward_sec
    )

    # Step 13 — optional evaluation against visual scoring
    if scoring_path is not None:
        actual = _load_visual_scoring(scoring_path)
        r.actual_events = actual
        _compute_metrics(r)


def _verify_events(
    physio: np.ndarray,
    resp_locs: np.ndarray,
    backward_sec: float,
) -> np.ndarray:
    """Return the most recent respiratory event within [0, backward_sec] before each physio event."""
    verified: list[float] = []
    for t in physio:
        look_back = resp_locs[(t - resp_locs >= 0) & (t - resp_locs <= backward_sec)]
        if len(look_back):
            verified.append(float(np.max(look_back)))
    return np.array(verified)


def _load_visual_scoring(path) -> np.ndarray:
    """Parse a Visual_scoring text file; return array of apnoea/hypopnoea times (s)."""
    times: list[float] = []
    current_type = ""
    with open(path, encoding="utf-8") as fh:
        for line in fh:
            line = line.strip()
            if not line:
                continue
            if line.startswith("[") and "]" in line:
                current_type = line[1: line.index("]")]
            else:
                parts = line.split()
                if len(parts) >= 1 and current_type:
                    lower = current_type.lower()
                    if "apnea" in lower or "hypopnea" in lower:
                        try:
                            times.append(float(parts[0]))
                        except ValueError:
                            pass
    return np.array(times)


def _compute_metrics(r: ApneaResult):
    actual  = r.actual_events * r.fs   # convert to samples
    pred    = r.verified_events         # already in seconds
    pred_s  = np.round(pred * r.fs).astype(int)
    act_s   = np.round(actual).astype(int)

    r.ann_actual    = len(act_s)
    r.ann_predicted = len(pred_s)

    remaining = list(act_s)
    tp, fp = 0, 0
    window = round(20 * r.fs)

    for loc in pred_s:
        nearby = [i for i, a in enumerate(remaining) if abs(a - loc) <= window]
        if nearby:
            tp += 1
            remaining.pop(nearby[0])
        else:
            fp += 1

    fn = len(remaining)
    total_windows = math.floor(r.duration_sec / 40)
    tn = max(0, total_windows - (tp + fp + fn))

    r.tp, r.fp, r.fn, r.tn = tp, fp, fn, tn
    r.precision   = tp / (tp + fp)  if (tp + fp) > 0 else float("nan")
    r.sensitivity = tp / (tp + fn)  if (tp + fn) > 0 else float("nan")
    r.specificity = tn / (tn + fp)  if (tn + fp) > 0 else float("nan")
    p, s = r.precision, r.sensitivity
    r.f1_score = 2 * p * s / (p + s) if (p + s) > 0 else float("nan")


# ── Batch runner ───────────────────────────────────────────────────────────

def run_batch(
    csv_paths: list[str | Path],
    scoring_paths: list[str | Path | None] | None = None,
    **pipeline_kwargs,
) -> list[ApneaResult]:
    """Run the pipeline on a list of session CSVs.  Returns one result per session."""
    if scoring_paths is None:
        scoring_paths = [None] * len(csv_paths)
    return [
        run_record_session_pipeline(p, visual_scoring_path=s, **pipeline_kwargs)
        for p, s in zip(csv_paths, scoring_paths)
    ]
