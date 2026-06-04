# Expanded Apnea Detection Method

This document describes an expanded apnea-detection method based on respiratory airflow, respiratory effort, autonomic markers, and blood-pressure-related timing markers. The method extends the current logic by adding **pulse arrival time (PAT) fall** as an additional post-event physiological marker and by using the relationship between **nasal respiration** and **respiratory effort** to classify events as **central**, **obstructive**, or **mixed** apnea. The classification principles are consistent with standard sleep scoring concepts, where obstructive apnea has persistent respiratory effort, central apnea has absent inspiratory effort, and mixed apnea transitions from absent to present effort during the same event. \[web:83\]\[web:85\]\[web:86\]\[web:89\]\[web:90\]\[web:92\]\[web:97\]

\[image:1\]

## Overview

In the original method, an event only counts as an apnea event when a respiratory disturbance is followed by autonomic evidence such as heart-rate surge and/or oxygen desaturation. The expanded method keeps that structure, but adds a **PAT fall** marker to capture the blood-pressure and sympathetic response that often follows a respiratory event or arousal. PAT is a useful cardiovascular marker because sleep-breathing events can trigger transient sympathetic activation and nocturnal blood-pressure surges, and PAT/PTT-derived measures can reflect these changes. \[web:88\]\[web:91\]\[web:93\]\[web:97\]

The expanded algorithm therefore has four layers:

- respiratory airflow disturbance detection,

- respiratory effort disturbance detection,

- physiological confirmation using HR, SpO2, and PAT,

- event-type classification using the airflow-versus-effort pattern. \[web:83\]\[web:89\]\[web:92\]\[web:97\]

## Signals used

The method assumes the following signals are available:

- **Nasal respiration / airflow** signal, used as the main airflow channel.

- **Thoracic and abdominal respiratory effort** signals, combined into a composite effort signal.

- **ECG**, used to derive heart rate and beat timing.

- **SpO2**, used for desaturation detection.

- **PAT**, derived from ECG-to-PPG timing or another pulse-arrival metric, used as a surrogate for transient blood-pressure/autonomic change. \[web:86\]\[web:92\]\[web:97\]

A future implementation may also compute RR variance and PAT variance, but the present method specifically adds **PAT fall** as a direct event-confirmation marker. PAT and pulse transit metrics have been used as non-invasive correlates of autonomic and vascular responses in sleep-disordered breathing. \[web:88\]\[web:93\]\[web:97\]

## Step 1: Detect airflow and effort candidate events

The nasal airflow signal and the composite respiratory effort signal are each processed with the same general logic:

1. filter and normalise the raw signal,

2. generate a slow moving baseline or moving-envelope version,

3. shift the smoothed baseline in time,

4. detect intersections between the raw signal and shifted baseline,

5. detect **no-intersection intervals** lasting at least 10 seconds. \[web:92\]

The physiological interpretation is that a prolonged no-intersection interval indicates a sustained change in amplitude or morphology, such as markedly reduced airflow or reduced respiratory effort. In the current logic, this serves as a candidate respiratory event detector rather than a final apnea decision. \[web:89\]\[web:92\]

This should be done separately for:

- **nasal airflow** → `nas\_event\_starts`, `nas\_event\_ends`

- **respiratory effort composite** → `resp\_event\_starts`, `resp\_event\_ends`

## Step 2: Create distinct airflow and effort event streams

The original method merged nasal and effort events early. For classification of central, obstructive, and mixed apnea, the two streams should remain distinct long enough to compare them.

Two event streams should therefore be retained:

- **airflow candidate events** from the nasal signal,

- **effort candidate events** from the thoracic + abdominal composite. \[web:83\]\[web:86\]\[web:89\]\[web:90\]

These streams can still be merged for the purpose of identifying a broad respiratory disturbance window, but the algorithm should preserve whether airflow loss occurred, whether effort loss occurred, and whether effort returned during the event.

\[image:2\]

## Step 3: Add physiological confirmation markers

