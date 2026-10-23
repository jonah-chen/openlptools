#include "processing/sequence.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <fstream>
#include <sstream>
#include <system_error>

#include "net/meter_client.h"
#include "io/xmp_settings.h"

namespace fs = std::filesystem;

namespace {

std::string Utf8(const fs::path& path) {
    const std::u8string s = path.u8string();
    return {reinterpret_cast<const char*>(s.data()), s.size()};
}

double Median(std::vector<double>& v) {
    const size_t mid = v.size() / 2;
    std::nth_element(v.begin(), v.begin() + static_cast<ptrdiff_t>(mid), v.end());
    return v[mid];
}

// Robust spread of v around center: 1.4826 * median absolute deviation, which
// equals the standard deviation for normally distributed values.
double Spread(std::vector<double> v, double center) {
    for (double& x : v) {
        x = std::abs(x - center);
    }
    return 1.4826 * Median(v);
}

// Least-squares polynomial fit of y(x), with two passes that down-weight outliers.
// Returns the fitted value at each x.
std::vector<double> FitPolynomial(const std::vector<double>& x, const std::vector<double>& y, int degree) {
    const size_t n = x.size();
    degree = std::clamp<int>(degree, 0, static_cast<int>(n) - 1);
    const int terms = degree + 1;

    // Normalize x to [-1, 1] so the normal equations stay well conditioned.
    const auto [xmin, xmax] = std::ranges::minmax(x);
    const double scale = xmax > xmin ? 2.0 / (xmax - xmin) : 1.0;
    auto normalized = [&](double v) { return (v - xmin) * scale - 1.0; };

    std::vector<double> weights(n, 1.0);
    std::vector<double> fitted(n, 0.0);
    for (int pass = 0; pass < 3; ++pass) {
        // Build and solve the weighted normal equations by Gaussian elimination.
        std::vector<double> ata(static_cast<size_t>(terms) * terms, 0.0);
        std::vector<double> atb(terms, 0.0);
        std::vector<double> powers(terms);
        for (size_t i = 0; i < n; ++i) {
            const double xn = normalized(x[i]);
            powers[0] = 1.0;
            for (int k = 1; k < terms; ++k) {
                powers[k] = powers[k - 1] * xn;
            }
            for (int r = 0; r < terms; ++r) {
                atb[r] += weights[i] * powers[r] * y[i];
                for (int c = 0; c < terms; ++c) {
                    ata[static_cast<size_t>(r) * terms + c] += weights[i] * powers[r] * powers[c];
                }
            }
        }
        std::vector<double> coef(terms, 0.0);
        for (int col = 0; col < terms; ++col) {
            int pivot = col;
            for (int r = col + 1; r < terms; ++r) {
                if (std::abs(ata[static_cast<size_t>(r) * terms + col]) >
                    std::abs(ata[static_cast<size_t>(pivot) * terms + col])) {
                    pivot = r;
                }
            }
            if (std::abs(ata[static_cast<size_t>(pivot) * terms + col]) < 1e-12) {
                continue;
            }
            for (int c = 0; c < terms; ++c) {
                std::swap(ata[static_cast<size_t>(col) * terms + c], ata[static_cast<size_t>(pivot) * terms + c]);
            }
            std::swap(atb[col], atb[pivot]);
            for (int r = 0; r < terms; ++r) {
                if (r == col) {
                    continue;
                }
                const double f = ata[static_cast<size_t>(r) * terms + col] / ata[static_cast<size_t>(col) * terms + col];
                for (int c = col; c < terms; ++c) {
                    ata[static_cast<size_t>(r) * terms + c] -= f * ata[static_cast<size_t>(col) * terms + c];
                }
                atb[r] -= f * atb[col];
            }
        }
        for (int k = 0; k < terms; ++k) {
            const double diag = ata[static_cast<size_t>(k) * terms + k];
            coef[k] = std::abs(diag) < 1e-12 ? 0.0 : atb[k] / diag;
        }
        for (size_t i = 0; i < n; ++i) {
            const double xn = normalized(x[i]);
            double v = 0.0, p = 1.0;
            for (int k = 0; k < terms; ++k) {
                v += coef[k] * p;
                p *= xn;
            }
            fitted[i] = v;
        }

        // Tukey bisquare weights from the residuals for the next pass.
        std::vector<double> residuals(n);
        for (size_t i = 0; i < n; ++i) {
            residuals[i] = y[i] - fitted[i];
        }
        const double s = Spread(residuals, 0.0);
        if (s < 1e-6) {
            break;
        }
        const double c = 4.685 * s;
        for (size_t i = 0; i < n; ++i) {
            const double u = residuals[i] / c;
            weights[i] = std::abs(u) < 1.0 ? (1 - u * u) * (1 - u * u) : 0.0;
        }
    }
    return fitted;
}

// Gaussian-weighted average over neighboring points (by position in the list).
std::vector<double> FitMovingAverage(const std::vector<double>& y, int sigma) {
    const size_t n = y.size();
    const double s = std::max(1, sigma);
    std::vector<double> fitted(n);
    for (size_t i = 0; i < n; ++i) {
        double sum = 0.0, wsum = 0.0;
        const size_t lo = i > static_cast<size_t>(3 * s) ? i - static_cast<size_t>(3 * s) : 0;
        const size_t hi = std::min(n - 1, i + static_cast<size_t>(3 * s));
        for (size_t j = lo; j <= hi; ++j) {
            const double d = (static_cast<double>(j) - static_cast<double>(i)) / s;
            const double w = std::exp(-0.5 * d * d);
            sum += w * y[j];
            wsum += w;
        }
        fitted[i] = sum / wsum;
    }
    return fitted;
}

std::string CsvEscape(const std::string& s) {
    if (s.find_first_of(",\"\n") == std::string::npos) {
        return s;
    }
    std::string out = "\"";
    for (char ch : s) {
        if (ch == '"') {
            out += '"';
        }
        out += ch;
    }
    return out + "\"";
}

}  // namespace

