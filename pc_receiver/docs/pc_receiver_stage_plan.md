# PC Receiver and Stage-2 Analysis Plan for Claude

**Project:** tulog-biowatch PC companion  
**Date created:** 2026-05-11  
**Last updated:** 2026-05-12  
**Scope:** PC-side Python receiver now, staged analytics later.


## Current status

### Stage 1 — COMPLETE

`pc_receiver/` is built, tested, and running.

Files created:

```
pc_receiver/
├── receiver.py          ✅ Flask server, 0.0.0.0:8000, POST /upload, GET /ping
├── config.py            ✅ All paths and analysis constants
├── classifiers.py       ✅ Filename + CSV header classification
├── file_index.py        ✅ JSONL append-only metadata index
├── requirements.txt     ✅ Flask (Stage 1) + numpy/scipy/pandas/matplotlib (Stage 2)
├── .gitignore           ✅
├── .venv/               ✅ Virtual environment (not committed)
├── inbox/record/        ✅
├── inbox/bp/            ✅
├── inbox/unknown/       ✅
├── processed/           ✅
└── logs/                ✅
```

To run:
```bash
cd pc_receiver
source .venv/bin/activate
python receiver.py
```

Note: the receiver does **not** set up the Wi-Fi hotspot — that is a separate OS step
(Windows Mobile Hotspot or Linux NetworkManager/hostapd) done before starting the server.
The server binds to `0.0.0.0:8000` so it automatically listens on the hotspot interface.

### Stage 2 — SCAFFOLDED (not yet validated against real data)

All analysis modules exist and import cleanly under `pc_receiver/analysis/`:

```
analysis/
├── __init__.py              ✅
├── signal_processing.py     ✅ lp_filter, bp_filter, normalise, simple_dc,
│                               build_enhanced_baseline, shift_signal
├── ecg_processing.py        ✅ detect_r_peaks (Pan–Tompkins), calculate_heart_rate
├── intersection_detector.py ✅ find_intersections, find_no_intersection_events,
│                               merge_event_starts
├── physio_markers.py        ✅ detect_sao2_events, detect_hr_surge_events,
│                               detect_pat_variance_events, detect_rr_variance_events,
│                               merge_physio_events
├── apnea_pipeline.py        ✅ run_record_session_pipeline → ApneaResult dataclass
│                               (full 12-step MATLAB port; all intermediates preserved)
├── bp_hrv_bpv.py            ✅ run_bp_session_analysis → BpSessionResult
│                               (RMSSD, SDNN, pNN50, PAT mean/var, rolling PAT var)
├── correlation.py           ✅ pair_sessions (by filename timestamp),
│                               build_correlation_table
└── visualisation.py         ✅ plot_record_session (4-panel MATLAB mirror),
                                plot_bp_session, plot_batch_summary
```

### What still needs doing before Stage 2 is usable

1. **Validate `detect_r_peaks` against MATLAB `ECG_anno`** on a shared patient dataset.
   Log intermediate arrays (intersection indices, HR peaks, SpO2 events, merged events)
   and compare counts and timing with MATLAB output before trusting the Python results.

2. **Confirm `simple_dc` cutoff** matches the MATLAB helper behaviour.
   Current implementation uses a 0.05 Hz Butterworth high-pass; if MATLAB uses a moving
   average subtraction the results will differ slightly.

3. **Wire a CLI entry point** (`python -m analysis.apnea_pipeline <csv>`) once
   validation on the existing patient database is complete.

4. **Add a Dash or Streamlit wrapper** after the algorithm is validated — the backend
   already returns structured dataclasses that either frontend can consume directly.


## Goal

Create a PC-side companion program in Python that will eventually do two jobs:

### Stage 1 (DONE)

Receive files sent from the watch over Wi-Fi while the PC acts as a hotspot.

### Stage 2 (scaffolded — validation pending)

Analyse:

- **Record sessions** for sleep apnoea / sleep-study style indicators

- **BP sessions** for BPV / HRV

- correlation between overnight sleep-study outputs and next-morning BP recordings

The MATLAB pipeline (Apnea\_code\_all\_databases.m) has been ported to Python.
Validate against MATLAB outputs on the existing patient database before using on watch data.


## Recommended program structure

Create a small Python project with clear separation between:

1. **receiver** — HTTP server that saves files

