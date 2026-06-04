"""Append-only JSONL metadata index for every received file.

Each line is one JSON object with the fields documented in pc_receiver_stage_plan.md.
`read_all()` lets Stage 2 analysis discover and filter sessions without walking the inbox.
"""

from __future__ import annotations

import json
from pathlib import Path

from config import FILE_INDEX


def append_entry(entry: dict) -> None:
    FILE_INDEX.parent.mkdir(parents=True, exist_ok=True)
    with open(FILE_INDEX, "a", encoding="utf-8") as fh:
        fh.write(json.dumps(entry, ensure_ascii=False) + "\n")


def read_all() -> list[dict]:
    if not FILE_INDEX.exists():
        return []
    entries: list[dict] = []
    with open(FILE_INDEX, encoding="utf-8") as fh:
        for line in fh:
            line = line.strip()
            if line:
                try:
                    entries.append(json.loads(line))
                except json.JSONDecodeError:
                    pass
    return entries


def sessions_of_type(session_type: str) -> list[dict]:
    return [e for e in read_all() if e.get("session_type") == session_type]
