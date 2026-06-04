// kinarm_converter.cpp
// CLI: kinarm-binary-converter <input.kinarm> <output_stem> [--formats json,csv,mat]
//
// Reads a .kinarm ZIP archive (Dexterit-E export) and emits any of:
//   <output_stem>.json             full structured JSON
//   <output_stem>_timeseries.csv   flat per-sample CSV (MATLAB/Python/R/Julia/Excel)
//   <output_stem>.mat              MATLAB v5 file matching exam_load() struct layout
//
// No MATLAB required. The .mat output uses the same struct field names as the
// official KINARM Analysis Scripts exam_load(), so existing MATLAB code works
// without changes.

#include "miniz.h"  // implementation in miniz.c (compiled as C, see CMakeLists)

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <string>
#include <vector>
#include <unordered_map>
#include <map>
#include <set>
#include <algorithm>
#include <fstream>
#include <sstream>

// ---------------------------------------------------------------------------
// Little-endian readers
// ---------------------------------------------------------------------------
static uint32_t read_u32_le(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static float read_f32_le(const uint8_t* p) {
    uint32_t u = read_u32_le(p);
    float f;
    std::memcpy(&f, &u, sizeof(f));
    return f;
}

// Reads a UTF-16LE string: uint32 char count prefix, then that many UTF-16
// code points. ASCII range only is sufficient for Kinarm names. Advances off.
static std::string decode_utf16le(const std::vector<uint8_t>& data, size_t& off) {
    if (off + 4 > data.size()) { off = data.size(); return std::string(); }
    uint32_t n = read_u32_le(&data[off]);
    off += 4;
    std::string out;
    out.reserve(n);
    for (uint32_t i = 0; i < n; ++i) {
        if (off + 2 > data.size()) { off = data.size(); break; }
        uint16_t cp = (uint16_t)data[off] | ((uint16_t)data[off + 1] << 8);
        off += 2;
        // ASCII / Latin-1 only
        out.push_back((cp < 256) ? (char)cp : '?');
    }
    return out;
}

// ---------------------------------------------------------------------------
// ZIP wrapper (miniz, in-memory)
// ---------------------------------------------------------------------------
struct ZipFile {
    mz_zip_archive zip;
    std::vector<uint8_t> buf;
    bool ok = false;

    ZipFile() { std::memset(&zip, 0, sizeof(zip)); }
    ~ZipFile() {
        if (ok) mz_zip_reader_end(&zip);
    }

    bool open(const std::string& path) {
        std::ifstream f(path, std::ios::binary);
        if (!f) {
            std::fprintf(stderr, "[error] cannot open file: %s\n", path.c_str());
            return false;
        }
        f.seekg(0, std::ios::end);
        std::streamoff sz = f.tellg();
        f.seekg(0, std::ios::beg);
        if (sz <= 0) {
            std::fprintf(stderr, "[error] empty file: %s\n", path.c_str());
            return false;
        }
        buf.resize((size_t)sz);
        f.read(reinterpret_cast<char*>(buf.data()), sz);
        if (!f) {
            std::fprintf(stderr, "[error] short read on: %s\n", path.c_str());
            return false;
        }
        std::memset(&zip, 0, sizeof(zip));
        if (!mz_zip_reader_init_mem(&zip, buf.data(), buf.size(), 0)) {
            std::fprintf(stderr, "[error] not a valid ZIP archive: %s\n", path.c_str());
            return false;
        }
        ok = true;
        return true;
    }

    // Returns the bytes of an entry, empty vector if not found.
    std::vector<uint8_t> read(const std::string& entry) {
        std::vector<uint8_t> out;
        if (!ok) return out;
        int idx = mz_zip_reader_locate_file(&zip, entry.c_str(), nullptr, 0);
        if (idx < 0) return out;
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip, idx, &st)) return out;
        out.resize((size_t)st.m_uncomp_size);
        if (st.m_uncomp_size > 0) {
            if (!mz_zip_reader_extract_to_mem(&zip, idx, out.data(), out.size(), 0)) {
                out.clear();
            }
        }
        return out;
    }

    bool has(const std::string& entry) {
        if (!ok) return false;
        return mz_zip_reader_locate_file(&zip, entry.c_str(), nullptr, 0) >= 0;
    }

    // All entry names that start with prefix.
    std::vector<std::string> list_entries(const std::string& prefix) {
        std::vector<std::string> names;
        if (!ok) return names;
        mz_uint n = mz_zip_reader_get_num_files(&zip);
        for (mz_uint i = 0; i < n; ++i) {
            mz_zip_archive_file_stat st;
            if (!mz_zip_reader_file_stat(&zip, i, &st)) continue;
            std::string name = st.m_filename;
            if (name.size() >= prefix.size() &&
                name.compare(0, prefix.size(), prefix) == 0) {
                names.push_back(name);
            }
        }
        return names;
    }
};