2. **storage** — where files are stored and indexed

3. **classifier** — determine whether a file is a Record file or BP file from name and/or header

4. **analysis** — Stage 2 placeholder modules

5. **UI/CLI** — optional simple console logging now, richer interface later

Suggested layout:

```
pc\_receiver/  
├── receiver.py  
├── config.py  
├── file\_index.py  
├── classifiers.py  
├── analysis/  
│   ├── \_\_init\_\_.py  
│   ├── record\_sleep\_apnea.py  
│   ├── bp\_hrv\_bpv.py  
│   ├── correlation.py  
│   ├── signal\_processing.py  
│   ├── ecg\_processing.py  
│   ├── intersection\_detector.py  
│   ├── physio\_markers.py  
│   ├── event\_validator.py  
│   ├── apnea\_pipeline.py  
│   └── visualisation.py  
├── inbox/  
│   ├── record/  
│   ├── bp/  
│   └── unknown/  
├── processed/  
└── logs/
```

You can also keep it as a single-file Stage 1 server if preferred, but design comments should clearly indicate how to split it later.


## Stage 1 — receiver requirements

### 1. Server role

Run a simple HTTP server on the PC, listening on:

- `0.0.0.0`

- port `8000`

- endpoint `POST /upload`

The watch will upload a file as raw request body.

### 2. Expected headers

The receiver should accept and use:

- `X-Filename`

- `X-File-Size`

- `X-Session-Type`

If headers are missing:

- still save the file,

- infer type later,

- but log a warning.

### 3. Save behaviour

Save files into:

- `inbox/record/`

- `inbox/bp/`

- `inbox/unknown/`

Routing rule:

- if filename ends with `\_bp.csv` =\> BP

- if filename ends with `.csv` and not `\_bp.csv` =\> Record

- otherwise Unknown

If a duplicate filename arrives:

- do not overwrite silently.

- append a suffix like `\_dup1`, `\_dup2`, etc.

### 4. Logging

Log every upload:

- client IP

- timestamp

- filename

- bytes received

- destination path

- detected class

- success/failure

Write logs both to console and a text log file in `logs/`.

### 5. Response contract

On success:

- return HTTP 200

- body text: `OK`

On failure:

- return appropriate error code (400 or 500)

- body text with brief reason

### 6. Recommended Python stack

For Stage 1, prefer one of:

- **Flask** for fast implementation

- **BaseHTTPRequestHandler** for minimal dependency

Recommendation: use **Flask** if simplicity and readability are the priority.

Suggested Stage 1 Flask behaviour:

- one `/upload` route

- read `request.data`

- save bytes directly

- return JSON or plain text response


## Stage 2 — design and implementation direction

### 1. Record-session analysis target

The future Record-session pipeline should be able to process overnight/sleep-study style files and estimate sleep-apnoea-relevant outputs.

Expected long-term inputs from Record sessions may include, depending on firmware stage:

- ECG

- PPG / PAT / PTT-related timing

- SpO2 when MAX86140 is integrated

- respiration / airflow channels

- effort-band channels

- timestamps and drift

Stage 2 should eventually support:

- reading Record CSVs

- identifying session duration and channel availability

- extracting features for apnoea/hypopnoea detection

- integrating or porting MATLAB logic later

- producing summary outputs such as event markers, candidate AHI-like metrics, and nightly summary tables

### 2. BP-session analysis target

The future BP-session pipeline should analyse BP files for:

- HRV from RR interval series

- BPV-related surrogate measures from PAT / PTT-derived variability

- signal-quality metrics

- session summary metrics

Likely future outputs:

- RMSSD

- SDNN

- PAT mean / variance

- RR variance

- beat count

- valid duration

- optional beat-series exports

### 3. Correlation target

The long-term research goal is to correlate:

- prior overnight Record-derived sleep-apnoea outputs with

- next-morning BP recording metrics

Plan for a future correlator module that can:

- pair sessions by date/time

- match a Record night session to the next morning BP session

- compute paired metrics tables

- later support regression/correlation analyses once MATLAB logic is available


## Stage 2 Detailed — MATLAB-based Apnea Analysis

### Can Claude code this?