std::vector<fs::path> ListDngs(const fs::path& folder) {
    std::vector<fs::path> files;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(folder, ec)) {
        if (entry.is_regular_file(ec) && IsDngPath(entry.path())) {
            files.push_back(entry.path());
        }
    }
    std::ranges::sort(files);
    return files;
}

SequenceAnalysis::SequenceAnalysis(std::vector<fs::path> files, size_t limit, MeterEndpoint endpoint,
                                   unsigned parallel)
    : endpoint_(std::move(endpoint)), parallel_(std::clamp(parallel, 1u, 16u)) {
    std::erase_if(files, [](const fs::path& path) { return !IsDngPath(path); });
    if (!files.empty()) {
        folder_ = files.front().parent_path();
    }
    if (limit > 0 && files.size() > limit) {
        files.resize(limit);
    }
    for (size_t i = 0; i < files.size(); ++i) {
        SequenceFrame frame;
        frame.index = static_cast<int>(i);
        frame.path = files[i];
        frame.name = Utf8(files[i].filename());
        frames_.push_back(std::move(frame));
    }
    count_ = frames_.size();
}

SequenceAnalysis::~SequenceAnalysis() {
    Cancel();
}

void SequenceAnalysis::Start() {
    if (running_ || frames_.empty()) {
        return;
    }
    running_ = true;
    reloading_ = false;
    worker_ = std::jthread([this](std::stop_token stop) { Run(stop, true); });
}

void SequenceAnalysis::ReloadSettings() {
    if (running_ || frames_.empty()) {
        return;
    }
    if (worker_.joinable()) {
        worker_.join();
    }
    running_ = true;
    reloading_ = true;
    metadataRead_ = 0;
    worker_ = std::jthread([this](std::stop_token stop) { Run(stop, false); });
}