// ---------------------------------------------------------------------------
// exam_info_*.txt — Java properties parser
// ---------------------------------------------------------------------------
static std::unordered_map<std::string, std::string>
parse_exam_info(const std::vector<uint8_t>& data) {
    std::unordered_map<std::string, std::string> kv;
    std::string text(data.begin(), data.end());
    std::istringstream ss(text);
    std::string line;
    while (std::getline(ss, line)) {
        // strip trailing \r
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        if (line[0] == '#' || line[0] == '!') continue; // comment
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        // trim spaces
        auto trim = [](std::string& s) {
            size_t a = s.find_first_not_of(" \t");
            size_t b = s.find_last_not_of(" \t");
            if (a == std::string::npos) { s.clear(); return; }
            s = s.substr(a, b - a + 1);
        };
        trim(key);
        trim(val);
        kv[key] = val;
    }
    return kv;
}

// ---------------------------------------------------------------------------
// examevents.bin
// ---------------------------------------------------------------------------
struct Event {
    std::string name;
    float time_s;
};

static std::vector<Event> parse_examevents(const std::vector<uint8_t>& data) {
    std::vector<Event> events;
    if (data.size() < 8) return events;
    size_t off = 0;
    /* version  = */ read_u32_le(&data[off]); off += 4;
    uint32_t n_events = read_u32_le(&data[off]); off += 4;

    for (uint32_t e = 0; e < n_events; ++e) {
        if (off + 4 > data.size()) break;
        std::string name = decode_utf16le(data, off);

        // Duplicate-name detection: peek next uint32. If it is a plausible
        // char count (< 200) and there is room for that many UTF-16 chars,
        // it is a duplicate copy of the name — read and discard it.
        if (off + 4 <= data.size()) {
            uint32_t peek = read_u32_le(&data[off]);
            if (peek < 200 && off + 4 + (size_t)peek * 2 <= data.size()) {
                size_t tmp = off;
                decode_utf16le(data, tmp); // discard duplicate
                off = tmp;
            }
        }

        if (off + 4 > data.size()) break;
        float t = read_f32_le(&data[off]); off += 4;
        events.push_back({name, t});
    }
    return events;
}

// ---------------------------------------------------------------------------
// *.position  — interleaved [x,y] float pairs in meters
// ---------------------------------------------------------------------------
struct XYSeries {
    std::vector<float> x;
    std::vector<float> y;
    uint32_t sample_count = 0;
};

static XYSeries parse_position(const std::vector<uint8_t>& data) {
    XYSeries s;
    if (data.size() < 12) return s;
    size_t off = 0;
    /* version    = */ read_u32_le(&data[off]); off += 4;
    /* n_channels = */ read_u32_le(&data[off]); off += 4; // actually channel-name char count
    // The second uint32 doubles as the char count of the channel-name string.
    // Re-read it as a UTF-16LE string starting at the n_channels field.
    off -= 4;
    std::string chan = decode_utf16le(data, off);
    (void)chan;

    // Robustly locate sample_count: the value sc for which the remaining bytes
    // equal sc * 8 (two float32 per sample). Extra UTF-16 description/units
    // strings after the channel name can leave the tag mis-aligned, so scan
    // every byte offset over a generous window.
    uint32_t sample_count = 0;
    size_t data_off = 0;
    size_t scan_start = (off < data.size()) ? off : data.size();
    size_t scan_limit = std::min(data.size(), scan_start + 1024);
    for (size_t probe = scan_start; probe + 4 <= scan_limit; ++probe) {
        uint32_t sc = read_u32_le(&data[probe]);
        size_t remain = data.size() - (probe + 4);
        if (sc > 0 && (size_t)sc * 8 == remain) {
            sample_count = sc;
            data_off = probe + 4;
            break;
        }
    }

    if (sample_count == 0) return s;

    s.sample_count = sample_count;
    s.x.reserve(sample_count);
    s.y.reserve(sample_count);
    for (uint32_t i = 0; i < sample_count; ++i) {
        size_t p = data_off + (size_t)i * 8;
        if (p + 8 > data.size()) break;
        s.x.push_back(read_f32_le(&data[p]));
        s.y.push_back(read_f32_le(&data[p + 4]));
    }
    return s;
}

