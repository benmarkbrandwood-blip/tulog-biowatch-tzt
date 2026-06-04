"""ECG beat detection and heart-rate signal derivation.

Python port of the MATLAB helpers:
  ECG_anno(ECG, fs, plot_flag)            → detect_r_peaks(ecg, fs)
  calculate_heart_rate(locs, fs, length)  → calculate_heart_rate(locs, fs, n)

ECG_anno in the MATLAB codebase is an R-peak detector (Pan–Tompkins style).
This implementation uses scipy.signal with band-pass + differentiation +
squaring + moving-window integration, then find_peaks for robust detection.
Validate outputs against MATLAB ECG_anno before use on clinical data.
"""

from __future__ import annotations

import numpy as np
from scipy import signal as _sig
from scipy.interpolate import interp1d

from analysis.signal_processing import bp_filter


def detect_r_peaks(ecg: np.ndarray, fs: float) -> tuple[np.ndarray, np.ndarray]:
    """Detect R-peak sample indices using a Pan–Tompkins-inspired pipeline.

    Returns
    -------
    locs   : 1-D int array of R-peak sample indices
    peaks  : 1-D float array of ECG amplitude at those samples

    Note: The original MATLAB ECG_anno may differ in filter design or
    threshold logic.  Cross-validate on a shared data set before replacing.
    """
    filtered   = bp_filter(fs, ecg, low_hz=1.0, high_hz=25.0)
    derivative = np.diff(filtered, prepend=filtered[0])
    squared    = derivative ** 2

    # Moving-window integration: ~150 ms window
    win = max(1, round(0.15 * fs))
    integrated = np.convolve(squared, np.ones(win) / win, mode="same")

    # Minimum inter-peak distance: 200 ms (300 bpm max)
    min_dist = round(0.20 * fs)
    threshold = 0.3 * np.max(integrated)
    locs_raw, _ = _sig.find_peaks(integrated, height=threshold, distance=min_dist)

    # Refine: find the true maximum in ECG within ±50 ms of each detected index
    half = round(0.05 * fs)
    locs_refined = []
    for idx in locs_raw:
        lo = max(0, idx - half)
        hi = min(len(ecg) - 1, idx + half)
        best = lo + int(np.argmax(np.abs(ecg[lo : hi + 1])))
        locs_refined.append(best)

    locs = np.array(locs_refined, dtype=int)
    peaks = ecg[locs] if len(locs) else np.array([])
    return locs, peaks


def calculate_heart_rate(
    locs: np.ndarray, fs: float, n_samples: int
) -> np.ndarray:
    """Convert R-peak indices to a continuous heart-rate signal (BPM) at fs.

    The instantaneous HR at each beat is 60 / RR_interval_s.  Values are
    linearly interpolated to produce a signal of length n_samples, matching
    the original ECG vector.  Corresponds to the MATLAB calculate_heart_rate
    helper used in the apnoea pipeline.
    """
    if len(locs) < 2:
        return np.full(n_samples, np.nan)

    rr_samples = np.diff(locs)
    rr_sec     = rr_samples / fs
    hr_bpm     = 60.0 / rr_sec

    # Times of the HR estimates (midpoint between consecutive beats)
    hr_times = (locs[:-1] + locs[1:]) / 2.0

    # Extrapolate to cover [0, n_samples-1] at integer sample indices
    t_out = np.arange(n_samples, dtype=float)
    interp = interp1d(
        hr_times, hr_bpm,
        kind="linear",
        bounds_error=False,
        fill_value=(hr_bpm[0], hr_bpm[-1]),
    )
    return interp(t_out)