void SequenceAnalysis::RereadSettings(const std::vector<int>& frames) {
    for (int index : frames) {
        if (index < 0 || static_cast<size_t>(index) >= frames_.size()) {
            continue;
        }
        const size_t i = static_cast<size_t>(index);
        fs::path path;
        {
            std::scoped_lock lock(mutex_);
            if (frames_[i].state == SequenceFrame::State::Failed) {
                continue;
            }
            path = frames_[i].path;
        }
        auto settings = ReadDevelopSettings(path);
        std::scoped_lock lock(mutex_);
        if (settings) {
            frames_[i].settings = std::move(*settings);
        } else {
            frames_[i].settings = DevelopSettings{.source = "unreadable: " + settings.error()};
        }
    }
}

void SequenceAnalysis::Cancel() {
    if (worker_.joinable()) {
        worker_.request_stop();
        worker_.join();
    }
    running_ = false;
    reloading_ = false;
}

std::vector<fs::path> SequenceAnalysis::Paths() const {
    std::vector<fs::path> paths;
    for (const SequenceFrame& f : frames_) {
        paths.push_back(f.path);
    }
    return paths;
}

// Phase one: the camera settings (once) and the develop settings (every time) of
// each frame, `parallel` at a time. Only headers are read, so this is quick.
void SequenceAnalysis::ReadMetadata(std::stop_token stop) {
    std::atomic<size_t> next{0};
    auto worker = [&] {
        for (size_t i = next++; i < frames_.size() && !stop.stop_requested(); i = next++) {
            SequenceFrame frame;
            {
                std::scoped_lock lock(mutex_);
                frame = frames_[i];
            }
            if (frame.state == SequenceFrame::State::Failed) {
                ++metadataRead_;
                continue;
            }
            if (!frame.metadataRead) {
                if (auto probe = DngDocument::Probe(frame.path)) {
                    frame.capture = *probe;
                    frame.cameraEv = CameraExposureEv(frame.capture);
                } else {
                    frame.state = SequenceFrame::State::Failed;
                    frame.error = probe.error();
                }
            }
            if (frame.state != SequenceFrame::State::Failed) {
                if (auto settings = ReadDevelopSettings(frame.path)) {
                    frame.settings = std::move(*settings);
                } else {
                    frame.settings = DevelopSettings{.source = "unreadable: " + settings.error()};
                }
                frame.metadataRead = true;
            }
            {
                std::scoped_lock lock(mutex_);
                frames_[i] = std::move(frame);
            }
            ++metadataRead_;
        }
    };
    std::vector<std::jthread> pool;
    for (unsigned t = 1; t < parallel_; ++t) {
        pool.emplace_back(worker);
    }
    worker();
    pool.clear();
    if (!stop.stop_requested()) {
        ++settingsGeneration_;
    }
}

void SequenceAnalysis::Run(std::stop_token stop, bool meter) {
    ReadMetadata(stop);
    if (meter && !stop.stop_requested()) {
        // Each frame needs two comparisons from the server: against the first frame
        // and against the previous one. The server caches metered files, so a frame
        // is decoded once however many pairs name it. Frames are handed out in
        // order, `parallel` at a time, and published as they finish.
        const std::vector<fs::path> paths = Paths();
        std::atomic<size_t> next{0};
        auto worker = [&] {
            for (size_t i = next++; i < paths.size() && !stop.stop_requested(); i = next++) {
                SequenceFrame frame;
                {
                    std::scoped_lock lock(mutex_);
                    frame = frames_[i];
                }
                if (frame.state == SequenceFrame::State::Failed) {
                    ++completed_;
                    continue;
                }
                std::string error;
                if (i == 0) {
                    // The reference compares with itself: no difference, but the
                    // server meters it now, so the other frames find it cached.
                    if (auto self = CompareRemote(endpoint_, paths[0], paths[0])) {
                        frame.toFirst = frame.toPrevious = *self;
                    } else {
                        error = self.error();
                    }
                } else {
                    auto toFirst = CompareRemote(endpoint_, paths[0], paths[i]);
                    auto toPrevious = toFirst ? CompareRemote(endpoint_, paths[i - 1], paths[i]) : toFirst;
                    if (!toFirst) {
                        error = toFirst.error();
                    } else if (!toPrevious) {
                        error = toPrevious.error();
                    } else {
                        frame.toFirst = *toFirst;
                        frame.toPrevious = *toPrevious;
                    }
                }
                if (error.empty()) {
                    frame.state = SequenceFrame::State::Done;
                } else {
                    frame.state = SequenceFrame::State::Failed;
                    frame.error = error;
                }
                {
                    std::scoped_lock lock(mutex_);
                    frames_[i] = std::move(frame);
                }
                ++completed_;
            }
        };
        {
            std::vector<std::jthread> pool;
            for (unsigned t = 1; t < parallel_; ++t) {
                pool.emplace_back(worker);
            }
            worker();
        }
    }
    reloading_ = false;
    running_ = false;
}

