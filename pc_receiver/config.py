"""Central configuration for the tulog-biowatch PC companion."""

from pathlib import Path

BASE_DIR = Path(__file__).parent

# ── Inbox / storage paths ──────────────────────────────────────────────────
INBOX_RECORD  = BASE_DIR / "inbox" / "record"
INBOX_BP      = BASE_DIR / "inbox" / "bp"
INBOX_UNKNOWN = BASE_DIR / "inbox" / "unknown"
PROCESSED_DIR = BASE_DIR / "processed"
LOGS_DIR      = BASE_DIR / "logs"
UPLOAD_LOG    = LOGS_DIR / "upload_log.txt"
FILE_INDEX    = LOGS_DIR / "file_index.jsonl"

# ── Stage 1 server ─────────────────────────────────────────────────────────
HOST = "0.0.0.0"
PORT = 8000

# ── Stage 2 — apnea pipeline defaults ─────────────────────────────────────
APNEA_FS                    = 200     # expected sampling rate, Hz
APNEA_WINDOW_SEC            = 20      # moving baseline window, s
APNEA_MIN_GAP_SEC           = 10      # minimum no-intersection gap to count as event, s
APNEA_RESP_MERGE_SEC        = 20      # merge window for nasal + respiratory candidates, s
APNEA_PHYSIO_MERGE_SEC      = 20      # merge window for HR/SpO2/PAT/RR events, s
APNEA_VERIFY_BACKWARD_SEC   = 50      # max look-back for respiratory verification, s
APNEA_BASELINE_SHIFT_SEC    = 0.5     # phase shift applied to moving baseline, s
APNEA_O2_THRESH             = 2.0     # SpO2 drop threshold, % below last peak
APNEA_O2_MIN_DURATION_SEC   = 3.0     # minimum SpO2 event duration, s
APNEA_O2_MIN_PEAK_DIST_SEC  = 10.0    # minimum distance between SpO2 peaks, s
APNEA_O2_MIN_PEAK_PROM      = 0.2     # minimum SpO2 peak prominence, %
APNEA_HR_THRESH_FACTOR      = 2.7     # HR surge threshold factor * mean(|hr_dc|)
APNEA_HR_MIN_PEAK_DIST_SEC  = 30.0    # minimum distance between HR surge peaks, s
APNEA_WINDOW_40_SEC         = 40      # window size for TN calculation, s

# ── Stage 2 — PAT / RR variance defaults ──────────────────────────────────
PAT_VARIANCE_WINDOW_SEC  = 20   # rolling variance window for PAT, s
PAT_VARIANCE_MULTIPLIER  = 3.0  # spike threshold = multiplier * baseline variance
RR_VARIANCE_WINDOW_SEC   = 20   # rolling variance window for RR intervals, s
RR_VARIANCE_MULTIPLIER   = 3.0

# ── Stage 2 — BP / HRV defaults ───────────────────────────────────────────
HRV_NN50_THRESHOLD_MS = 50   # threshold for pNN50 calculation, ms
