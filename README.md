# kinarm-binary-converter

A standalone command-line tool that converts `.kinarm` files (Dexterit-E ZIP archives from KINARM exoskeleton robots) to open formats — **no MATLAB required**.

## Why

The official KINARM conversion pipeline requires MATLAB and the KINARM Analysis Scripts toolbox. This means:

- Starting MATLAB just to convert files
- No headless/automated pipeline support
- Every lab member needs a MATLAB license
- Hard to integrate into Python, R, or CI workflows

This tool replaces that step entirely. It reads the `.kinarm` binary format directly and writes standard open formats that work everywhere.

---

## Download

Pre-compiled binaries — no build tools needed:

| Platform | Download |
|----------|----------|
| macOS (Apple Silicon) | [kinarm-binary-converter-macos-arm64](https://github.com/Neuromechanics-Lab/kinarm-binary-converter/releases/latest/download/kinarm-binary-converter-macos-arm64) |
| Windows x64 | [kinarm-binary-converter-win64.exe](https://github.com/Neuromechanics-Lab/kinarm-binary-converter/releases/latest/download/kinarm-binary-converter-win64.exe) |
| Linux x86_64 | [kinarm-binary-converter-linux-x86_64](https://github.com/Neuromechanics-Lab/kinarm-binary-converter/releases/latest/download/kinarm-binary-converter-linux-x86_64) |

Or browse all releases: [GitHub Releases](https://github.com/Neuromechanics-Lab/kinarm-binary-converter/releases)

**macOS note:** after downloading, you may need to allow the binary in System Settings → Privacy & Security, or run:
```bash
xattr -d com.apple.quarantine kinarm-binary-converter-macos-arm64
chmod +x kinarm-binary-converter-macos-arm64
```

---

## Output Formats

| Format | File | Use case |
|--------|------|----------|
| JSON | `<stem>.json` | Structured data for R/Python pipelines, web apps |
| CSV | `<stem>_timeseries.csv` | Flat per-sample table — Excel, pandas, R `read.csv()`, Julia |
| MAT | `<stem>.mat` | MATLAB v5 format with same struct layout as `exam_load()` — drop-in for existing MATLAB workflows |

The `.mat` output uses the same field names as the official KINARM Analysis Scripts (`data.c3d(i).Right_HandX`, `data.c3d(i).ANALOG.RATE`, etc.) so it works with any existing MATLAB code that calls `exam_load()`.

All 41 channels are included in every format: hand position, joint angles/velocities/accelerations, motor torques, force sensor data, and EMG.

---

## Usage

### Terminal / command line

```bash
kinarm-binary-converter <input.kinarm> <output_stem> [--formats json,csv,mat]
```

Writes all three formats by default:
```
output_stem.json
output_stem_timeseries.csv
output_stem.mat
```

Examples:
```bash
# Convert a single file (all formats)
kinarm-binary-converter subject01_task.kinarm results/subject01

# JSON + CSV only
kinarm-binary-converter subject01_task.kinarm results/subject01 --formats json,csv

# Batch convert a folder
for f in data/*.kinarm; do
  stem="converted/$(basename "${f%.kinarm}")"
  kinarm-binary-converter "$f" "$stem"
done
```

### From R

```r
system2("kinarm-binary-converter",
        args = c("subject01.kinarm", "output/subject01"),
        stdout = TRUE, stderr = TRUE)

df <- read.csv("output/subject01_timeseries.csv")
hold <- subset(df, in_home_window == 1)
```

### From Python

```python
import subprocess, json

subprocess.run(["kinarm-binary-converter", "subject01.kinarm", "output/subject01"])

with open("output/subject01.json") as f:
    data = json.load(f)
```

### From MATLAB

```matlab
% Convert the file
system('kinarm-binary-converter subject01.kinarm output/subject01');

% Load the .mat result — same struct as exam_load()
load('output/subject01.mat');   % loads variable 'data'
data.c3d(1).Right_HandX        % hand X position, trial 1 (m)
data.c3d(1).ANALOG.RATE        % sample rate (Hz)
data.c3d(1).EVENTS             % event names and times

% Or load the CSV
T = readtable('output/subject01_timeseries.csv');
hold_data = T(T.in_home_window == 1, :);
```

---

## Protocol Detection

The converter auto-detects file type from the `protocol` field in the exam info:

- **Calibration** (protocol name contains "calibration"): home window = `TARGET2_ONSET` → end of trial. The participant holds at the nearest target for the full trial duration.
- **Task** (all other protocols): home window = `IN_TARGET2` → last `WAIT_CORRECT`. Captures the pre-reach home-hold dwell period.

`home_valid` is set to `false` and `home_start_s`/`home_end_s` are null if the required events are absent.

---

## JSON Structure

```json
{
  "metadata": {
    "subject_id": "P001",
    "protocol": "Visually Guided Reaching",
    "protocol_mode": "task",
    "dex_ver": "3.9.2",
    "robot_arm": "right",
    "operator": "...",
    "posture": "SEATED",
    "robotver": "KINARM_EP_Rev2"
  },
  "trials": [
    {
      "trial_id": "02_01_01",
      "block": 2, "trial": 1, "repeat": 1,
      "sample_rate": 1000,
      "home_start_s": 2.94,
      "home_end_s": 5.61,
      "home_valid": true,
      "events": [
        {"name": "TARGET1_ONSET", "time_s": 0.001},
        {"name": "IN_TARGET2",    "time_s": 2.94},
        {"name": "WAIT_CORRECT",  "time_s": 5.61}
      ],
      "hand_full": {"time_s": [...], "x": [...], "y": [...]},
      "hand_home": {"time_s": [...], "x": [...], "y": [...]},
      "vel_home":  {"vx": [...], "vy": [...]},
      "channels": {
        "Right_HandX": [...],
        "Right_HandY": [...],
        "Right_L1Ang": [...],
        "bicep": [...],
        "..."
      }
    }
  ]
}
```

---

## CSV Structure

One row per sample across all trials. Fixed columns followed by one column per channel:

```
trial_id, block, trial, repeat, time_s, in_home_window, Right_HandX, Right_HandY, Right_L1Ang, ...
```

- `in_home_window` — 1 if sample falls within the home-hold window, 0 otherwise
- All 41 channels present as numeric columns
- Empty cells where a channel was not recorded for that trial

---

## MAT Structure

MATLAB v5 format, compatible with all MATLAB versions and `scipy.io.loadmat`. Struct layout mirrors `exam_load()` from the official KINARM Analysis Scripts:

```matlab
load('subject01.mat');              % loads variable 'data'
data.c3d(1).Right_HandX            % right hand X position, trial 1 (m)
data.c3d(1).Right_HandY            % right hand Y position, trial 1 (m)
data.c3d(1).Right_L1Ang            % shoulder joint angle (rad)
data.c3d(1).Right_L2Ang            % elbow joint angle (rad)
data.c3d(1).Right_FS_ForceX        % force sensor X (N)
data.c3d(1).bicep                  % EMG — bicep
data.c3d(1).ANALOG.RATE            % sample rate (Hz)
data.c3d(1).EVENTS.LABELS          % event name strings (cell array)
data.c3d(1).EVENTS.TIMES           % event timestamps (s)
data.filename                      % source .kinarm filename
```

In Python:
```python
import scipy.io
mat = scipy.io.loadmat("subject01.mat", squeeze_me=True)
trial1 = mat["data"]["c3d"].item()[0]
x = trial1["Right_HandX"]
```

---

## Advanced

### Building from source

Only needed if you want to modify the converter or build for an unsupported platform.

**Prerequisites:** CMake 3.14+, C++17 compiler (GCC, Clang, or MSVC). No other dependencies — ZIP reading uses bundled [miniz](https://github.com/richgel999/miniz).

**macOS / Linux:**
```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/kinarm-binary-converter --help
```

**Windows (MSVC):**
```cmd
cmake -S . -B build
cmake --build build --config Release
.\build\Release\kinarm-binary-converter.exe --help
```

### What's in a `.kinarm` file

A `.kinarm` file is a ZIP archive containing per-trial binary data in Dexterit-E format:

```
raw/
  common/          # Exam-level parameters
  02_01_01/        # Block 2, Trial 1, Repeat 1
    Right_Hand.position        # XY hand position (float32 pairs, ~1 kHz)
    Right_L1Ang.kinematics     # Shoulder joint angle (float32, ~1 kHz)
    Right_L2Ang.kinematics     # Elbow joint angle
    Right_HandXVel.kinematics  # Hand velocity X (if recorded)
    Right_FS_ForceX.kinematics # Force sensor X (if present)
    examevents.bin             # Event names + timestamps (UTF-16LE + float32)
    ...
  02_02_01/
    ...
exam_info_5.txt    # Subject/protocol metadata (Java properties format)
```

All binary data is little-endian. Time-series channels are prefixed with a small header (version, channel count, UTF-16LE name, sample count) followed by raw float32 arrays.

---

## License

MIT