A respiratory event becomes much more convincing when it is followed by physiological consequences. The expanded method should use three post-event physiological markers:

### 3.1 HR surge

Use ECG-derived heart rate and detect large positive HR excursions relative to baseline. In the current code this is done with a peak-prominence threshold based on the DC-removed heart-rate signal. This represents the post-event autonomic arousal or rebound response. (2.7 times the mean HR) \[web:95\]\[web:97\]

### 3.2 SpO2 desaturation

Detect oxygen desaturation events as sustained drops from the recent peak SpO2 level, for example a fall greater than 2% lasting more than 3 seconds. This remains a useful confirmation signal because respiratory events often lead to intermittent hypoxemia. \[web:92\]\[web:96\]\[web:97\]

### 3.3 PAT fall

Add a new **PAT fall detector**.

The logic is:

1. derive or load PAT per beat or as a continuous/interpolated series,

2. estimate a local baseline PAT using a rolling mean or median,

3. detect a **fall in PAT** that exceeds a chosen threshold relative to that baseline,

4. optionally require the PAT fall to persist for a minimum duration or to occur over a physiologically plausible post-event window. \[web:88\]\[web:91\]\[web:93\]\[web:97\]

Physiological meaning:

- A **fall in PAT** usually corresponds to increased vascular tone and/or increased blood pressure following sympathetic activation or arousal.

- Sleep-breathing events, especially obstructive events with post-event arousal, are known to produce transient BP surges and autonomic activation, so PAT fall can act as another confirming marker. \[web:91\]\[web:93\]\[web:97\]

A practical implementation can define:

- `PAT\_baseline = rolling\_median(PAT, 20–30 s)`

- `PAT\_drop = PAT\_baseline - PAT`

- mark a PAT event when `PAT\_drop` exceeds a threshold, such as a fixed microsecond drop or a multiple of baseline variability.

## Step 4: Merge physiological markers into a response cluster

Create a combined list of physiological response times from:

- HR surge events,

- SpO2 desaturation events,

- PAT fall events. \[web:91\]\[web:97\]

Sort them and merge markers that occur within a chosen window, such as 20 seconds, into a single **physiological response cluster**. This keeps the method robust when multiple markers are triggered by the same breathing event.

The output of this stage is a set of cluster times, each representing a likely post-event physiological consequence.

## Step 5: Verify respiratory disturbance using a backward window

For each physiological response cluster, look backward in time to find respiratory candidate events that occurred shortly before it.

Suggested rule:

- search the airflow/effort event streams for candidate respiratory events that start within **0 to 50 seconds before** the physiological response cluster.

- if such events exist, keep the most recent one as the anchor respiratory event. \[web:97\]

This preserves the same causal structure as the current code:

- respiratory disturbance first,

- physiological consequence second.

Only events that satisfy this respiratory-to-physiology sequence should be counted as verified apnea candidates.

## Step 6: Classify verified events as central, obstructive, or mixed

Once a verified respiratory event has been identified, classify it by comparing what happened in **airflow** versus **effort** over the event interval.

This classification follows standard sleep-scoring logic:

- **Obstructive apnea**: airflow is absent or severely reduced while respiratory effort continues or increases.

- **Central apnea**: airflow is absent or severely reduced and inspiratory effort is absent throughout the event.

- **Mixed apnea**: the event begins with absent effort and later develops persistent effort while airflow remains absent/reduced. \[web:83\]\[web:85\]\[web:86\]\[web:89\]\[web:90\]\[web:92\]

### 6.1 Define the analysis window

For each verified event, create an event window using the corresponding airflow and/or effort no-intersection interval:

- start at the respiratory event start time,

- end at the event end time,

- optionally extend slightly before and after for context.

### 6.2 Measure airflow suppression

Within the event window, determine whether nasal airflow meets the algorithm’s airflow-disturbance criterion, such as:

- no-intersection event lasting at least 10 seconds,

- large amplitude reduction,

- or near-absence of airflow relative to baseline.

This establishes that the event behaves like an apnea in the airflow channel. \[web:89\]\[web:92\]