// ---------------------------------------------------------------------------
// *.kinematics  — single-channel float32 array
// ---------------------------------------------------------------------------
static std::vector<float> parse_kinematics(const std::vector<uint8_t>& data) {
    std::vector<float> out;
    if (data.size() < 12) return out;
    size_t off = 0;
    /* version    = */ read_u32_le(&data[off]); off += 4;
    /* n_channels = */ read_u32_le(&data[off]); off += 4; // doubles as channel-name char count
    off -= 4;
    std::string chan = decode_utf16le(data, off);
    (void)chan;

    // Robustly locate the sample_count: the value sc for which the remaining
    // bytes equal sc * 4 (one float32 per sample). Many kinematics channels
    // carry one or more extra UTF-16 strings (description, units) after the
    // channel name; their byte lengths are odd, so the sample_count tag is NOT
    // 4-byte aligned. Scan EVERY byte offset over a generous window to find it.
    uint32_t sample_count = 0;
    size_t data_off = 0;
    size_t scan_start = (off < data.size()) ? off : data.size();
    size_t scan_limit = std::min(data.size(), scan_start + 1024);
    for (size_t probe = scan_start; probe + 4 <= scan_limit; ++probe) {
        uint32_t sc = read_u32_le(&data[probe]);
        size_t remain = data.size() - (probe + 4);
        if (sc > 0 && (size_t)sc * 4 == remain) {
            sample_count = sc;
            data_off = probe + 4;
            break;
        }
    }

    if (sample_count == 0) return out;

    out.reserve(sample_count);
    for (uint32_t i = 0; i < sample_count; ++i) {
        size_t p = data_off + (size_t)i * 4;
        if (p + 4 > data.size()) break;
        out.push_back(read_f32_le(&data[p]));
    }
    return out;
}

// ---------------------------------------------------------------------------
// Trial-ID extraction from raw/ entries
// ---------------------------------------------------------------------------
static std::vector<std::string> extract_trial_ids(ZipFile& zf) {
    std::set<std::string> ids;
    auto entries = zf.list_entries("raw/");
    for (const auto& e : entries) {
        // e looks like "raw/02_01_01/Right_Hand.position"
        size_t p1 = e.find('/');                  // after "raw"
        if (p1 == std::string::npos) continue;
        size_t p2 = e.find('/', p1 + 1);          // after the trial-id component
        std::string comp;
        if (p2 == std::string::npos) {
            comp = e.substr(p1 + 1);
        } else {
            comp = e.substr(p1 + 1, p2 - (p1 + 1));
        }
        if (comp.empty()) continue;
        if (comp == "common") continue;
        if (comp.find('_') == std::string::npos) continue;
        ids.insert(comp);
    }
    return std::vector<std::string>(ids.begin(), ids.end()); // set => sorted+unique
}

// ---------------------------------------------------------------------------
// JSON helpers
// ---------------------------------------------------------------------------
static std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:   out += c;      break;
        }
    }
    return out;
}

static std::string floats_to_json(const std::vector<float>& v) {
    std::string out = "[";
    char buf[64];
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) out += ",";
        std::snprintf(buf, sizeof(buf), "%.8g", (double)v[i]);
        out += buf;
    }
    out += "]";
    return out;
}

// ---------------------------------------------------------------------------
// Per-trial in-memory model
// ---------------------------------------------------------------------------
struct TrialData {
    std::string trial_id;
    int block = 0, trial = 0, repeat = 0;
    int sample_rate = 1000;
    bool home_valid = false;
    double home_start_s = 0.0;
    double home_end_s = 0.0;
    std::vector<Event> events;

    std::vector<float> full_t, full_x, full_y;
    std::vector<float> home_t, home_x, home_y;
    std::vector<float> home_vx, home_vy;
    bool has_vel = false;
};

// ===========================================================================
// MATLAB v5 .mat writer
// ===========================================================================
// Reference: MATLAB MAT-File Format, The MathWorks (publicly available spec).
// We write MATLAB v5 (level 5) uncompressed format — readable by all MATLAB
// versions and scipy.io.loadmat.

// miTYPE constants
static const uint32_t miINT8     = 1;
static const uint32_t miUINT16   = 4;
static const uint32_t miINT32    = 5;
static const uint32_t miUINT32   = 6;
static const uint32_t miDOUBLE   = 9;
static const uint32_t miMATRIX   = 14;

// mxCLASS constants (array class stored in flags byte)
static const uint8_t mxCELL_CLASS   = 1;
static const uint8_t mxSTRUCT_CLASS = 2;
static const uint8_t mxCHAR_CLASS   = 4;
static const uint8_t mxDOUBLE_CLASS = 6;

struct MatBuf {
    std::vector<uint8_t> data;

    void write_u32_le(uint32_t v) {
        for (int i = 0; i < 4; ++i) { data.push_back((v >> (8*i)) & 0xff); }
    }
    void write_i32_le(int32_t v) { write_u32_le((uint32_t)v); }
    void write_bytes(const uint8_t* p, size_t n) {
        data.insert(data.end(), p, p + n);
    }
    void pad_to_8() {
        while (data.size() % 8 != 0) data.push_back(0);
    }
    size_t size() const { return data.size(); }

    // Write a MAT data sub-element tag + data, padded to 8-byte boundary.
    void write_element(uint32_t dtype, const void* bytes, uint32_t nbytes) {
        write_u32_le(dtype);
        write_u32_le(nbytes);
        if (nbytes) write_bytes((const uint8_t*)bytes, nbytes);
        uint32_t pad = (8 - (nbytes % 8)) % 8;
        for (uint32_t i = 0; i < pad; ++i) data.push_back(0);
    }
};

