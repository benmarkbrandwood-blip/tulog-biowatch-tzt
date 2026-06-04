"""Classify an uploaded file as 'bp', 'record', or 'unknown'.

Two strategies are combined:
  1. Filename suffix   (_bp.csv → bp, .csv → record)
  2. CSV header fields (decisive column names → bp or record)

If both agree, that class is used.
If header is decisive and contradicts the filename, header wins and the
caller receives enough information to log a mismatch warning.
"""

from __future__ import annotations

# Filename suffixes
_BP_SUFFIX     = "_bp.csv"
_RECORD_SUFFIX = ".csv"

# BP files are expected to contain these columns
_BP_REQUIRED_COLS = {"time_ms", "ecg", "ppg", "rr_us"}

# Record files should contain ECG/PPG plus at least one respiratory channel
_RECORD_ECG_PPG  = {"ecg", "ppg"}
_RECORD_RESP_ANY = {"nasal", "naf", "sao2", "resp", "vth", "vab", "airflow", "effort"}


def classify_by_filename(name: str) -> str:
    lower = name.lower()
    if lower.endswith(_BP_SUFFIX):
        return "bp"
    if lower.endswith(_RECORD_SUFFIX):
        return "record"
    return "unknown"


def _parse_header_fields(data: bytes) -> list[str]:
    try:
        first_line = data.split(b"\n")[0].decode("utf-8", errors="replace").strip()
        return [f.strip().lower() for f in first_line.split(",") if f.strip()]
    except Exception:
        return []


def classify_by_header(data: bytes) -> tuple[str, list[str]]:
    """Return (class, field_list). class is 'bp', 'record', or 'unknown'."""
    fields = _parse_header_fields(data)
    field_set = set(fields)

    if field_set >= _BP_REQUIRED_COLS:
        return "bp", fields

    if field_set >= _RECORD_ECG_PPG and field_set & _RECORD_RESP_ANY:
        return "record", fields

    return "unknown", fields


def classify(name: str, data: bytes) -> tuple[str, list[str]]:
    """Return (session_class, header_fields).

    Precedence:
      - Decisive header class beats a filename-only classification.
      - Filename class used when header is undecisive.
      - 'unknown' only if both strategies fail.
    The caller is responsible for warning on filename/header mismatch.
    """
    name_class   = classify_by_filename(name)
    hdr_class, hdr_fields = classify_by_header(data)

    if hdr_class != "unknown":
        # Header is decisive — use it regardless of filename
        return hdr_class, hdr_fields

    # Header undecisive — fall back to filename
    return name_class, hdr_fields