Yes. Claude should be able to port this MATLAB pipeline into Python, especially if the MATLAB helper functions (`ECG\_anno`, `BP\_filter`, `LP\_filter`, `normalise`, `simple\_dc`, `find\_intersections`, `find\_no\_intersection\_events`, `calculate\_heart\_rate`) are either supplied or described. The main work is engineering translation, not inventing a brand-new algorithm.

### Core MATLAB logic to preserve

The Python implementation should preserve these steps from the MATLAB method:

1. Load Record-session signals and optional visual scoring annotations.

2. Detect ECG beats and derive heart-rate signal.

3. Preprocess nasal and respiratory channels with low-pass / DC-removal / normalisation stages.

4. Build moving-window enhanced respiratory baselines.

5. Shift the low-passed baselines by 0.5 seconds.

6. Detect raw-vs-baseline intersections for nasal and composite respiratory signals.

7. Detect no-intersection periods longer than 10 seconds as candidate respiratory events.

8. Merge nasal and respiratory candidate events within a 20-second window.

9. Detect SpO2 desaturation events.

10. Detect HR surge events.

11. Merge physiological markers.

12. Verify events by requiring a prior merged respiratory event within the allowed backward window.

13. Compare predicted events with visual scoring and compute TP, FP, FN, TN, precision, sensitivity, specificity, and F1. – won’t be available outside of MATLAB stored databases; mirror developed apnea detction code in MATLAB to test on databases.

14. Export per-patient and batch summaries to Excel.

### Python module mapping

Recommended Stage 2 implementation split:

- `signal\_processing.py` — filtering, DC removal, normalisation, moving averages.

- `ecg\_processing.py` — beat detection and heart-rate derivation.

- `intersection\_detector.py` — signal intersection and no-intersection event logic.

- `physio\_markers.py` — SpO2, HR surge, PAT variance, RR variance events.

- `apnea\_pipeline.py` — end-to-end orchestration.

- `visualisation.py` — figures and dashboard plots.

### PAT and RR variance additions

Add PAT and RR variance as optional Stage 2 physiological markers.

#### PAT variance

For BP files containing `pat\_us`:

- compute rolling PAT variance over a configurable window, for example 20–30 seconds;

- estimate a baseline variance level;

- detect PAT variance spikes above a configurable multiplier threshold;

- merge these events into the physiological marker pool alongside HR and SpO2 events.

Potential value:

- sympathetic arousal after respiratory disturbance may shorten PAT and increase PAT variability;

- PAT variance may help flag events that are weak in airflow but stronger in cardiovascular response.

#### RR variance

For BP or Record data containing beat timing / RR-derived series:

- compute rolling RR variance or short-window HRV surrogate metrics;

- detect abrupt variance increases or rebound patterns;

- merge those events with HR and SpO2 markers before respiratory verification.

Potential value:

- respiratory disturbance termination can produce transient autonomic HR variability changes;

- RR variance can act as another corroborating marker.

### Suggested merge logic with added markers

Instead of merging only HR and SpO2 markers, Stage 2 should support a combined physiological event pool:

- HR surge events

- SpO2 desaturation events

- PAT variance spike events

- RR variance spike events

These can then be merged inside a configurable time window, such as 20 seconds, before the respiratory-backward verification step.

### Validation strategy

Before using the method on watch-generated files, validate the Python port against the MATLAB outputs on the same patient set.

Validation targets:

- matched event counts per patient;

- same TP/FP/FN behaviour within tolerable indexing differences;

- same precision / sensitivity / specificity / F1;

- same batch summary table structure.

If exact identity is not achieved initially, log intermediate arrays for:

- intersection indices,

- no-intersection event starts/ends,

- HR peaks,

- SpO2 event times,

- merged event times,

- verified event times.


## UI recommendation for Stage 2

Stage 2 can be built either as a polished dashboard or as a text-first research tool.

### Option A — polished professional dashboard

Recommended if the tool will be used interactively.

Suggested stack:

- **Plotly Dash** for a more application-like interface, or

- **Streamlit** for faster delivery with a still polished result.

Recommended layout:

- top controls for loading files, choosing patient/session, and setting thresholds;

- central signal plots mirroring the MATLAB 4-panel workflow;

- metrics cards for TP, FP, FN, precision, sensitivity, specificity, F1;

- table view for batch results;

- export buttons for Excel and figures.

This option can look **slick and professional** if Claude is told explicitly to build a polished local dashboard with consistent typography, spacing, themed plots, and clear interaction states.