// Append a complete miMATRIX element (the body built in `inner`) to `out`.
static void append_matrix(MatBuf& out, const MatBuf& inner) {
    out.write_u32_le(miMATRIX);
    out.write_u32_le((uint32_t)inner.size());
    out.write_bytes(inner.data.data(), inner.size());
    out.pad_to_8();
}

// Build the array-flags + dims + name preamble shared by every miMATRIX.
static void mat_preamble(MatBuf& inner, uint8_t mx_class,
                         int32_t nrows, int32_t ncols,
                         const std::string& name) {
    uint32_t flags[2] = { (uint32_t)mx_class, 0 };
    inner.write_element(miUINT32, flags, 8);
    int32_t dims[2] = { nrows, ncols };
    inner.write_element(miINT32, dims, 8);
    inner.write_element(miINT8, name.data(), (uint32_t)name.size());
}

// char array (string) miMATRIX. name is the variable name ("" inside a struct).
static void buf_string_field(MatBuf& out, const std::string& str,
                             const std::string& name = "") {
    MatBuf inner;
    mat_preamble(inner, mxCHAR_CLASS, 1, (int32_t)str.size(), name);
    std::vector<uint16_t> chars(str.begin(), str.end());
    inner.write_element(miUINT16, chars.data(), (uint32_t)(chars.size() * 2));
    append_matrix(out, inner);
}

// double column vector (Nx1) miMATRIX.
static void buf_double_col(MatBuf& out, const std::vector<double>& vals,
                           const std::string& name = "") {
    MatBuf inner;
    if (vals.empty()) {
        mat_preamble(inner, mxDOUBLE_CLASS, 0, 0, name);
    } else {
        mat_preamble(inner, mxDOUBLE_CLASS, (int32_t)vals.size(), 1, name);
    }
    // Always emit the data (pr) element — MATLAB/scipy require it even when
    // the array is empty (an empty 0x0 still gets a 0-byte DOUBLE element).
    inner.write_element(miDOUBLE, vals.data(), (uint32_t)(vals.size() * 8));
    append_matrix(out, inner);
}

// double row vector (1xN) miMATRIX.
static void buf_double_row(MatBuf& out, const std::vector<double>& vals,
                           const std::string& name = "") {
    MatBuf inner;
    if (vals.empty()) {
        mat_preamble(inner, mxDOUBLE_CLASS, 0, 0, name);
    } else {
        mat_preamble(inner, mxDOUBLE_CLASS, 1, (int32_t)vals.size(), name);
    }
    inner.write_element(miDOUBLE, vals.data(), (uint32_t)(vals.size() * 8));
    append_matrix(out, inner);
}

static void buf_double_scalar(MatBuf& out, double val,
                              const std::string& name = "") {
    buf_double_col(out, std::vector<double>{ val }, name);
}

// Round field-name length up to a multiple of 8 (MATLAB convention here).
static int32_t struct_fname_len(const std::vector<std::string>& fields) {
    int m = 0;
    for (const auto& s : fields) m = std::max(m, (int)s.size());
    int len = m + 1; // +1 for null terminator
    if (len % 8 != 0) len += 8 - (len % 8);
    return len;
}

// Write the struct field-name-length + field-names block into `out`.
static void buf_field_names(MatBuf& out, const std::vector<std::string>& fields,
                            int32_t fname_len) {
    out.write_u32_le(miINT32);
    out.write_u32_le(4);
    out.write_i32_le(fname_len);
    out.pad_to_8();
    std::vector<uint8_t> fnames_data;
    for (const auto& fn : fields) {
        for (char c : fn) fnames_data.push_back((uint8_t)c);
        for (int i = (int)fn.size(); i < fname_len; ++i) fnames_data.push_back(0);
    }
    out.write_element(miINT8, fnames_data.data(), (uint32_t)fnames_data.size());
}

