
%%
clc, clear;

%%
%Thresh_values = [0.4, 0.8, 1.2, 1.6, 2, 2.4, 2.6]; %
%Thresh_values = [1.6, 2, 2.4, 2.6,3.0, 3.4, 3.4, 3.6];
%Thresh_values = [3.6, 3.8, 4.0, 4.2, 4.4, 4.6, 4.8];     Thresh_value
%Thresh_values = [1.8, 2, 2.2, 2.4, 2.6, 3.2];
%Thresh_values = [0.0006, 0.0007, 0.0008, 0.0009, 0.001, 0.0011, 0.0012];
Patient_num = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12];
for j = 1:length(Patient_num)
    patient = Patient_num(j);
    fprintf('For Patient: %.0f\n', patient);
%%

% ONE PATIENT AT A TIME FROM HERE
include_apnea = true;
% patient = 10;

% Load data
text_file = sprintf('Visual_scoring1_excerpt%d.txt', patient);
load(sprintf('excerpt%d.mat', patient));
graph = 1;

% Thresholds as strings
HrThresh = '2.7*mean(abs(heart_rate2))';
O2Thresh = 2;

% Set sampling frequency and time vector
fs = 200;
tm = (0:length(VAB)-1) / fs;

% Process ECG data
[locs, peaks] = ECG_anno(ECG, fs, 0);
ECG_1_25 = BP_filter(fs, ECG, 1, 25);

% Normalise input signals
NAS_raw = normalise(LP_filter(fs, NAF2P_A1, 0.5));
RespT_raw = normalise(LP_filter(fs,simple_dc(fs, VTH), 0.5));
RespA_raw = normalise(LP_filter(fs, simple_dc(fs, VAB), 0.5));

% Set moving window duration (in seconds)
window_sec = 20;

% Calculate window size in samples
window_samples = round(window_sec * fs);

% Add moving mean of absolute value (20s window) to each signal
NAS_LP   = NAS_raw   +  movmean(abs(NAS_raw),   window_samples, 'Endpoints', 'shrink');
% RespT_LP = RespT_raw + 0.8* movmean(abs(RespT_raw), window_samples, 'Endpoints', 'shrink');
% RespA_LP = RespA_raw + 0.8 * movmean(abs(RespA_raw), window_samples, 'Endpoints', 'shrink');

% Combine thoracic and abdominal for composite respiratory effort
Resp_all    = RespT_raw + RespA_raw;
Resp_all_LP = Resp_all  + 0.5 * movmean(abs(Resp_all), window_samples, 'Endpoints', 'shrink');


% Introduce a 1-second phase shift (delay) to the LP signals
shift_samples = 0.5*fs; % Number of samples for 1 second

% Shift each LP signal
NAS_LP_shifted = [zeros(shift_samples,1); NAS_LP(1:end-shift_samples)];
Resp_all_LP_shifted = [zeros(shift_samples,1); Resp_all_LP(1:end-shift_samples)];


% Find intersection indices
NAS_intersections = find_intersections(NAS_raw, NAS_LP_shifted);
Resp_intersections = find_intersections(Resp_all, Resp_all_LP_shifted);

% Convert indices to time
NAS_times = tm(NAS_intersections);
Resp_times = tm(Resp_intersections);

% Set minimum gap duration (in seconds)
min_gap_duration = 10;

% Get total duration of the signal
total_duration = tm(end);

% Find no-intersection events for both signals
[nas_event_starts, nas_event_ends] = find_no_intersection_events(NAS_times, total_duration, min_gap_duration);
[resp_event_starts, resp_event_ends] = find_no_intersection_events(Resp_times, total_duration, min_gap_duration);

max_diff_sec = 20; % Set your merging window (in seconds)

% Combine and sort all events
all_events = sort([nas_event_starts(:); resp_event_starts(:)]);

if isempty(all_events)
    combined_locs = [];
else
    combined_locs = all_events(1);
    for i = 2:length(all_events)
        if all_events(i) - combined_locs(end) > max_diff_sec
            combined_locs = [combined_locs; all_events(i)];
        end
        % If within max_diff_sec, skip (keep only the earlier event)
    end
end