KeyframeDetection DetectKeyframes(const std::vector<SequenceFrame>& frames) {
    KeyframeDetection result;
    for (const SequenceFrame& f : frames) {
        if (f.metadataRead && f.state != SequenceFrame::State::Failed && f.settings.HasEdits()) {
            result.edited.push_back(f.index);
        }
    }
    const size_t edited = result.edited.size();
    if (edited == 0) {
        result.reason = "no frame has edits";
    } else if (edited > static_cast<size_t>(kMaxAutoKeyframes)) {
        result.reason = std::format("{} frames have edits, more than the {} keyframes allowed", edited, kMaxAutoKeyframes);
    } else if (edited * 3 > frames.size()) {
        result.reason = std::format("{} of {} frames have edits, more than the third allowed", edited, frames.size());
    } else {
        result.ok = true;
    }
    return result;
}

double InterpolateSmooth(const std::vector<double>& xs, const std::vector<double>& ys, double x) {
    const size_t n = xs.size();
    if (n == 0) {
        return 0.0;
    }
    if (n == 1 || x <= xs.front()) {
        return ys.front();
    }
    if (x >= xs.back()) {
        return ys.back();
    }
    // Secant slopes, then the slope at each point: zero at the ends, inside the
    // Fritsch-Butland harmonic mean, which keeps each segment monotone.
    std::vector<double> h(n - 1), d(n - 1), m(n, 0.0);
    for (size_t k = 0; k + 1 < n; ++k) {
        h[k] = xs[k + 1] - xs[k];
        d[k] = h[k] > 0 ? (ys[k + 1] - ys[k]) / h[k] : 0.0;
    }
    for (size_t k = 1; k + 1 < n; ++k) {
        if (d[k - 1] * d[k] > 0) {
            const double w1 = 2 * h[k] + h[k - 1];
            const double w2 = h[k] + 2 * h[k - 1];
            m[k] = (w1 + w2) / (w1 / d[k - 1] + w2 / d[k]);
        }
    }
    // The segment holding x, then the cubic Hermite value in it.
    const size_t k = static_cast<size_t>(std::upper_bound(xs.begin(), xs.end(), x) - xs.begin()) - 1;
    if (h[k] <= 0) {
        return ys[k];
    }
    const double t = (x - xs[k]) / h[k];
    const double t2 = t * t, t3 = t2 * t;
    return (2 * t3 - 3 * t2 + 1) * ys[k] + (t3 - 2 * t2 + t) * h[k] * m[k] + (-2 * t3 + 3 * t2) * ys[k + 1] +
           (t3 - t2) * h[k] * m[k + 1];
}

