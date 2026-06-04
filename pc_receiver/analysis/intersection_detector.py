"""Respiratory signal crossing detection and no-intersection event logic.

Direct Python port of the MATLAB helpers:
  find_intersections(raw, baseline_shifted)
  find_no_intersection_events(times, total_duration, min_gap_duration)

Plus a shared merge_event_starts helper used by the apnoea pipeline.
"""

from __future__ import annotations

import numpy as np


def find_intersections(raw: np.ndarray, baseline: np.ndarray) -> np.ndarray:
    """Return sample indices where raw crosses the shifted baseline.

    A crossing occurs wherever (raw - baseline) changes sign.  The returned
    index is the sample *before* the sign change (matching MATLAB 1-indexing
    offset behaviour).

    Corresponds to:
        NAS_intersections = find_intersections(NAS_raw, NAS_LP_shifted)
    """
    diff = raw - baseline
    sign = np.sign(diff)
    # Replace zeros with the previous non-zero sign to avoid spurious crossings
    for i in range(1, len(sign)):
        if sign[i] == 0:
            sign[i] = sign[i - 1]
    crossings = np.where(np.diff(sign))[0]
    return crossings


def find_no_intersection_events(
    times: np.ndarray,
    total_duration: float,
    min_gap: float,
) -> tuple[np.ndarray, np.ndarray]:
    """Find gaps between intersection times that exceed min_gap seconds.

    Returns
    -------
    starts : array of event start times (seconds)
    ends   : array of event end times   (seconds)

    Corresponds to:
        [nas_event_starts, nas_event_ends] =
            find_no_intersection_events(NAS_times, total_duration, min_gap_duration)
    """
    if len(times) == 0:
        # No crossings at all — the whole signal is one event
        return np.array([0.0]), np.array([total_duration])

    # Include signal boundaries as virtual crossing times
    extended = np.concatenate([[0.0], np.sort(times), [total_duration]])
    gaps = np.diff(extended)

    mask   = gaps > min_gap
    starts = extended[:-1][mask]
    ends   = extended[1:][mask]
    return starts, ends


def merge_event_starts(
    *event_arrays: np.ndarray,
    merge_window_sec: float,
) -> np.ndarray:
    """Merge event start-time lists within merge_window_sec, keeping the earliest.

    Combines any number of event-start arrays, sorts them, then greedily
    keeps the first event in each cluster where all subsequent events fall
    within merge_window_sec of the last kept one.

    Corresponds to the MATLAB merge pattern used for both:
      - nasal + respiratory candidate events (20 s window)
      - HR + SpO2 + optional PAT/RR events   (20 s window)
    """
    if not event_arrays:
        return np.array([])

    all_events = np.sort(np.concatenate([a.ravel() for a in event_arrays]))
    if len(all_events) == 0:
        return np.array([])

    merged = [all_events[0]]
    for t in all_events[1:]:
        if t - merged[-1] > merge_window_sec:
            merged.append(t)
    return np.array(merged)