if graph == 1
    figure('Position', [100, 100, 1200, 1200]);
    
    % Subplot 1: Respiratory Signals (Raw, LPF, Intersections)
    subplot(4,1,1);
    plot(tm, Resp_all, 'b', 'DisplayName', 'Resp');
    hold on;
    plot(tm, Resp_all_LP_shifted, 'k', 'DisplayName', 'Resp (Shifted)');
    if ~isempty(Resp_times)
        plot(Resp_times, Resp_all(Resp_intersections), 'go', ...
            'MarkerSize', 6, 'DisplayName', 'Intersections');
    end
    hold off;
    legend('show');
    title('Respiratory Signals and Intersections, with Visual Apnea Event Scoring ');
    xlabel('Time (s)');
    ylabel('Amplitude');
    
    % Subplot 2: No-Intersection Events for Both Respiratory and Nasal
    subplot(4,1,2);
    hold on;
    % Subplot 2: Nasal Pressure
    plot(tm, NAS_raw, 'b', 'DisplayName', 'NAS');
    plot(tm, NAS_LP_shifted, 'k', 'DisplayName', 'NAS (Shifted)');
    if ~isempty(NAS_times)
        plot(NAS_times, NAS_raw(NAS_intersections), 'go', 'MarkerSize', 6, 'DisplayName', 'Intersections');
    end
    ylims = ylim;
    y_pos = ylims(2);

    % Respiratory events (red)
    for k = 1:length(resp_event_starts)
        if k == 1
            xline(resp_event_starts(k), 'r:', 'LineWidth', 2, ...
                'DisplayName', 'Resp No-Intersection Event');
        else
            xline(resp_event_starts(k), 'r:', 'LineWidth', 2, ...
                'HandleVisibility', 'off');
        end
        event_duration = resp_event_ends(k) - resp_event_starts(k);
        text(resp_event_starts(k), y_pos*0.08, ...
            sprintf('%.1fs', event_duration), ...
            'Color', 'r', 'FontWeight', 'bold', ...
            'FontSize', 10, 'VerticalAlignment','bottom');
    end

    % Nasal events (black)
    for k = 1:length(nas_event_starts)
        if k == 1
            xline(nas_event_starts(k), 'k:', 'LineWidth', 2, ...
                'DisplayName', 'NAS No-Intersection Event');
        else
            xline(nas_event_starts(k), 'k:', 'LineWidth', 2, ...
                'HandleVisibility', 'off');
        end
        event_duration = nas_event_ends(k) - nas_event_starts(k);
        text(nas_event_starts(k), y_pos*0.01, ...
            sprintf('%.1fs', event_duration), ...
            'Color', 'k', 'FontWeight', 'bold', ...
            'FontSize', 10, 'VerticalAlignment','bottom');
    end

    hold off;
    legend('show');
    title('Nasal Sensor Data with Respiration Events (red) and Nasal Events (black)');
    xlabel('Time (s)');
    ylabel('Event Indicator');


    % Subplot 3: SAO2 and HR
    subplot(4,1,3);
    plot(tm, SAO2);
    hold on;
    title('Heart Rate and SAO2 with Detected Peaks and Hypoxia Events');
    xlabel('Time (s)');
    ylabel('SAO2 (%) and HR (BPM)');
    ylim([50 100]);
end

% --- Apnea annotation from visual scoring (unchanged) ---
if include_apnea
    fileID = fopen(text_file, 'r');
    current_type = '';
    apnea_times = [];
    apnea_durations = [];
    apnea_types = {};
    while ~feof(fileID)
        tline = fgetl(fileID);
        if isempty(tline) || strcmp(tline, '')
            continue;
        end
        if contains(tline, '[')
            current_type = extractBetween(tline, '[', ']');
            current_type = current_type{1};
        else
            data = sscanf(tline, '%f %f');
            if ~isempty(data) && ~isempty(current_type)
                apnea_times = [apnea_times; data(1)];
                apnea_durations = [apnea_durations; data(2)];
                apnea_types{end+1} = current_type;
            end
        end
    end
    fclose(fileID);

    if graph == 1
        subplot(4,1,1);
        ylims = ylim;
        y_pos = ylims(2);
        for i = 1:length(apnea_times)
            if contains(apnea_types{i}, 'apnea') || contains(apnea_types{i}, 'hypopnea')
                if contains(apnea_types{i}, 'hypopnea')
                    xline(apnea_times(i), 'b--', 'LineWidth', 1, 'HandleVisibility', 'off');
                else
                    xline(apnea_times(i), 'k--', 'LineWidth', 1, 'HandleVisibility', 'off');
                end
                event_text = strrep(apnea_types{i}, 'vis_', '');
                event_text = strrep(event_text, '_', ' ');
                event_text = strrep(event_text, '/PSG', '');
                text(apnea_times(i), y_pos*0.15, ...
                    {event_text, sprintf('%.1fs', apnea_durations(i))}, ...
                    'Rotation', 90, ...
                    'HorizontalAlignment', 'right', ...
                    'VerticalAlignment', 'bottom', ...
                    'FontSize', 8, ...
                    'Color', 'r'); % Set text color to blue
            end
        end
    end
