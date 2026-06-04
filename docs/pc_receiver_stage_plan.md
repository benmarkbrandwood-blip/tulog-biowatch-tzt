# PC Receiver and Stage-2 Analysis Plan for Claude

**Project:** tulog-biowatch PC companion  
**Date:** 2026-05-11 (updated 2026-05-13)  
**Scope:** PC-side Python receiver now, staged analytics later.


## Goal

Create a PC-side companion program in Python that will eventually do two jobs:

### Stage 1 (implement now)

Receive files sent from the watch over Wi-Fi while the PC acts as a hotspot.

### Stage 2 (plan only for now)

Analyse:

- **Record sessions** for sleep apnoea / sleep-study style indicators

- **BP sessions** for BPV / HRV

- correlation between overnight sleep-study outputs and next-morning BP recordings

For now, Stage 1 is enough to build. Stage 2 should be planned in the design so that it can be added later, likely using existing MATLAB logic as the reference basis.


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


---

## Stage 1b — Linux host support

### Background and problem statement

The original Stage 1 plan assumed a Windows PC acting as the hotspot. Two platform differences break this on Linux:

1. **Gateway IP**: Windows Mobile Hotspot assigns the PC gateway `192.168.137.1`. Linux Network Manager hotspot assigns `10.42.0.1` (its default subnet for shared connections on `wlp*` interfaces). The watch firmware already tries both addresses in sequence (fixed 2026-05-13), so this is resolved on the firmware side.

2. **Firewall**: Linux distributions (Ubuntu, Pop!\_OS, etc.) run UFW/nftables with a default deny-incoming policy. When the hotspot is active, inbound TCP connections from the hotspot subnet to port 8000 on the host are silently dropped before Flask ever sees them. The receiver appears to start correctly but the watch times out after 15 s with `ESP_ERR_HTTP_CONNECT`. This is the primary remaining blocker on Linux.

3. **Lack of startup guidance**: The receiver prints no information about which IP the watch will route to, making debugging unnecessarily hard when the setup is wrong.

### Implementation plan

#### New file: `platform_info.py`

A utility module that is called once at receiver startup. Responsibilities:

- Detect the host OS using `platform.system()` — returns `'Windows'` or `'Linux'`.
- Scan active network interfaces (via `socket` + `fcntl` / `subprocess ip addr`) to find a hotspot-range IP:
  - Look for an interface with an address in `10.42.0.0/24` (Linux NM default).
  - Look for an interface with an address in `192.168.137.0/24` (Windows Mobile Hotspot).
  - Fall back to printing all non-loopback IPs if neither known subnet is found.
- On Linux: check UFW status by running `ufw status` with `subprocess`. Determine whether an ALLOW rule exists for port 8000 from the hotspot subnet.
- Return a simple dataclass (or plain dict):

```python
@dataclass
class PlatformInfo:
    os_name: str           # 'Windows' | 'Linux' | 'Darwin' | other
    hotspot_ip: str | None # e.g. '10.42.0.1' or '192.168.137.1', or None
    hotspot_subnet: str | None  # e.g. '10.42.0.0/24'
    firewall_blocking: bool     # True if UFW active and no rule for port 8000
    firewall_fix_cmd: str | None  # the exact command to run to fix it, or None
```

The module must **never** modify firewall state itself. It only reads and reports.

#### Changes to `receiver.py` startup

After `_setup_logging()`, call `platform_info.detect()` and log:

- The detected OS.
- The hotspot interface IP (if found) — logged as `"Watch should connect to: {ip}:{port}"`.
- If hotspot IP is not found: a warning that the hotspot may not be active yet, with instructions to run `start-linux.sh` (Linux) or start the Mobile Hotspot (Windows).
- On Linux: if `firewall_blocking` is True, log a clearly visible `ERROR`-level message with the exact `ufw` command to fix it, then exit with a non-zero code so the problem is not silently ignored. Example:

```
ERROR  Firewall is blocking port 8000 from the hotspot subnet.
ERROR  Fix:  sudo ufw allow from 10.42.0.0/24 to any port 8000
ERROR  Or run start-linux.sh which handles this automatically.
ERROR  Receiver will NOT be reachable by the watch until this is fixed.
```

Whether to hard-exit or just warn is a judgement call. Recommend a warning (not exit) so the user can still test with curl from localhost while they fix the firewall.

#### New file: `start-linux.sh`

A one-stop Linux startup script. The user runs this instead of `python receiver.py` directly. Steps:

1. Bring up the NM hotspot (idempotent — does nothing if already active):
   ```bash
   sudo nmcli device wifi hotspot \
     ifname wlp62s0 \
     con-name TulogHotspot \
     ssid TulogHotspot \
     password "tulogpass"
   ```
   The interface name (`wlp62s0`) should be configurable at the top of the script as a variable, or auto-detected with `nmcli device status | awk '/wifi/ {print $1; exit}'`.

2. Open the firewall rule for this session:
   ```bash
   sudo ufw allow from 10.42.0.0/24 to any port 8000 comment "tulog-biowatch receiver"
   ```

3. Start the receiver:
   ```bash
   python receiver.py
   ```