std::vector<FrameExposure> ComputeExposures(const std::vector<SequenceFrame>& frames, const FitSettings& settings,
                                            const std::vector<int>& keyframes, bool* usesTime) {
    std::vector<FrameExposure> out(frames.size());
    if (frames.empty()) {
        return out;
    }

    // X axis: capture time if every analyzed frame has one, else the frame number.
    bool time = settings.xAxis == XAxis::CaptureTime;
    std::optional<double> firstTime;
    for (const SequenceFrame& f : frames) {
        if (f.state != SequenceFrame::State::Done) {
            continue;
        }
        if (!f.capture.hasTime) {
            time = false;
        } else if (!firstTime) {
            firstTime = f.capture.timeSeconds;
        }
    }
    time = time && firstTime.has_value();
    if (usesTime) {
        *usesTime = time;
    }
    for (size_t i = 0; i < frames.size(); ++i) {
        out[i].x = time && frames[i].capture.hasTime ? frames[i].capture.timeSeconds - *firstTime
                                                     : static_cast<double>(i);
    }

    // The metered raw difference to the first frame, directly or chained.
    double chained = 0.0;
    bool allHaveCameraEv = true;
    std::optional<double> baseEv;  // the first metered frame's camera EV
    for (size_t i = 0; i < frames.size(); ++i) {
        const SequenceFrame& f = frames[i];
        if (f.state != SequenceFrame::State::Done) {
            continue;
        }
        chained += f.toPrevious.deltaEv;
        out[i].rawDeltaEv = settings.compare == CompareMode::FirstFrame ? f.toFirst.deltaEv : chained;
        allHaveCameraEv = allHaveCameraEv && f.cameraEv.has_value();
        if (!baseEv) {
            baseEv = f.cameraEv.value_or(0.0);
        }
    }

    // Absolute brightness: raw brightness is scene brightness plus camera exposure,
    // so taking out each frame's change of settings leaves the scene. Without
    // settings for every frame only the raw difference can be shown.
    for (size_t i = 0; i < frames.size(); ++i) {
        if (!out[i].rawDeltaEv) {
            continue;
        }
        const double settingsChange = allHaveCameraEv ? *frames[i].cameraEv - *baseEv : 0.0;
        out[i].measuredEv = *baseEv + *out[i].rawDeltaEv - settingsChange;
    }

    // Smooth the scene brightness over time.
    std::vector<size_t> fitIndex;
    std::vector<double> xs, ys;
    for (size_t i = 0; i < frames.size(); ++i) {
        if (out[i].measuredEv) {
            fitIndex.push_back(i);
            xs.push_back(out[i].x);
            ys.push_back(*out[i].measuredEv);
        }
    }
    std::vector<double> fitted;
    switch (settings.model) {
        case FitModel::None:
            fitted = ys;
            break;
        case FitModel::Polynomial:
            if (!ys.empty()) {
                fitted = FitPolynomial(xs, ys, settings.degree);
            }
            break;
        case FitModel::MovingAverage:
            fitted = FitMovingAverage(ys, settings.window);
            break;
    }
    for (size_t k = 0; k < fitIndex.size(); ++k) {
        out[fitIndex[k]].fittedEv = fitted[k];
    }

    // The raw brightness each frame is taken to have, relative to the first frame:
    // as metered when deflickering, else the fitted scene brightness with the frame's
    // own settings put back (the difference to the metered value is noise then).
    auto estimated = [&](size_t i) {
        return settings.deflicker ? *out[i].rawDeltaEv : *out[i].rawDeltaEv - (*out[i].measuredEv - *out[i].fittedEv);
    };

    // The viewing brightness: a smooth curve through the keyframes, each at its
    // estimated brightness plus the exposure its edits give it.
    std::vector<std::pair<double, double>> knots;
    for (int k : keyframes) {
        if (k >= 0 && static_cast<size_t>(k) < frames.size() && out[static_cast<size_t>(k)].fittedEv) {
            const size_t i = static_cast<size_t>(k);
            knots.emplace_back(out[i].x, estimated(i) + frames[i].settings.exposure2012.value_or(0.0));
        }
    }
    if (knots.empty()) {
        return out;
    }
    std::ranges::sort(knots);
    std::vector<double> kx, ky;
    for (const auto& [x, y] : knots) {
        kx.push_back(x);
        ky.push_back(y);
    }
    for (size_t i : fitIndex) {
        const double target = InterpolateSmooth(kx, ky, out[i].x);
        out[i].targetEv = *baseEv + target;
        out[i].adjustmentEv = target - estimated(i);
    }
    return out;
}

fs::path CsvPath(const fs::path& folder) {
    return folder / "exposure_match.csv";
}