end


% --- Count and display apnea event types ---
% Convert all event type strings to lowercase for robust matching
apnea_types_lower = lower(apnea_types);

% Count each type
num_hypopnea         = sum(contains(apnea_types_lower, 'hypopnea'));
num_obstructive_apnea = sum(contains(apnea_types_lower, 'obstructive_apnea'));
num_mixed_apnea      = sum(contains(apnea_types_lower, 'mixed_apnea'));
num_central_apnea    = sum(contains(apnea_types_lower, 'central_apnea'));
Ann_act = length(apnea_types);

% Calculate percentage of hypopnea events for this subject
if Ann_act > 0
    hypopnea_pct = 100 * num_hypopnea / Ann_act;
else
    hypopnea_pct = 0;
end
hypopnea_percentages(j) = hypopnea_pct;

% Write results to the command window
fprintf('Number of hypopnea events: %d\n', num_hypopnea);
fprintf('Number of obstructive apnea events: %d\n', num_obstructive_apnea);
fprintf('Number of mixed apnea events: %d\n', num_mixed_apnea);
fprintf('Number of central apnea events: %d\n', num_central_apnea);


% --- SAO2 and HR event detection and plotting (unchanged) ---
O2annotation_times = [];
in_event = false;
event_start = 0;
min_peak_distance = round(10 * fs);
min_peak_prominence = 0.2;
[peak_values, peak_locs] = findpeaks(SAO2, 'MinPeakDistance', min_peak_distance, 'MinPeakProminence', min_peak_prominence);
last_peak = SAO2(1);
for i = 2:length(SAO2)
    if ismember(i, peak_locs)
        last_peak = SAO2(i);
    end
    if SAO2(i) < last_peak - O2Thresh
        if ~in_event
            event_start = i;
            in_event = true;
        end
    else
        if in_event
            event_duration = (i - event_start) / fs;
            if event_duration > 3
                O2annotation_times = [O2annotation_times, tm(event_start)];
            end
            in_event = false;
        end
    end
end
if graph == 1
    subplot(4,1,3);
    for i = 1:length(O2annotation_times)
        xline(O2annotation_times(i), 'r-', 'LineWidth', 1, 'HandleVisibility', 'off');
    end
    plot(tm(peak_locs), peak_values, 'go', 'MarkerSize', 8, 'MarkerFaceColor', 'g');
    last_peak_line = zeros(size(SAO2));
    current_peak_index = 1;
    for i = 1:length(SAO2)
        if current_peak_index < length(peak_locs) && i >= peak_locs(current_peak_index + 1)
            current_peak_index = current_peak_index + 1;
        end
        last_peak_line(i) = peak_values(current_peak_index);
    end
    plot(tm, last_peak_line, 'g--', 'LineWidth', 1);
    sgtitle(sprintf('Respiratory, Nasal, SAO2 and Heartrate signals, with True positive markers - Patient %d', patient));
    set(gcf, 'Color', 'w');
end

% --- Heart Rate detection and plotting (unchanged) ---
heart_rate = calculate_heart_rate(locs, fs, length(ECG));
heart_rate2 = simple_dc(fs, heart_rate);
HrThresh_val = eval(HrThresh);
[HR_values, HR_locs] = findpeaks(heart_rate2, 'MinPeakDistance', 30*fs, 'MinPeakProminence', HrThresh_val);
if graph == 1
    subplot(4,1,3);
    hold on;
    plot(tm, heart_rate, 'm');
    plot(tm(HR_locs), heart_rate(HR_locs), 'ro', 'MarkerSize', 12);
    ylim([60 120]);
    legend('SAO2', 'Detected Peaks', 'Last Peak Value', 'HR signal', 'HR Peaks');