### Option B — text / CLI first

Recommended if the first priority is algorithm correctness.

Features:

- command-line processing of one patient or batch cohort;

- saved PNG/PDF figures;

- Excel summary export;

- log output for debugging intermediate detections.

This option will be functional and efficient, but not visually polished unless a GUI layer is added later.

### Recommended sequence

Best path:

1. implement the analysis engine as a CLI/backend first;

2. verify against MATLAB patient outputs;

3. then wrap the validated backend in Dash or Streamlit for the polished UI.

That gives the safest research workflow.


## Recommended file metadata/indexing

Even in Stage 1, create a simple metadata index so Stage 2 becomes easier.

Suggested per-file metadata fields:

```
\{  
    "filename": "20260511\_071030\_456\_bp.csv",  
    "path": "inbox/bp/20260511\_071030\_456\_bp.csv",  
    "session\_type": "bp",  
    "received\_at": "2026-05-11T07:20:14",  
    "source\_ip": "192.168.137.23",  
    "declared\_size": 183422,  
    "actual\_size": 183422,  
    "header\_fields": \["time\_ms", "ecg", "ppg", "drift\_ms", "r\_peak\_ms", "rr\_us", "pat\_us"\]  
\}
```

Write this either:

- to a JSONL file, or

- as one small `.json` sidecar per upload.

Recommendation: **JSONL append log** is simplest.


## CSV classification rules

Implement both filename-based and header-based classification.

### Filename-based

- `\_bp.csv` =\> BP

- `.csv` =\> Record candidate

### Header-based

If needed, inspect first line:

- BP likely header contains: `time\_ms,ecg,ppg,drift\_ms,r\_peak\_ms,rr\_us,pat\_us`

- Record likely header contains Record-session channels such as ECG, PPG, respiration, nasal, and effort signals

If filename and header disagree:

- save the file anyway

- log a mismatch warning

- classify by header if header is decisive


## Testing plan for Stage 1

Claude should include a simple manual test method:

### Test 1 — local curl upload

```
curl -X POST http://127.0.0.1:8000/upload \\  
  -H "X-Filename: test\_bp.csv" \\  
  -H "X-File-Size: 1234" \\  
  -H "X-Session-Type: bp" \\  
  --data-binary @test\_bp.csv
```

Expected:

- server logs request

- file saved in `inbox/bp/`

- response `OK`

### Test 2 — duplicate filename

Upload the same file twice. Expected:

- both preserved

- second receives suffix

### Test 3 — missing headers

Send a POST without metadata headers. Expected:

- file still stored

- class inferred if possible

- warning logged


## Suggested Claude brief for the PC program

Use this as the implementation brief later:

```
Implement a Stage-1 PC companion receiver for tulog-biowatch.  
  
Goal:  
- run on a PC that also acts as the Wi-Fi hotspot  
- accept file uploads from the watch via HTTP POST on port 8000  
- save uploads into inbox/record, inbox/bp, or inbox/unknown  
- preserve filenames, avoiding overwrite by adding duplicate suffixes  
- log uploads to console and logs/upload\_log.txt  
- create a simple metadata index for future analysis  
  
Also prepare the project structure for Stage 2.  
  
Stage-2 requirements:  
- support Record-session sleep-apnoea analysis using the supplied MATLAB logic as the reference method  
- support BP-session HRV/BPV analysis  
- support Record-to-BP correlation by date/time  
- add PAT variance and RR variance as optional physiological markers in Stage 2  
- allow a later polished dashboard UI, but keep the backend analysis modular first  
  
Constraints:  
- keep Stage 1 simple and reliable  
- local hotspot use only  
- no auth or TLS yet  
- the Stage 2 backend should be written so it can power either a CLI tool or a polished Dash/Streamlit app later
```


## What Claude should not do yet

- Do not assume the MATLAB helper functions are identical to built-in Python functions without checking behaviour.

- Do not skip validation against MATLAB outputs.

- Do not overbuild the UI before the algorithm is validated.

- Do not add cloud sync or remote database features.

- Do not add unnecessary protocol complexity to Stage 1.

Keep Stage 1 focused on **reliable receipt and storage of watch files**, and Stage 2 focused on a **validated port of the MATLAB apnea-analysis pipeline with PAT/RR variance expansion**.