// Write the full .mat file.
// all_channels[channel_name][trial_index] = float vector.
static void write_mat(const std::string& path,
                      const std::string& kinarm_filename,
                      const std::vector<TrialData>& trials,
                      const std::map<std::string, std::vector<std::vector<float>>>& all_channels) {
    // Sorted list of all channel field names present across all trials.
    std::vector<std::string> chan_fields;
    for (const auto& kv : all_channels) chan_fields.push_back(kv.first);
    std::sort(chan_fields.begin(), chan_fields.end());

    // c3d struct field names — match exam_load() output field names.
    std::vector<std::string> c3d_fields;
    c3d_fields.push_back("FILE_NAME");
    for (const auto& f : chan_fields) c3d_fields.push_back(f);
    c3d_fields.push_back("ANALOG");
    c3d_fields.push_back("EVENTS");

    std::vector<std::string> data_fields  = { "filename", "c3d" };
    std::vector<std::string> analog_fields = { "RATE" };
    std::vector<std::string> events_fields = { "LABELS", "TIMES" };

    int32_t data_fname_len   = struct_fname_len(data_fields);
    int32_t c3d_fname_len    = struct_fname_len(c3d_fields);
    int32_t analog_fname_len = struct_fname_len(analog_fields);
    int32_t events_fname_len = struct_fname_len(events_fields);

    std::ofstream f(path, std::ios::binary);
    if (!f) {
        std::fprintf(stderr, "[error] cannot write MAT: %s\n", path.c_str());
        return;
    }

    // ---- File header (128 bytes) ----
    {
        std::string desc = "MATLAB 5.0 MAT-file, created by kinarm-binary-converter";
        desc.resize(116, ' ');  // pad description to 116 bytes
        f.write(desc.data(), 116);
        for (int i = 0; i < 8; ++i) f.put(0);  // subsystem data offset
        f.put(0x00); f.put(0x01);              // version = 0x0100 (LE bytes 00 01)
        // Endian indicator: on a little-endian writer the physical bytes are
        // 'I','M' so that reading them as a uint16 yields 0x4D49 = "MI".
        // scipy.io.loadmat keys off byte 126 == 'I' to locate the major
        // version byte — writing 'M','I' here makes it misread the version.
        f.put('I'); f.put('M');
    }

    // =====================================================================
    // Build the top-level 'data' miMATRIX (1x1 struct).
    // =====================================================================
    MatBuf data_mat;
    mat_preamble(data_mat, mxSTRUCT_CLASS, 1, 1, "data");
    buf_field_names(data_mat, data_fields, data_fname_len);

    // ---- field 0: filename (string) ----
    buf_string_field(data_mat, kinarm_filename);

    // ---- field 1: c3d (1xN struct array) ----
    {
        MatBuf c3d_mat;
        uint32_t n_trials = (uint32_t)trials.size();
        mat_preamble(c3d_mat, mxSTRUCT_CLASS, 1, (int32_t)n_trials, "");
        buf_field_names(c3d_mat, c3d_fields, c3d_fname_len);

        // MATLAB stores struct-array contents ELEMENT-MAJOR: for each element
        // (trial), write the value of every field in field order, then move to
        // the next element. Field order = c3d_fields:
        //   FILE_NAME, <channels...>, ANALOG, EVENTS
        for (size_t ti = 0; ti < trials.size(); ++ti) {
            const TrialData& td = trials[ti];

            // FILE_NAME (char)
            buf_string_field(c3d_mat, td.trial_id);

            // Channel fields (double column vector each)
            for (const auto& chan : chan_fields) {
                const auto& per_trial = all_channels.at(chan);
                if (ti < per_trial.size() && !per_trial[ti].empty()) {
                    std::vector<double> dv(per_trial[ti].begin(), per_trial[ti].end());
                    buf_double_col(c3d_mat, dv);
                } else {
                    buf_double_col(c3d_mat, std::vector<double>{});  // empty 0x0
                }
            }

            // ANALOG (1x1 struct with RATE field)
            {
                MatBuf analog_mat;
                mat_preamble(analog_mat, mxSTRUCT_CLASS, 1, 1, "");
                buf_field_names(analog_mat, analog_fields, analog_fname_len);
                buf_double_scalar(analog_mat, (double)td.sample_rate);
                append_matrix(c3d_mat, analog_mat);
            }

            // EVENTS (1x1 struct with LABELS cell + TIMES row)
            {
                MatBuf events_mat;
                mat_preamble(events_mat, mxSTRUCT_CLASS, 1, 1, "");
                buf_field_names(events_mat, events_fields, events_fname_len);

                // LABELS: 1xN cell array of char arrays.
                {
                    MatBuf labels_mat;
                    mat_preamble(labels_mat, mxCELL_CLASS, 1, (int32_t)td.events.size(), "");
                    for (const auto& ev : td.events) buf_string_field(labels_mat, ev.name);
                    append_matrix(events_mat, labels_mat);
                }

                // TIMES: 1xN double row vector.
                {
                    std::vector<double> times;
                    times.reserve(td.events.size());
                    for (const auto& ev : td.events) times.push_back((double)ev.time_s);
                    buf_double_row(events_mat, times);
                }

                append_matrix(c3d_mat, events_mat);
            }
        }

        append_matrix(data_mat, c3d_mat);
    }

    // Write the top-level 'data' element.
    {
        uint32_t tag[2] = { miMATRIX, (uint32_t)data_mat.size() };
        uint8_t tagb[8];
        for (int i = 0; i < 4; ++i) tagb[i]   = (tag[0] >> (8*i)) & 0xff;
        for (int i = 0; i < 4; ++i) tagb[4+i] = (tag[1] >> (8*i)) & 0xff;
        f.write((const char*)tagb, 8);
        f.write((const char*)data_mat.data.data(), data_mat.size());
        // top-level elements are already padded to 8 inside data_mat building,
        // but ensure the stream itself is 8-aligned.
        std::streamoff pos = f.tellp();
        int rem = (int)(pos % 8);
        if (rem) for (int i = 0; i < 8 - rem; ++i) f.put(0);
    }

    f.close();
    std::fprintf(stderr, "[info] wrote %s\n", path.c_str());
}