end
HR_times = HR_locs/fs;

% Combine and sort all HR and O2 events
all_events = sort([HR_times(:); O2annotation_times(:)]);

% Merge events within 10 seconds
merged_events = [];
if ~isempty(all_events)
    merged_events = all_events(1);
    for i = 2:length(all_events)
        if all_events(i) - merged_events(end) > 20
            merged_events = [merged_events; all_events(i)];
        end
        % If within 10s, skip (keep only the earlier event)
    end
end

% For each merged event, if a combined_locs event exists within 0–50s before, use its position
verified_events = [];
for i = 1:length(merged_events)
    % Find all combined_locs events within 0–50 seconds before this event
    valid_locs = combined_locs((merged_events(i) - combined_locs) >= 0 & (merged_events(i) - combined_locs) <= 50);
    if ~isempty(valid_locs)
        % Store the most recent combined_locs event (i.e., the largest value)
        verified_events = [verified_events; max(valid_locs)];
    else
        % % Otherwise, keep the original merged event time
        % verified_events = [verified_events; merged_events(i)];
    end
end


apnea_times2 = apnea_times * fs; 
verified_events = round(verified_events*fs);
verified_events2 = verified_events;

% Prepare event arrays for stem plots
ECG_length = length(ECG);
% Initialize counters
TP = 0;
FP = 0;
FN = 0;

% Create a temporary copy of apnea_times2
apnea_times_temp = apnea_times2;
TP_array = [];

% Loop through verified events to find TP
for i = 1:length(verified_events2)
    current_loc = verified_events2(i);
    
    % Find nearby events within the specified window
    %nearby_indices = find((apnea_times_temp >= current_loc - 40*fs) & (apnea_times_temp <= current_loc + 20*fs));
    nearby_indices = find((apnea_times_temp >= current_loc - 20*fs) & (apnea_times_temp <= current_loc + 20*fs));

    if ~isempty(nearby_indices)
        TP = TP + 1;
        % Remove the used event from apnea_times_temp
        apnea_times_temp(nearby_indices(1)) = [];
        TP_array = [TP_array, current_loc];
    else
        FP = FP + 1;
    end

end

FP2 = length(apnea_times_temp);

% Loop through apnea times to find FN
for i = 1:length(apnea_times2)
    current_loc = apnea_times2(i);
    has_nearby = any((verified_events2 >= current_loc - 50*fs) & (verified_events2 <= current_loc + 20*fs));
    
    if ~has_nearby
        FN = FN + 1;
    end
end

% Calculate TN
total_duration = max([max(apnea_times2), max(verified_events2)]);
total_windows = floor(total_duration / (fs*40)); % Assuming 40-second windows

TN = total_windows - (TP + FP + FN);

Ann_act = length(apnea_times);
Ann_pred = length(verified_events);
percentage = (TP *100)/(length(apnea_times));
percentage_pred = (TP *100)/(length(verified_events));

precision = TP / (TP + FP);
sensitivity = TP / (TP + FN);
specificity = TN / (TN + FP);

f1_score = 2 * (precision * sensitivity) / (precision + sensitivity);

fprintf('For patient: %.3f\n', patient);
fprintf('Precision: %.3f\n', precision);
fprintf('Sensitivity: %.3f\n', sensitivity);
fprintf('Specificity: %3f\n', specificity);
fprintf('F1: %3f\n', f1_score);
fprintf('Actual Events: %.2f\n', Ann_act);
fprintf('Calculated Events: %.2f\n', Ann_pred);
fprintf('Percentage Actual as TP: %.2f%%\n', percentage);
fprintf('Percentage Predicted as TP: %.2f%%\n', percentage_pred);
fprintf('True Positives: %.2f\n', TP);
fprintf('False Positives: %.2f\n', FP);
fprintf('False Negatives: %.2f\n', FN);
fprintf('True Negatives: %.2f\n', TN);
disp(' ');


% Use sorted and scaled event arrays 
apnea_times = apnea_times * fs;
apnea_times = sort(apnea_times);
verified_events2 = verified_events';

% Create actual_events array
actual_events = zeros(1, ECG_length);
for i = 1:length(apnea_times2)
    event = round(apnea_times2(i));
    if event >= 1 && event <= ECG_length
        actual_events(event) = 1;
    end
end