4. On exit (trap `EXIT` / `INT` / `TERM`): remove the UFW rule added in step 2 so it does not persist after the session:
   ```bash
   sudo ufw delete allow from 10.42.0.0/24 to any port 8000
   ```
   This is optional but keeps the system clean. If the user prefers a persistent rule, they can comment out the trap.

The script should print a short banner explaining what it is doing at each step.

#### New file: `start-windows.bat` (or `start-windows.ps1`)

Windows equivalent. Much simpler — no firewall setup needed because Windows Mobile Hotspot does not block inbound connections by default.

```bat
@echo off
echo tulog-biowatch PC receiver
echo Make sure Windows Mobile Hotspot is active before proceeding.
python receiver.py
```

A PowerShell variant could optionally check that the hotspot adapter is up before starting.

#### Changes to `config.py`

Add the two known hotspot gateway constants so they are available to both `platform_info.py` and any future NVS-config tooling:

```python
# Known hotspot gateway IPs (one will be the watch's default route)
WINDOWS_HOTSPOT_GATEWAY = "192.168.137.1"
LINUX_NM_HOTSPOT_GATEWAY = "10.42.0.1"

WINDOWS_HOTSPOT_SUBNET  = "192.168.137.0/24"
LINUX_NM_HOTSPOT_SUBNET = "10.42.0.0/24"
```

### Updated project layout

```
pc_receiver/
├── receiver.py          (modified — call platform_info at startup)
├── config.py            (modified — add gateway/subnet constants)
├── platform_info.py     (new — OS detection, hotspot IP, firewall check)
├── file_index.py
├── classifiers.py
├── start-linux.sh       (new — hotspot + firewall + launch)
├── start-windows.bat    (new — simple Windows launch wrapper)
├── requirements.txt
├── analysis/
├── inbox/
├── processed/
└── logs/
```

### What this does NOT do

- Does not auto-configure the firewall from inside Python (would require sudo / elevated process).
- Does not set up the hotspot from inside Python.
- Does not support macOS (not a target platform).
- Does not add TLS, auth, or remote access — still local hotspot only.
- Does not change the receiver's upload logic, classification, or indexing.

### Implementation order

1. Add gateway/subnet constants to `config.py`.
2. Implement `platform_info.py` (detect → return dataclass).
3. Update `receiver.py` to call `platform_info.detect()` and log the result at startup.
4. Write `start-linux.sh`.
5. Write `start-windows.bat`.
6. Test on Linux with hotspot active and UFW enabled to confirm the warning is shown correctly.
7. Test with `start-linux.sh` to confirm end-to-end send from watch works.

### Acceptance criteria

- Running `python receiver.py` on Linux with UFW blocking port 8000 prints a visible error and the exact fix command.
- Running `start-linux.sh` on Linux results in a successful file transfer from the watch with no manual firewall steps.
- Running `python receiver.py` on Windows is unchanged in behaviour.
- The receiver startup banner clearly shows which IP the watch should reach (e.g. `Watch should connect to: 10.42.0.1:8000`).


---

## Known issue — Linux end-to-end transfer not yet confirmed (2026-05-13)

### Status: unresolved, deferred. Using Windows for now.

### What was investigated

- Flask receiver correctly binds to `0.0.0.0:8000` and responds to `curl http://10.42.0.1:8000/ping` from the PC itself.
- NM hotspot is up on `wlp62s0` at `10.42.0.1`; watch connects and gets `10.42.0.21` with gateway `10.42.0.1`.
- UFW is **disabled** (`ENABLED=no` in `/etc/ufw/ufw.conf`).
- No iptables tables are loaded (`/proc/net/ip_tables_names` empty).
- nftables config (`/etc/nftables.conf`) has empty `input`/`forward`/`output` chains with no default drop policy.
- `start-linux.sh` was implemented to handle hotspot creation and firewall rules.

### What still fails

The watch reports `ESP_ERR_HTTP_CONNECT` / `select() timeout` when attempting to POST to `10.42.0.1:8000`, despite the receiver being reachable from the PC itself. Whether any TCP SYN reaches the receiver during a live watch send attempt has not been confirmed (receiver terminal output not captured during the failing attempt).

### Likely remaining causes to investigate

1. **NM hotspot adds dynamic iptables/nftables rules at runtime** that are not visible in config files. These would not show in `/etc/nftables.conf` but would be active in the kernel. Needs `sudo nft list ruleset` or `sudo iptables -L -n` run while the hotspot is active.
2. **Watch is connecting to a different saved network** (not TulogHotspot) so the route to `10.42.0.1` doesn't exist for that session.
3. **Receiver is not running when the watch sends** — timing issue between starting receiver and pressing Send on watch.

### Next diagnostic steps (when returning to Linux)

1. Start hotspot and receiver, then run `sudo nft list ruleset` and `sudo iptables -L INPUT -n -v` to see live firewall state.
2. From a second device (phone/tablet) connected to TulogHotspot, run `curl http://10.42.0.1:8000/ping` — this confirms the receiver is reachable from outside the PC, not just from localhost.
3. Capture the receiver terminal output at the exact moment the watch presses Send — if no request arrives, the packet is being dropped before Flask; if a request arrives but returns an error, the issue is in the application layer.