// ---------------------------------------------------------------------------
// Read ALL kinematics + position channels for one trial folder.
// Splits .position into <stem>X / <stem>Y; .kinematics stored under <stem>.
// ---------------------------------------------------------------------------
static void read_all_channels_for_trial(
        ZipFile& zf, const std::string& tid, size_t ti, size_t n_trials,
        std::map<std::string, std::vector<std::vector<float>>>& all_channels) {
    std::string base = "raw/" + tid + "/";
    auto entries = zf.list_entries(base);
    auto ensure = [&](const std::string& key) -> std::vector<std::vector<float>>& {
        auto& v = all_channels[key];
        if (v.size() < n_trials) v.resize(n_trials);
        return v;
    };
    for (const auto& entry : entries) {
        std::string fname = entry.substr(base.size());
        if (fname.empty() || fname.find('/') != std::string::npos) continue;

        if (fname.size() > 9 && fname.compare(fname.size() - 9, 9, ".position") == 0) {
            std::string stem = fname.substr(0, fname.size() - 9);
            XYSeries xy = parse_position(zf.read(entry));
            ensure(stem + "X")[ti] = xy.x;
            ensure(stem + "Y")[ti] = xy.y;
        } else if (fname.size() > 11 && fname.compare(fname.size() - 11, 11, ".kinematics") == 0) {
            std::string stem = fname.substr(0, fname.size() - 11);
            // Skip non-data status/timestamp channels.
            if (stem == "StatusBits" ||
                stem.find("TimeStamp") != std::string::npos ||
                stem.find("Status") != std::string::npos) continue;
            ensure(stem)[ti] = parse_kinematics(zf.read(entry));
        }
    }
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr,
            "usage: %s <input.kinarm> <output_stem> [--formats json,csv,mat]\n",
            argv[0]);
        return 2;
    }
    std::string input = argv[1];
    std::string stem  = argv[2];

    // ---- parse --formats ---------------------------------------------------
    bool want_json = true, want_csv = true, want_mat = true;
    for (int i = 3; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--formats" && i + 1 < argc) {
            want_json = want_csv = want_mat = false;
            std::string list = argv[++i];
            std::stringstream ss(list);
            std::string tok;
            while (std::getline(ss, tok, ',')) {
                // trim + lowercase
                size_t s0 = tok.find_first_not_of(" \t");
                size_t s1 = tok.find_last_not_of(" \t");
                if (s0 == std::string::npos) continue;
                tok = tok.substr(s0, s1 - s0 + 1);
                for (auto& c : tok) c = (char)std::tolower((unsigned char)c);
                if (tok == "json") want_json = true;
                else if (tok == "csv") want_csv = true;
                else if (tok == "mat") want_mat = true;
                else std::fprintf(stderr, "[warn] unknown format '%s' (ignored)\n", tok.c_str());
            }
            if (!want_json && !want_csv && !want_mat) {
                std::fprintf(stderr, "[error] --formats produced no valid formats\n");
                return 2;
            }
        } else {
            std::fprintf(stderr, "[warn] unrecognized argument '%s' (ignored)\n", a.c_str());
        }
    }

    std::fprintf(stderr, "[info] opening %s\n", input.c_str());
    ZipFile zf;
    if (!zf.open(input)) return 1;

    // ---- metadata (exam_info_5 preferred, fall back to exam_info_3) --------
    std::unordered_map<std::string, std::string> info;
    {
        auto d = zf.read("exam_info_5.txt");
        if (d.empty()) d = zf.read("exam_info_3.txt");
        if (!d.empty()) info = parse_exam_info(d);
    }
    auto get = [&](const char* k) -> std::string {
        auto it = info.find(k);
        return it == info.end() ? std::string() : it->second;
    };
    std::string subject_id = get("model");
    std::string protocol   = get("protocol");
    std::string dex_ver    = get("dex_ver");
    std::string robot_arm  = get("robot_arm");
    std::string operatorv  = get("operator");
    std::string posture    = get("posture");
    std::string robotver   = get("robotver");

    // arm side -> channel base name (capitalize first letter)
    std::string arm = robot_arm.empty() ? "right" : robot_arm;
    std::string arm_cap = arm;
    if (!arm_cap.empty()) arm_cap[0] = (char)std::toupper((unsigned char)arm_cap[0]);
    std::string pos_file = arm_cap + "_Hand.position";
    std::string vx_file  = arm_cap + "_HandXVel.kinematics";
    std::string vy_file  = arm_cap + "_HandYVel.kinematics";

    // Detect calibration mode: protocol field contains "calibration".
    std::string protocol_lower = protocol;
    for (auto& c : protocol_lower) c = (char)std::tolower((unsigned char)c);
    bool is_calibration = (protocol_lower.find("calibration") != std::string::npos);
    std::string protocol_mode = is_calibration ? "calibration" : "task";

    std::fprintf(stderr, "[info] subject=%s protocol=%s mode=%s arm=%s\n",
                 subject_id.c_str(), protocol.c_str(), protocol_mode.c_str(), arm.c_str());

    // ---- trials ------------------------------------------------------------
    auto trial_ids = extract_trial_ids(zf);
    std::fprintf(stderr, "[info] found %zu trial(s)\n", trial_ids.size());

    std::vector<TrialData> trials;
    trials.reserve(trial_ids.size());

    // all_channels[field_name][trial_index] = float vector (for the MAT writer)
    std::map<std::string, std::vector<std::vector<float>>> all_channels;

    for (size_t ti = 0; ti < trial_ids.size(); ++ti) {
        const std::string& tid = trial_ids[ti];
        TrialData td;
        td.trial_id = tid;
        // parse block/trial/repeat from "BB_TT_RR"
        {
            int b = 0, t = 0, r = 0;
            std::sscanf(tid.c_str(), "%d_%d_%d", &b, &t, &r);
            td.block = b; td.trial = t; td.repeat = r;
        }
        std::string base = "raw/" + tid + "/";

        // events
        auto ev_data = zf.read(base + "examevents.bin");
        if (!ev_data.empty()) td.events = parse_examevents(ev_data);

        // home window depends on protocol mode (see existing logic).
        bool have_t2_onset = false, have_in2 = false, have_wc = false;
        double t2_onset = 0.0, in2_first = 0.0, wc_last = 0.0;
        for (const auto& e : td.events) {
            if (e.name == "TARGET2_ONSET" && !have_t2_onset) {
                t2_onset = e.time_s; have_t2_onset = true;
            } else if (e.name == "IN_TARGET2" && !have_in2) {
                in2_first = e.time_s; have_in2 = true;
            } else if (e.name == "WAIT_CORRECT") {
                wc_last = e.time_s; have_wc = true;
            }
        }
        if (is_calibration) {
            td.home_valid = have_t2_onset;
            if (td.home_valid) {
                td.home_start_s = t2_onset;
                td.home_end_s   = 1e99; // sentinel: replaced with last sample time below
            }
        } else {
            td.home_valid = have_in2 && have_wc;
            if (td.home_valid) {
                td.home_start_s = in2_first;
                td.home_end_s   = wc_last;
            }
        }

        // position (primary hand, used by JSON/CSV windowing)
        auto pos_data = zf.read(base + pos_file);
        XYSeries pos = parse_position(pos_data);
        td.full_x = pos.x;
        td.full_y = pos.y;
        td.full_t.reserve(pos.sample_count);
        for (uint32_t i = 0; i < pos.sample_count; ++i) {
            td.full_t.push_back((float)((double)i / (double)td.sample_rate));
        }

        if (is_calibration && td.home_valid && td.home_end_s > 1e90) {
            td.home_end_s = td.full_t.empty() ? td.home_start_s
                                               : (double)td.full_t.back();
        }

        // velocity (optional)
        std::vector<float> vx_all, vy_all;
        if (zf.has(base + vx_file) && zf.has(base + vy_file)) {
            vx_all = parse_kinematics(zf.read(base + vx_file));
            vy_all = parse_kinematics(zf.read(base + vy_file));
            td.has_vel = !vx_all.empty() && !vy_all.empty();
        }

        // home-windowed slices
        if (td.home_valid) {
            for (size_t i = 0; i < td.full_t.size(); ++i) {
                double ts = td.full_t[i];
                if (ts >= td.home_start_s && ts <= td.home_end_s) {
                    td.home_t.push_back(td.full_t[i]);
                    td.home_x.push_back(td.full_x[i]);
                    td.home_y.push_back(td.full_y[i]);
                    if (td.has_vel && i < vx_all.size() && i < vy_all.size()) {
                        td.home_vx.push_back(vx_all[i]);
                        td.home_vy.push_back(vy_all[i]);
                    }
                }
            }
        }

        // ALL channels for this trial (for the MAT output).
        if (want_mat) {
            read_all_channels_for_trial(zf, tid, ti, trial_ids.size(), all_channels);
        }

        std::fprintf(stderr,
            "[info] trial %s: %zu samples, home_valid=%d, home=[%.4f,%.4f], home_samples=%zu\n",
            tid.c_str(), td.full_t.size(), (int)td.home_valid,
            td.home_start_s, td.home_end_s, td.home_t.size());

        trials.push_back(std::move(td));
    }

    // ---- write CSV ---------------------------------------------------------
    if (want_csv) {
        std::string csv_path = stem + "_timeseries.csv";
        std::ofstream f(csv_path, std::ios::binary);
        if (!f) {
            std::fprintf(stderr, "[error] cannot write CSV: %s\n", csv_path.c_str());
            return 1;
        }
        f << "trial_id,block,trial,repeat,time_s,x,y,vx,vy,in_home_window\n";
        char nb[64];
        for (const auto& t : trials) {
            size_t home_cursor = 0;
            for (size_t i = 0; i < t.full_t.size(); ++i) {
                double ts = t.full_t[i];
                int in_home = (t.home_valid && ts >= t.home_start_s && ts <= t.home_end_s) ? 1 : 0;
                f << t.trial_id << ',' << t.block << ',' << t.trial << ',' << t.repeat << ',';
                std::snprintf(nb, sizeof(nb), "%.6g", ts); f << nb << ',';
                std::snprintf(nb, sizeof(nb), "%.8g", (double)t.full_x[i]); f << nb << ',';
                std::snprintf(nb, sizeof(nb), "%.8g", (double)t.full_y[i]); f << nb << ',';
                if (in_home && t.has_vel &&
                    home_cursor < t.home_vx.size() && home_cursor < t.home_vy.size()) {
                    std::snprintf(nb, sizeof(nb), "%.8g", (double)t.home_vx[home_cursor]); f << nb << ',';
                    std::snprintf(nb, sizeof(nb), "%.8g", (double)t.home_vy[home_cursor]); f << nb << ',';
                } else {
                    f << ',' << ',';
                }
                if (in_home) home_cursor++;
                f << in_home << '\n';
            }
        }
        std::fprintf(stderr, "[info] wrote %s\n", csv_path.c_str());
    }

    // ---- write JSON --------------------------------------------------------
    if (want_json) {
        std::string json_path = stem + ".json";
        std::ofstream f(json_path, std::ios::binary);
        if (!f) {
            std::fprintf(stderr, "[error] cannot write JSON: %s\n", json_path.c_str());
            return 1;
        }
        f << "{\n";
        f << "  \"metadata\": {";
        f << "\"subject_id\":\"" << json_escape(subject_id) << "\",";
        f << "\"protocol\":\""   << json_escape(protocol)   << "\",";
        f << "\"dex_ver\":\""    << json_escape(dex_ver)    << "\",";
        f << "\"robot_arm\":\""  << json_escape(robot_arm)  << "\",";
        f << "\"operator\":\""   << json_escape(operatorv)  << "\",";
        f << "\"posture\":\""    << json_escape(posture)    << "\",";
        f << "\"robotver\":\""      << json_escape(robotver)      << "\",";
        f << "\"protocol_mode\":\"" << json_escape(protocol_mode) << "\"";
        f << "},\n";

        f << "  \"trials\": [\n";
        char nb[64];
        for (size_t ti = 0; ti < trials.size(); ++ti) {
            const auto& t = trials[ti];
            f << "    {\n";
            f << "      \"trial_id\": \"" << json_escape(t.trial_id) << "\",\n";
            f << "      \"block\": " << t.block << ", \"trial\": " << t.trial
              << ", \"repeat\": " << t.repeat << ",\n";
            f << "      \"sample_rate\": " << t.sample_rate << ",\n";
            if (t.home_valid) {
                std::snprintf(nb, sizeof(nb), "%.6g", t.home_start_s);
                f << "      \"home_start_s\": " << nb << ",\n";
                std::snprintf(nb, sizeof(nb), "%.6g", t.home_end_s);
                f << "      \"home_end_s\": " << nb << ",\n";
            } else {
                f << "      \"home_start_s\": null,\n";
                f << "      \"home_end_s\": null,\n";
            }
            f << "      \"home_valid\": " << (t.home_valid ? "true" : "false") << ",\n";

            f << "      \"events\": [";
            for (size_t ei = 0; ei < t.events.size(); ++ei) {
                if (ei) f << ", ";
                std::snprintf(nb, sizeof(nb), "%.6g", (double)t.events[ei].time_s);
                f << "{\"name\":\"" << json_escape(t.events[ei].name)
                  << "\",\"time_s\":" << nb << "}";
            }
            f << "],\n";

            f << "      \"hand_full\": {\"time_s\":" << floats_to_json(t.full_t)
              << ",\"x\":" << floats_to_json(t.full_x)
              << ",\"y\":" << floats_to_json(t.full_y) << "},\n";

            f << "      \"hand_home\": {\"time_s\":" << floats_to_json(t.home_t)
              << ",\"x\":" << floats_to_json(t.home_x)
              << ",\"y\":" << floats_to_json(t.home_y) << "},\n";

            f << "      \"vel_home\": {\"vx\":" << floats_to_json(t.home_vx)
              << ",\"vy\":" << floats_to_json(t.home_vy) << "}\n";

            f << "    }" << (ti + 1 < trials.size() ? "," : "") << "\n";
        }
        f << "  ]\n";
        f << "}\n";
        std::fprintf(stderr, "[info] wrote %s\n", json_path.c_str());
    }

    // ---- write MAT ---------------------------------------------------------
    if (want_mat) {
        // basename of input for the data.filename field
        std::string fname = input;
        size_t slash = fname.find_last_of("/\\");
        if (slash != std::string::npos) fname = fname.substr(slash + 1);
        write_mat(stem + ".mat", fname, trials, all_channels);
    }

    return 0;
}