% Create predicted_events array
predicted_events = zeros(1, ECG_length);
for i = 1:length(verified_events2)
    event = round(verified_events2(i));
    if event >= 1 && event <= ECG_length
        predicted_events(event) = 1;
    end
end

if graph == 1
    % Get logical indices or event indices as row vectors
    actual_event_indices = find(actual_events);          % Indices where actual_events == 1
    predicted_event_indices = find(predicted_events);    % Indices where predicted_events == 1
    TP_event_indices = TP_array(:);                      % Ensure TP_array is a column vector

    % Extract times using indices and ensure column vectors
    actual_event_times = tm(actual_event_indices)';
    predicted_event_times = tm(predicted_event_indices)';
    TP_event_times = tm(TP_event_indices)';

    % Plot actual, predicted, and TP events in subplot 5
    subplot(4,1,4);
        hold on;
    if ~isempty(actual_event_times)
        stem(actual_event_times, ones(length(actual_event_times),1), 'b');
    end
    if ~isempty(predicted_event_times)
        stem(predicted_event_times, 0.8 * ones(length(predicted_event_times),1), 'r');
    end
    if ~isempty(TP_event_times)
        stem(TP_event_times, 0.9 * ones(length(TP_event_times),1), 'g');
    end
    hold off;
    title('Actual vs Predicted Apnea Events');
    legend('Actual', 'Predicted', 'TP');
    xlabel('Time (seconds)');
    ylabel('Event Occurrence');

    % Link the x-axes of all subplots
    ax = findall(gcf, 'type', 'axes');
    linkaxes(ax, 'x');
end


%_________________________________________________________________________________________________________________________
%%
    Thresh_vals(j) = patient;
    TP_vals(j) = TP;
    FP_vals(j) = FP;
    TN_vals(j) = TN;
    precision_vals(j) = precision;
    sensitivity_vals(j) = sensitivity;
    specificity_vals(j) = specificity;
    calculated_vals(j) = Ann_pred;
    actual_vals(j) = Ann_act;
    Percentage_vals(j) = percentage;
    Percentage_vals2(j) = percentage_pred;
    F1_vals(j) = f1_score;

end

%% Extract data from the results arrays
thresholds = Patient_num;
percentages = Percentage_vals;
percentage_preds = Percentage_vals2;  % Corrected typo
precisions = precision_vals;
sensitivities = sensitivity_vals;
specificities = specificity_vals;
true_positives = TP_vals;
false_positives = FP_vals;
true_negatives = TN_vals;
f1_scores = F1_vals;  % Added F1 scores
% Create a new figure
figure('Position', [100, 100, 1200, 800]);

% --- Subplot 1: Precision, Sensitivity, Specificity, F1 Score (Grouped Bar) ---
subplot(2, 2, 1);
bar([precisions(:), sensitivities(:), specificities(:), f1_scores(:)], 'grouped');
xlabel('Subject Number');
ylabel('Score');
title('Precision, Sensitivity, Specificity, and F1 Score by Subject');
legend({'Precision', 'Sensitivity', 'Specificity', 'F1 Score'}, 'Location', 'best');
set(gca, 'XTick', 1:length(thresholds));
set(gca, 'XTickLabel', arrayfun(@(x) sprintf('%.2f', x), thresholds, 'UniformOutput', false));
xtickangle(45);
grid on;

% --- Subplot 2: TP, FP, TN (Grouped Bar) ---
subplot(2, 2, 2);
bar([true_positives(:), false_positives(:), true_negatives(:)], 'grouped');
xlabel('Subject Number');
ylabel('Count');
title('TP, FP, and TN vs Subject');
legend({'True Positives', 'False Positives', 'True Negatives'}, 'Location', 'best');
set(gca, 'XTick', 1:length(thresholds));
set(gca, 'XTickLabel', arrayfun(@(x) sprintf('%.2f', x), thresholds, 'UniformOutput', false));
xtickangle(45);
grid on;

subplot(2, 2, 3);
hBar = bar([percentages(:), percentage_preds(:)], 'grouped');
xlabel('Subject Number');
ylabel('Percentage Score');
title('Percentage TP of Events');
legend({'Actual', 'Predicted'}, 'Location', 'best');
set(gca, 'XTick', 1:length(thresholds));
set(gca, 'XTickLabel', arrayfun(@(x) sprintf('%.2f', x), thresholds, 'UniformOutput', false));
xtickangle(45);
grid on;

