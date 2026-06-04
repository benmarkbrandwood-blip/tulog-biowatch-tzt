"""Physiological event detection: SpO2, HR surge, PAT variance, RR variance.

Ported from Apnea_code_all_databases.m and extended with PAT/RR variance
markers as described in pc_receiver_stage_plan.md.

All detect_* functions return a 1-D numpy array of event times in seconds.
merge_physio_events combines them using the same greedy merge used in MATLAB.
"""

from __future__ import annotations

import numpy as np
from scipy import signal as _sig

from analysis.intersection_detector import merge_event_starts


# ── SpO2 desaturation events ───────────────────────────────────────────────

def detect_sao2_events(
    sao2: np.ndarray,
    fs: float,
    drop_thresh: float = 2.0,
    min_duration_sec: float = 3.0,
    min_peak_distance_sec: float = 10.0,
    min_peak_prominence: float = 0.2,
) -> np.ndarray:
    """Detect SpO2 desaturation events.

    Tracks the most recent local peak in the SpO2 signal and flags any period
    where SpO2 falls more than drop_thresh % below that peak for longer than
    min_duration_sec.

    Mirrors the MATLAB O2 event detection loop:
        [peak_values, peak_locs] = findpeaks(SAO2, ...)
        for i = 2:length(SAO2)
            if SAO2(i) < last_peak - O2Thresh ...
    """
    min_peak_dist = round(min_peak_distance_sec * fs)
    peak_locs, _ = _sig.find_peaks(
        sao2,
        distance=max(1, min_peak_dist),
        prominence=min_peak_prominence,
    )

    if len(peak_locs) == 0:
        return np.array([])

    events: list[float] = []
    last_peak = sao2[0]
    in_event  = False
    event_start = 0

    peak_set = set(peak_locs.tolist())
    for i in range(1, len(sao2)):
        if i in peak_set:
            last_peak = sao2[i]
        if sao2[i] < last_peak - drop_thresh:
            if not in_event:
                event_start = i
                in_event = True
        else:
            if in_event:
                duration_sec = (i - event_start) / fs
                if duration_sec > min_duration_sec:
                    events.append(event_start / fs)
                in_event = False

    return np.array(events)


# ── HR surge events ────────────────────────────────────────────────────────

def detect_hr_surge_events(
    heart_rate_dc: np.ndarray,
    fs: float,
    thresh_factor: float = 2.7,
    min_peak_dist_sec: float = 30.0,
) -> np.ndarray:
    """Detect HR surge peaks in the DC-removed heart-rate signal.

    Mirrors the MATLAB:
        HrThresh_val = 2.7 * mean(abs(heart_rate2))
        [HR_values, HR_locs] = findpeaks(heart_rate2,
            'MinPeakDistance', 30*fs, 'MinPeakProminence', HrThresh_val)
    """
    threshold = thresh_factor * np.nanmean(np.abs(heart_rate_dc))
    min_dist  = max(1, round(min_peak_dist_sec * fs))
    locs, _   = _sig.find_peaks(heart_rate_dc, prominence=threshold, distance=min_dist)
    return locs / fs


# ── PAT variance spike events ──────────────────────────────────────────────

def detect_pat_variance_events(
    pat_us: np.ndarray,
    fs: float,
    window_sec: float = 20.0,
    multiplier: float = 3.0,
) -> np.ndarray:
    """Detect PAT variance spikes that may indicate sympathetic arousal.

    Computes a rolling variance of the PAT (Pulse Arrival Time) series,
    estimates a baseline variance level, and flags windows where the rolling
    variance exceeds multiplier * baseline.

    Returns event times (seconds) at the start of each spike window.
    """
    if len(pat_us) == 0:
        return np.array([])

    win = max(2, round(window_sec * fs))
    n   = len(pat_us)

    rolling_var = np.full(n, np.nan)
    for i in range(win, n):
        rolling_var[i] = np.var(pat_us[i - win : i])

    valid = rolling_var[~np.isnan(rolling_var)]
    if len(valid) == 0:
        return np.array([])

    baseline = np.median(valid)
    if baseline == 0:
        return np.array([])

    threshold = multiplier * baseline
    above     = (rolling_var > threshold).astype(float)
    # Rising edges of threshold crossings
    edges = np.where(np.diff(np.concatenate([[0], above])) > 0)[0]
    return edges / fs


# ── RR variance spike events ───────────────────────────────────────────────

def detect_rr_variance_events(
    rr_us: np.ndarray,
    fs: float,
    window_sec: float = 20.0,
    multiplier: float = 3.0,
) -> np.ndarray:
    """Detect abrupt RR-interval variance increases.

    Uses the same rolling-variance spike approach as PAT variance detection.
    RR intervals supplied as raw microsecond values (from the watch CSV).
    'fs' is the signal sampling rate used to convert sample counts to seconds.

    Beats arrive ~1 per second so a window_sec of 20 s covers ~20 beats.
    """
    return detect_pat_variance_events(rr_us, fs, window_sec=window_sec, multiplier=multiplier)


# ── Combined physiological event pool ─────────────────────────────────────

def merge_physio_events(
    hr_times: np.ndarray,
    sao2_times: np.ndarray,
    pat_times: np.ndarray | None = None,
    rr_times: np.ndarray | None = None,
    merge_window_sec: float = 20.0,
) -> np.ndarray:
    """Merge all physiological event streams into a single sorted, de-clustered list.

    Combines HR, SpO2, and optionally PAT/RR variance events within
    merge_window_sec, keeping the earliest event in each cluster.

    Mirrors the MATLAB merge pattern:
        all_events = sort([HR_times(:); O2annotation_times(:)]);
        merged_events = [all_events(1)];
        for i = 2:length(all_events)
            if all_events(i) - merged_events(end) > 20 ...
    """
    arrays = [a for a in (hr_times, sao2_times, pat_times, rr_times)
              if a is not None and len(a) > 0]
    if not arrays:
        return np.array([])
    return merge_event_starts(*arrays, merge_window_sec=merge_window_sec)