std::expected<void, std::string> WriteCsv(const fs::path& path, const std::vector<SequenceFrame>& frames,
                                          const std::vector<FrameExposure>& exposures, const FitSettings& settings,
                                          const std::vector<int>& keyframes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    const char* compare = settings.compare == CompareMode::FirstFrame ? "first frame" : "previous frame";
    const char* model = settings.model == FitModel::None         ? "none"
                        : settings.model == FitModel::Polynomial ? "polynomial"
                                                                 : "moving average";
    out << "# raw2raw2 exposure match. EV scale: log2(t / 1 s) - 2 log2(N) + log2(ISO / 100). "
        << "Reference: " << (frames.empty() ? "" : frames.front().name) << ". Compared with " << compare
        << ", fit " << model;
    if (settings.model == FitModel::Polynomial) {
        out << " degree " << settings.degree;
    } else if (settings.model == FitModel::MovingAverage) {
        out << " sigma " << settings.window << " frames";
    }
    out << ", deflicker " << (settings.deflicker ? "on" : "off") << ". Keyframes:";
    for (int k : keyframes) {
        if (k >= 0 && static_cast<size_t>(k) < frames.size()) {
            const SequenceFrame& f = frames[static_cast<size_t>(k)];
            out << ' ' << f.name << " (Exposure2012 " << FormatExposure2012(f.settings.exposure2012.value_or(0.0)) << ')';
        }
    }
    out << ". raw_delta_ev: metered raw brightness relative to the reference. measured_ev: scene brightness, "
           "i.e. raw_delta_ev with the frame's change of camera settings taken out, on the reference's scale. "
           "fitted_ev: the smoothed scene brightness. target_ev: the brightness wanted for the frame, a smooth curve "
           "through the keyframes' edited brightness, on the same scale. adjustment_ev: exposure to apply to the "
           "frame to reach it"
        << (settings.deflicker ? " (deflicker: from the metered brightness).\n" : " (from the fitted brightness).\n");
    out << "frame,file,keyframe,capture_time,exposure_time_s,f_number,iso,camera_ev,raw_delta_ev,measured_ev,"
           "fitted_ev,target_ev,adjustment_ev,spread_ev,compared_pixels,existing_exposure2012,status\n";
    for (size_t i = 0; i < frames.size(); ++i) {
        const SequenceFrame& f = frames[i];
        const FrameExposure& e = exposures[i];
        auto num = [](const std::optional<double>& v, int decimals) {
            return v ? std::format("{:.{}f}", *v, decimals) : std::string();
        };
        const Comparison& cmp = settings.compare == CompareMode::FirstFrame ? f.toFirst : f.toPrevious;
        const bool done = f.state == SequenceFrame::State::Done;
        const bool keyframe = std::ranges::find(keyframes, f.index) != keyframes.end();
        out << f.index + 1 << ',' << CsvEscape(f.name) << ',' << (keyframe ? "1" : "") << ','
            << CsvEscape(f.capture.timeText) << ','
            << (f.capture.exposureTime > 0 ? std::format("{:.6g}", f.capture.exposureTime) : "") << ','
            << (f.capture.fNumber > 0 ? std::format("{:.3g}", f.capture.fNumber) : "") << ','
            << (f.capture.iso ? std::to_string(f.capture.iso) : "") << ',' << num(f.cameraEv, 4) << ','
            << num(e.rawDeltaEv, 4) << ',' << num(e.measuredEv, 4) << ',' << num(e.fittedEv, 4) << ','
            << num(e.targetEv, 4) << ',' << num(e.adjustmentEv, 4) << ','
            << (done ? std::format("{:.4f}", cmp.spreadEv) : "") << ','
            << (done ? std::format("{:.3f}", cmp.validFraction) : "") << ',' << num(f.settings.exposure2012, 2)
            << ',' << (done ? "ok" : f.state == SequenceFrame::State::Failed ? CsvEscape("failed: " + f.error) : "pending")
            << '\n';
    }
    if (!out) {
        return std::unexpected("could not write " + Utf8(path.filename()));
    }
    return {};
}
