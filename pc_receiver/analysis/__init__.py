"""Stage 2 analysis package for tulog-biowatch.

Module layout (mirrors the MATLAB pipeline structure):

  signal_processing   — filtering, DC removal, normalisation, moving averages
  ecg_processing      — R-peak detection, heart-rate signal derivation
  intersection_detector — respiratory signal crossing and no-intersection events
  physio_markers      — SpO2 / HR / PAT variance / RR variance event detection
  apnea_pipeline      — end-to-end Record-session apnoea analysis orchestrator
  bp_hrv_bpv          — BP-session HRV and BPV analysis
  correlation         — Record-to-BP session pairing and correlation
  visualisation       — matplotlib / Plotly-ready figure builders

All public functions return structured Python objects (dataclasses or dicts) so
they can be consumed equally by a CLI tool or a Dash/Streamlit dashboard.
"""
