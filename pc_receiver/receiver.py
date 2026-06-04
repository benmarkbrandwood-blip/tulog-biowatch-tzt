"""Stage 1 — tulog-biowatch PC companion receiver.

Listens on 0.0.0.0:8000 for HTTP POST /upload from the watch.
Saves files into inbox/record, inbox/bp, or inbox/unknown.
Logs every upload to console and logs/upload_log.txt.
Appends a metadata entry to logs/file_index.jsonl.

Run:
    python receiver.py

Test with curl:
    curl -X POST http://127.0.0.1:8000/upload \\
      -H "X-Filename: 20260511_071030_456_bp.csv" \\
      -H "X-File-Size: 1234" \\
      -H "X-Session-Type: bp" \\
      --data-binary @my_file.csv
"""

import logging
import sys
from datetime import datetime, timezone
from pathlib import Path

from flask import Flask, request

import config
from classifiers import classify, classify_by_filename
from file_index import append_entry

app = Flask(__name__)
log = logging.getLogger(__name__)

# ── Logging setup ──────────────────────────────────────────────────────────

def _setup_logging() -> None:
    config.LOGS_DIR.mkdir(parents=True, exist_ok=True)
    fmt     = "%(asctime)s  %(levelname)-8s  %(message)s"
    datefmt = "%Y-%m-%d %H:%M:%S"
    logging.basicConfig(
        level=logging.INFO,
        format=fmt,
        datefmt=datefmt,
        handlers=[
            logging.StreamHandler(sys.stdout),
            logging.FileHandler(config.UPLOAD_LOG, encoding="utf-8"),
        ],
        force=True,
    )

# ── Helpers ────────────────────────────────────────────────────────────────

def _unique_path(folder: Path, filename: str) -> Path:
    """Return a non-colliding path, appending _dup1, _dup2 … if needed."""
    dest = folder / filename
    if not dest.exists():
        return dest
    stem   = Path(filename).stem
    suffix = Path(filename).suffix
    i = 1
    while True:
        candidate = folder / f"{stem}_dup{i}{suffix}"
        if not candidate.exists():
            return candidate
        i += 1

# ── Upload endpoint ────────────────────────────────────────────────────────

@app.route("/upload", methods=["POST"])
def upload():
    received_at = datetime.now(timezone.utc).isoformat()
    client_ip   = request.remote_addr

    filename         = request.headers.get("X-Filename",     "").strip()
    declared_size_s  = request.headers.get("X-File-Size",    "").strip()
    session_type_hdr = request.headers.get("X-Session-Type", "").strip()

    # Warn on missing headers but still process the file
    missing = [h for h, v in (
        ("X-Filename", filename),
        ("X-File-Size", declared_size_s),
        ("X-Session-Type", session_type_hdr),
    ) if not v]
    if missing:
        log.warning("ip=%s  missing headers: %s", client_ip, ", ".join(missing))

    if not filename:
        filename = f"upload_{datetime.utcnow().strftime('%Y%m%d_%H%M%S_%f')}.bin"

    data        = request.get_data()
    actual_size = len(data)

    if actual_size == 0:
        log.error("ip=%s  empty body (filename hint: %s)", client_ip, filename)
        return "empty body", 400

    session_class, header_fields = classify(filename, data)

    # Log a mismatch if filename and header disagree
    name_class = classify_by_filename(filename)
    if name_class != "unknown" and session_class != name_class:
        log.warning(
            "ip=%s  class mismatch — filename='%s' but header='%s' for %s — using header",
            client_ip, name_class, session_class, filename,
        )

    folder_map = {
        "bp":      config.INBOX_BP,
        "record":  config.INBOX_RECORD,
        "unknown": config.INBOX_UNKNOWN,
    }
    folder = folder_map[session_class]
    folder.mkdir(parents=True, exist_ok=True)

    dest = _unique_path(folder, filename)
    try:
        dest.write_bytes(data)
    except OSError as exc:
        log.error("ip=%s  save failed for '%s': %s", client_ip, filename, exc)
        return f"save failed: {exc}", 500

    try:
        declared_size: int | None = int(declared_size_s)
    except (ValueError, TypeError):
        declared_size = None

    log.info(
        "RECV  ip=%-15s  file=%-44s  bytes=%-8d  class=%s",
        client_ip, dest.name, actual_size, session_class,
    )

    append_entry({
        "filename":      dest.name,
        "path":          str(dest.relative_to(config.BASE_DIR)),
        "session_type":  session_class,
        "received_at":   received_at,
        "source_ip":     client_ip,
        "declared_size": declared_size,
        "actual_size":   actual_size,
        "header_fields": header_fields,
    })

    return "OK", 200

# ── Health check ───────────────────────────────────────────────────────────

@app.route("/ping", methods=["GET"])
def ping():
    return "tulog-biowatch receiver OK", 200

# ── Entry point ────────────────────────────────────────────────────────────

if __name__ == "__main__":
    _setup_logging()
    for d in (
        config.INBOX_RECORD, config.INBOX_BP, config.INBOX_UNKNOWN,
        config.PROCESSED_DIR, config.LOGS_DIR,
    ):
        d.mkdir(parents=True, exist_ok=True)

    log.info("tulog-biowatch receiver  —  listening on %s:%d", config.HOST, config.PORT)
    log.info("Inbox:  %s", config.BASE_DIR / "inbox")
    log.info("Index:  %s", config.FILE_INDEX)
    log.info("Log:    %s", config.UPLOAD_LOG)

    app.run(host=config.HOST, port=config.PORT, debug=False)
