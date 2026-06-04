"""Signal-processing primitives — Python port of the MATLAB helper functions.

Ported from the reference MATLAB pipeline (Apnea_code_all_databases.m):
  LP_filter   → lp_filter
  BP_filter   → bp_filter
  normalise   → normalise
  simple_dc   → simple_dc
  movmean(abs(...)) → moving_mean_abs

All functions operate on 1-D numpy arrays and return numpy arrays of the
same length.  Validate against MATLAB outputs before using in production.
"""

from __future__ import annotations

import numpy as np
from scipy import signal as _sig


def lp_filter(fs: float, x: np.ndarray, cutoff_hz: float) -> np.ndarray:
    """4th-order Butterworth low-pass filter."""
    nyq = fs / 2.0
    b, a = _sig.butter(4, cutoff_hz / nyq, btype="low")
    return _sig.filtfilt(b, a, x).astype(float)


def bp_filter(fs: float, x: np.ndarray, low_hz: float, high_hz: float) -> np.ndarray:
    """4th-order Butterworth band-pass filter."""
    nyq = fs / 2.0
    b, a = _sig.butter(4, [low_hz / nyq, high_hz / nyq], btype="band")
    return _sig.filtfilt(b, a, x).astype(float)


def normalise(x: np.ndarray) -> np.ndarray:
    """Zero-mean, unit-std normalisation (z-score).

    Centres the signal around zero so that movmean(abs(x)) reflects the
    typical amplitude envelope, matching the MATLAB pipeline assumption.
    """
    std = np.std(x)
    if std == 0:
        return x - np.mean(x)
    return (x - np.mean(x)) / std


def simple_dc(fs: float, x: np.ndarray) -> np.ndarray:
    """Remove DC component and very slow drift via a 0.05 Hz high-pass filter.

    Equivalent to the MATLAB simple_dc helper which strips the low-frequency
    baseline from signals such as ECG, respiratory effort, and heart rate
    before threshold-based event detection.
    """
    nyq = fs / 2.0
    cutoff = min(0.05 / nyq, 0.99)  # guard against cutoff ≥ Nyquist
    b, a = _sig.butter(2, cutoff, btype="high")
    return _sig.filtfilt(b, a, x).astype(float)


def moving_mean_abs(x: np.ndarray, window_samples: int) -> np.ndarray:
    """Causal moving mean of |x| with edge shrinkage (mirrors MATLAB 'shrink').

    Equivalent to MATLAB:  movmean(abs(x), window_samples, 'Endpoints', 'shrink')
    """
    abs_x = np.abs(x)
    # Use uniform_filter1d with reflect padding then trim — simpler: convolve
    kernel = np.ones(window_samples) / window_samples
    # 'same' mode + manual edge correction to match MATLAB 'shrink'
    pad = window_samples // 2
    padded = np.pad(abs_x, (pad, pad), mode="edge")
    smoothed = np.convolve(padded, kernel, mode="valid")
    # Trim to original length
    return smoothed[: len(x)]


def build_enhanced_baseline(
    raw: np.ndarray, window_sec: float, fs: float, weight: float = 1.0
) -> np.ndarray:
    """Compute raw + weight * movmean(|raw|, window).

    Corresponds to the MATLAB lines:
        NAS_LP = NAS_raw + movmean(abs(NAS_raw), window_samples, 'Endpoints', 'shrink')
        Resp_all_LP = Resp_all + 0.5 * movmean(abs(Resp_all), ...)
    """
    window_samples = round(window_sec * fs)
    return raw + weight * moving_mean_abs(raw, window_samples)


def shift_signal(x: np.ndarray, shift_sec: float, fs: float) -> np.ndarray:
    """Delay x by shift_sec seconds (prepend zeros, drop tail).

    Corresponds to the MATLAB phase-shift step:
        shifted = [zeros(shift_samples, 1); signal(1:end-shift_samples)]
    """
    n = round(shift_sec * fs)
    if n <= 0:
        return x.copy()
    return np.concatenate([np.zeros(n), x[:-n]])
