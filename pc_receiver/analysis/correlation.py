"""Record-to-BP session pairing and correlation.

Matches an overnight Record session with the next morning BP session using
the timestamp encoded in the watch filenames:

  Record   20260511_220000_123.csv        (night, e.g. 22:00)
  BP       20260512_070000_456_bp.csv     (next morning, e.g. 07:00)

The pairing rule: a BP session recorded within [0, 18 h] after a Record
session ends is considered a candidate match.  Multiple candidates are
ranked by elapsed time; the closest is returned as the primary pair.

`compute_session_correlation` accepts pre-computed ApneaResult and
BpSessionResult objects and builds a flat dict of paired metrics suitable
for DataFrame construction or regression analysis.
"""

from __future__ import annotations

import re
from datetime import datetime, timedelta
from pathlib import Path
from typing import Optional

import numpy as np

from analysis.apnea_pipeline import ApneaResult
from analysis.bp_hrv_bpv import BpSessionResult


# ── Filename timestamp parsing ─────────────────────────────────────────────

_TS_PATTERN = re.compile(r"^(\d{8}_\d{6})")


def parse_session_datetime(filename: str) -> Optional[datetime]:
    """Extract the datetime from a watch filename (YYYYMMDD_HHMMSS prefix)."""
    m = _TS_PATTERN.match(Path(filename).name)
    if not m:
        return None
    try:
        return datetime.strptime(m.group(1), "%Y%m%d_%H%M%S")
    except ValueError:
        return None


# ── Session pairing ────────────────────────────────────────────────────────

def pair_sessions(
    record_paths: list[str | Path],
    bp_paths: list[str | Path],
    max_gap_hours: float = 18.0,
) -> list[dict]:
    """Pair each Record session with the closest subsequent BP session.

    Returns a list of dicts, one per Record session, with keys:
      record_path, bp_path, record_dt, bp_dt, gap_hours
    Unmatched Record sessions have bp_path = None.
    """
    pairs: list[dict] = []
    bp_with_dt = [
        (p, parse_session_datetime(Path(p).name))
        for p in bp_paths
    ]
    bp_with_dt = [(p, dt) for p, dt in bp_with_dt if dt is not None]

    for rec in record_paths:
        rec_dt = parse_session_datetime(Path(rec).name)
        if rec_dt is None:
            pairs.append({"record_path": str(rec), "bp_path": None,
                          "record_dt": None, "bp_dt": None, "gap_hours": None})
            continue

        candidates = [
            (str(p), bp_dt, (bp_dt - rec_dt).total_seconds() / 3600)
            for p, bp_dt in bp_with_dt
            if 0 <= (bp_dt - rec_dt).total_seconds() / 3600 <= max_gap_hours
        ]
        if candidates:
            best = min(candidates, key=lambda x: x[2])
            pairs.append({
                "record_path": str(rec),
                "bp_path":     best[0],
                "record_dt":   rec_dt,
                "bp_dt":       best[1],
                "gap_hours":   best[2],
            })
        else:
            pairs.append({"record_path": str(rec), "bp_path": None,
                          "record_dt": rec_dt, "bp_dt": None, "gap_hours": None})

    return pairs


# ── Metric pairing ─────────────────────────────────────────────────────────

def compute_session_correlation(
    apnea: ApneaResult,
    bp: BpSessionResult,
    gap_hours: float | None = None,
) -> dict:
    """Build a flat dict of paired metrics from one Record + BP session pair.

    The returned dict is designed to be a single row in a results DataFrame.
    Fields that could not be computed are stored as NaN.
    """
    return {
        # ---- identity
        "record_path":        apnea.session_path,
        "bp_path":            bp.session_path,
        "gap_hours":          gap_hours,
        # ---- record-session apnoea metrics
        "predicted_events":   apnea.ann_predicted,
        "verified_events":    len(apnea.verified_events),
        "duration_min":       apnea.duration_sec / 60,
        "tp":                 apnea.tp,
        "fp":                 apnea.fp,
        "fn":                 apnea.fn,
        "precision":          apnea.precision,
        "sensitivity":        apnea.sensitivity,
        "f1_score":           apnea.f1_score,
        # ---- bp-session HRV
        "rmssd_ms":           bp.rmssd_ms,
        "sdnn_ms":            bp.sdnn_ms,
        "pnn50_pct":          bp.pnn50_pct,
        "mean_hr_bpm":        bp.mean_hr_bpm,
        "beat_count":         bp.beat_count,
        # ---- bp-session PAT / BPV
        "pat_mean_ms":        bp.pat_mean_ms,
        "pat_sd_ms":          bp.pat_sd_ms,
        "pat_var_ms2":        bp.pat_var_ms2,
        # ---- quality
        "bp_quality_ok":      bp.quality_ok,
    }


def build_correlation_table(
    apnea_results: list[ApneaResult],
    bp_results: list[BpSessionResult],
    pairs: list[dict],
) -> list[dict]:
    """Build a paired metrics table across all matched sessions.

    `pairs` is the output of `pair_sessions`.  Unmatched sessions are skipped.
    """
    apnea_by_path = {a.session_path: a for a in apnea_results}
    bp_by_path    = {b.session_path: b for b in bp_results}
    rows = []
    for p in pairs:
        if p["bp_path"] is None:
            continue
        apnea = apnea_by_path.get(p["record_path"])
        bp    = bp_by_path.get(p["bp_path"])
        if apnea is None or bp is None:
            continue
        rows.append(compute_session_correlation(apnea, bp, gap_hours=p["gap_hours"]))
    return rows