### 6.3 Measure effort presence or absence

Within the same event window, assess respiratory effort using the thoracic + abdominal composite or the two effort channels separately.

Effort should be described as one of the following states:

- **absent effort** — effort signal is strongly attenuated or absent for nearly the entire event,

- **present effort** — clear continuing respiratory effort is visible during the event,

- **transitioning effort** — early part absent, later part present. \[web:83\]\[web:85\]\[web:86\]\[web:89\]\[web:90\]

A practical implementation can use:

- no-intersection behaviour in the effort signal,

- event-window RMS or envelope amplitude relative to pre-event baseline,

- or segment-by-segment comparison of early versus late effort amplitude.

### 6.4 Decision rules

Suggested decision rules:

#### Obstructive apnea

Classify as **obstructive** when:

- airflow event is present,

- respiratory effort remains present or increases through most or all of the event,

- and a physiological response cluster confirms the event. \[web:85\]\[web:86\]\[web:89\]\[web:90\]

#### Central apnea

Classify as **central** when:

- airflow event is present,

- respiratory effort is absent throughout the event window,

- and a physiological response cluster confirms the event if your algorithm requires confirmation. \[web:83\]\[web:86\]\[web:89\]\[web:90\]\[web:92\]

#### Mixed apnea

Classify as **mixed** when:

- airflow event is present,

- the early portion of the event shows absent effort,

- the later portion shows return of effort while airflow remains absent/reduced,

- and the event still links to the physiological response cluster. \[web:89\]\[web:90\]

\[image:3\]

## Step 7: Proposed event-decision hierarchy

A practical decision hierarchy is:

1. Detect candidate airflow and effort disturbances.

2. Detect HR, SpO2, and PAT physiological markers.

3. Build merged physiological response clusters.

4. For each physiological response cluster, find the nearest preceding respiratory disturbance.

5. Mark that respiratory disturbance as a **verified event**.

6. Use airflow-versus-effort logic to label the verified event as:

   - obstructive,

   - central,

   - mixed,

   - or unclassified if the effort signal is ambiguous. \[web:83\]\[web:85\]\[web:86\]\[web:89\]\[web:90\]\[web:92\]

This approach separates **event detection** from **event typing**, which usually makes the method easier to tune and validate.

## Step 8: Validation against scored annotations

The expanded method should still be validated against manually scored respiratory events.

Validation should include:

- event-level matching against annotated apnea start times,

- separate confusion analysis for total apnea detection,

- subtype agreement for obstructive, central, and mixed labels when those labels exist in the annotation file. \[web:83\]\[web:90\]\[web:92\]

Recommended outputs:

- TP, FP, FN, TN for all apnea events,

- precision, sensitivity, specificity, and F1,

- subtype confusion matrix for central/obstructive/mixed,

- contribution analysis showing how often HR, SpO2, and PAT each participated in verification.

## Step 9: Suggested future visualisations

The later image set should show the following:

1. **Signal-processing flowchart** — raw signals to candidate events to physiological clustering to verified events.  
Placeholder: `\[image:1\]`

2. **Airflow vs effort event typing diagram** — three example event archetypes for obstructive, central, and mixed apnea.  
Placeholder: `\[image:2\]`

3. **PAT fall around respiratory event** — a schematic showing respiratory event onset followed by PAT shortening, HR surge, and SpO2 desaturation.  
Placeholder: `\[image:3\]`

## Summary of the expanded method

The expanded method keeps the current backward-verification apnea detector but strengthens it in two ways. First, it adds **PAT fall** as a blood-pressure-related physiological confirmation marker, which is useful because sleep-breathing events can trigger sympathetic activation and transient BP surges that are reflected in PAT. Second, it delays the merging of airflow and effort signals long enough to compare them directly, enabling classification of verified events into **central**, **obstructive**, and **mixed** apnea according to whether respiratory effort is absent, present, or transitions during the airflow event. \[web:83\]\[web:85\]\[web:86\]\[web:89\]\[web:91\]\[web:92\]\[web:97\]