% Add total number of actual events above each 'Actual' bar
hold on;
% Get X positions of bars (works for grouped bar charts)
xtips = hBar(1).XEndPoints; % X positions for 'Actual' bars
ytips = hBar(1).YEndPoints; % Heights of 'Actual' bars

for i = 1:length(xtips)
    text(xtips(i), ytips(i) + 3, ...       % Place label slightly above bar
        num2str(actual_vals(i)), ...       % Number of actual events
        'HorizontalAlignment', 'center', ...
        'FontWeight', 'bold', ...
        'Color', 'k', ...
        'FontSize', 10);
end
hold off;

% --- Subplot 4: TP, FP, TN Distribution (Stacked Bar) ---
subplot(2, 2, 4);
bar([true_positives(:), false_positives(:), true_negatives(:)], 'stacked');
xlabel('Subject Number');
ylabel('Count');
title('TP, FP, and TN Distribution');
legend({'True Positives', 'False Positives', 'True Negatives'}, 'Location', 'best');
set(gca, 'XTick', 1:length(thresholds));
set(gca, 'XTickLabel', arrayfun(@(x) sprintf('%.2f', x), thresholds, 'UniformOutput', false));
xtickangle(45);
grid on;

% --- Figure formatting ---
sgtitle('Analysis of Results for all Subjects');
set(gcf, 'Color', 'w');
% After your patient loop, assuming all *_vals and hypopnea_percentages are filled:
ResultsTable = table(Patient_num(:), ...
    precision_vals(:), sensitivity_vals(:), specificity_vals(:), F1_vals(:), ...
    TP_vals(:), FP_vals(:), TN_vals(:), ...
    actual_vals(:), calculated_vals(:), ...
    Percentage_vals(:), Percentage_vals2(:), ...
    hypopnea_percentages(:), ...
    'VariableNames', {'Patient', 'Precision', 'Sensitivity', 'Specificity', 'F1', ...
                      'TP', 'FP', 'TN', 'ActualEvents', 'PredictedEvents', ...
                      'PercentActualTP', 'PercentPredTP', 'HypopneaPercent'});
% Calculate totals and weighted averages
total_TP = sum(ResultsTable.TP);
total_FP = sum(ResultsTable.FP);
total_TN = sum(ResultsTable.TN);
total_ActualEvents = sum(ResultsTable.ActualEvents);
total_PredictedEvents = sum(ResultsTable.PredictedEvents);

% Weighted averages for ratios/percentages
weighted_precision = sum(ResultsTable.Precision .* ResultsTable.ActualEvents) / total_ActualEvents;
weighted_sensitivity = sum(ResultsTable.Sensitivity .* ResultsTable.ActualEvents) / total_ActualEvents;
weighted_specificity = sum(ResultsTable.Specificity .* ResultsTable.ActualEvents) / total_ActualEvents;
weighted_F1 = sum(ResultsTable.F1 .* ResultsTable.ActualEvents) / total_ActualEvents;
weighted_PercentActualTP = sum(ResultsTable.PercentActualTP .* ResultsTable.ActualEvents) / total_ActualEvents;
weighted_PercentPredTP = sum(ResultsTable.PercentPredTP .* ResultsTable.ActualEvents) / total_ActualEvents;
weighted_HypopneaPercent = sum(ResultsTable.HypopneaPercent .* ResultsTable.ActualEvents) / total_ActualEvents;

% Convert Patient column to cell array of strings for compatibility
ResultsTable.Patient = cellstr(num2str(ResultsTable.Patient));

% Prepare summary row as a cell array
summary_row = { ...
    'Total/Average', weighted_precision, weighted_sensitivity, weighted_specificity, weighted_F1, ...
    total_TP, total_FP, total_TN, total_ActualEvents, total_PredictedEvents, ...
    weighted_PercentActualTP, weighted_PercentPredTP, weighted_HypopneaPercent ...
};

% Convert ResultsTable to cell array and append summary row
ResultsCell = [ResultsTable.Properties.VariableNames; table2cell(ResultsTable); summary_row];

% Write the cell array to Excel
writecell(ResultsCell, 'ResultsTable_with_summary.xlsx');

% Display confirmation
disp('ResultsTable_with_summary.xlsx has been saved with a summary row.');
