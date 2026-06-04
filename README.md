# kinarm-binary-converter

A standalone C++ command-line tool that converts `.kinarm` files (Dexterit-E ZIP archives from KINARM exoskeleton robots) to open formats — **no MATLAB required**.

## Why

The official KINARM conversion pipeline requires MATLAB and the KINARM Analysis Scripts toolbox. This means:

- Starting MATLAB just to convert files
- No headless/automated pipeline support
- Every lab member needs a MATLAB license
- Hard to integrate into Python, R, or CI workflows

This tool replaces that step entirely. It reads the `.kinarm` binary format directly and writes standard open formats that work everywhere.

---

## Output Formats

| Format | File | Use case |
|--------|------|----------|
| JSON | `<stem>.json` | Structured data for R/Python pipelines, web apps |
| CSV | `<stem>_timeseries.csv` | Flat per-sample table — Excel, pandas, R `read.csv()`, Julia |
| MAT | `<stem>.mat` | MATLAB v5 format with same struct layout as `exam_load()` — drop-in for existing MATLAB workflows |

The `.mat` output uses the same field names as the official KINARM Analysis Scripts (`data.c3d(i).Right_HandX`, `data.c3d(i).ANALOG.RATE`, etc.) so it works with any existing MATLAB code that calls `exam_load()`.

---

## Usage

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

# Batch convert a folder (shell loop)
for f in data/*.kinarm; do
  stem="converted/$(basename "${f%.kinarm}")"
  kinarm-binary-converter "$f" "$stem"
done
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
      "vel_home":  {"vx": [...], "vy": [...]}
    }
  ]
}
```

---

## CSV Structure

One row per sample across all trials. Columns:

```
trial_id, block, trial, repeat, time_s, x, y, vx, vy, in_home_window
```

- `x`, `y` — hand position in meters (global coordinate system)
- `vx`, `vy` — hand velocity in m/s (empty if not recorded)
- `in_home_window` — 1 if sample falls within the home-hold window, 0 otherwise

Read in Python:
```python
import pandas as pd
df = pd.read_csv("subject01_timeseries.csv")
hold = df[df["in_home_window"] == 1]
```

Read in R:
```r
df <- read.csv("subject01_timeseries.csv")
hold <- subset(df, in_home_window == 1)
```

Read in MATLAB:
```matlab
T = readtable('subject01_timeseries.csv');
hold_data = T(T.in_home_window == 1, :);
```

---

## MAT Structure

The `.mat` file uses MATLAB v5 format (compatible with all MATLAB versions and `scipy.io.loadmat`). The struct layout mirrors `exam_load()` from the official KINARM Analysis Scripts:

```matlab
% In MATLAB:
load('subject01.mat');         % loads variable 'data'
data.c3d(1).Right_HandX       % right hand X position, trial 1 (m)
data.c3d(1).Right_HandY       % right hand Y position, trial 1 (m)
data.c3d(1).Right_L1Ang       % shoulder joint angle, trial 1 (rad)
data.c3d(1).Right_L2Ang       % elbow joint angle, trial 1 (rad)
data.c3d(1).ANALOG.RATE       % sample rate (Hz)
data.c3d(1).EVENTS            % event names and times
data.filename                 % source .kinarm filename
```

In Python with scipy:
```python
import scipy.io
mat = scipy.io.loadmat("subject01.mat", squeeze_me=True)
trial1 = mat["data"]["c3d"][0]
x = trial1["Right_HandX"]
```

---

## Build

### Prerequisites
- CMake 3.14+
- C++17 compiler (GCC, Clang, MSVC)
- No other dependencies — ZIP reading uses bundled [miniz](https://github.com/richgel999/miniz)

### macOS / Linux
```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/kinarm-binary-converter --help
```

### Windows (MSVC)
```cmd
cmake -S . -B build
cmake --build build --config Release
.\build\Release\kinarm-binary-converter.exe --help
```

### Pre-compiled binaries

Pre-compiled binaries for macOS (arm64, x86_64) and Windows x64 are available on the [Releases](https://gitlab.com/neurotrophy-git/kinarm-binary-converter/-/releases) page.

---

## Integration

### Use from R (no shell required)
```r
system2("kinarm-binary-converter",
        args = c("subject01.kinarm", "output/subject01"),
        stdout = TRUE, stderr = TRUE)
df <- read.csv("output/subject01_timeseries.csv")
```

### Use from Python
```python
import subprocess, json
subprocess.run(["kinarm-binary-converter", "subject01.kinarm", "output/subject01"])
with open("output/subject01.json") as f:
    data = json.load(f)
```

### Batch pipeline (bash)
```bash
#!/bin/bash
mkdir -p converted
for f in raw/*.kinarm; do
  name=$(basename "${f%.kinarm}")
  kinarm-binary-converter "$f" "converted/$name" --formats json,csv
  echo "Converted: $name"
done
```

---

## What's in a `.kinarm` file

A `.kinarm` file is a ZIP archive containing per-trial binary data in Dexterit-E format:

```
raw/
  common/          # Exam-level parameters
  02_01_01/        # Block 2, Trial 1, Repeat 1
    Right_Hand.position        # XY hand position (float32 pairs, ~1 kHz)
    Right_L1Ang.kinematics     # Shoulder joint angle (float32, ~1 kHz)
    Right_L2Ang.kinematics     # Elbow joint angle
    Right_HandXVel.kinematics  # Hand velocity X (if recorded)
    Right_HandYVel.kinematics  # Hand velocity Y (if recorded)
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
