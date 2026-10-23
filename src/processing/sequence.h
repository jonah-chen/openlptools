#pragma once

#include <atomic>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "io/dng_loader.h"
#include "io/xmp_settings.h"
#include "net/meter_client.h"
#include "processing/metering.h"

struct SequenceFrame {
    enum class State { Pending, Done, Failed };

    int index = 0;
    std::filesystem::path path;
    std::string name;

    State state = State::Pending;  // Done once metered
    std::string error;

    bool metadataRead = false;  // capture, cameraEv and settings are filled in
    CaptureInfo capture;
    std::optional<double> cameraEv;  // CameraExposureEv(capture)
    DevelopSettings settings;        // what Camera Raw currently applies to the file
    Comparison toFirst;              // against the first frame
    Comparison toPrevious;           // against the previous frame
};

// The DNGs directly in a folder, sorted by name: the frames of a sequence folder.
std::vector<std::filesystem::path> ListDngs(const std::filesystem::path& folder);

// Meters a sequence of DNGs on background threads: first every frame's camera
// settings and develop settings are read (quick: headers only), then the brightness
// comparisons come from the exposure-match server (see meter_client.h), `parallel`
// requests at a time.
class SequenceAnalysis {
public:
    // The frames are the DNGs among files, in the order given; limit > 0 analyzes
    // only the first limit of them. Folder() is the first frame's folder.
    SequenceAnalysis(std::vector<std::filesystem::path> files, size_t limit, MeterEndpoint endpoint,
                     unsigned parallel);
    ~SequenceAnalysis();

    void Start();
    // Reads every frame's develop settings again, in the background, e.g. after
    // editing keyframes in Lightroom. Nothing is metered again.
    void ReloadSettings();
    // The same for a few frames, at once. Not while Running().
    void RereadSettings(const std::vector<int>& frames);
    void Cancel();
    bool Running() const { return running_; }
    bool Reloading() const { return reloading_; }
    int MetadataRead() const { return metadataRead_; }  // frames whose metadata is in
    int Completed() const { return completed_; }        // frames metered (done or failed)
    // Counts up each time every frame's settings have been read, so keyframes can be
    // detected anew.
    int SettingsGeneration() const { return settingsGeneration_; }

    unsigned Parallel() const { return parallel_; }
    const MeterEndpoint& Endpoint() const { return endpoint_; }

    const std::filesystem::path& Folder() const { return folder_; }
    size_t Count() const { return count_; }
    std::vector<std::filesystem::path> Paths() const;

    // Runs f(const std::vector<SequenceFrame>&) with the frames locked against the
    // worker, which publishes each finished frame under the same lock.
    template <typename F>
    void WithFrames(F&& f) const {
        std::scoped_lock lock(mutex_);
        f(frames_);
    }

private:
    void Run(std::stop_token stop, bool meter);
    void ReadMetadata(std::stop_token stop);

    std::filesystem::path folder_;
    size_t count_ = 0;
    MeterEndpoint endpoint_;
    unsigned parallel_;
    std::vector<SequenceFrame> frames_;
    mutable std::mutex mutex_;
    std::jthread worker_;
    std::atomic<bool> running_{false};
    std::atomic<bool> reloading_{false};
    std::atomic<int> metadataRead_{0};
    std::atomic<int> completed_{0};
    std::atomic<int> settingsGeneration_{0};
};

enum class CompareMode { FirstFrame, PreviousFrame };
enum class FitModel { None, Polynomial, MovingAverage };
enum class XAxis { Frame, CaptureTime };

struct FitSettings {
    CompareMode compare = CompareMode::FirstFrame;
    FitModel model = FitModel::Polynomial;
    int degree = 3;    // polynomial
    int window = 10;   // moving average: Gaussian sigma in frames
    XAxis xAxis = XAxis::Frame;
    // Off: each frame's difference from the fitted scene brightness is taken as
    // metering noise and left alone, so only the smooth trend is corrected. On: every
    // frame's metered brightness is corrected, flicker included.
    bool deflicker = false;

    bool operator==(const FitSettings&) const = default;
};

// Brightness of each frame on the camera-settings scale (1 s, f/1.0, ISO 100 = 0 EV).
struct FrameExposure {
    double x = 0.0;  // frame index, or seconds since the first frame
    std::optional<double> rawDeltaEv;  // metered brightness of the raw data relative to the first frame
    // The scene's absolute brightness: the first frame's camera EV plus the raw
    // difference, with the frame's own change of camera settings taken out again.
    // The first frame sits exactly at its camera EV.
    std::optional<double> measuredEv;
    std::optional<double> fittedEv;      // smooth trend through the measured values (None: same as measured)
    std::optional<double> targetEv;      // brightness the frame is adjusted to: the keyframe curve
    std::optional<double> adjustmentEv;  // exposure to apply to the frame to reach the target
};

// Keyframes are the frames whose look is taken as given: each one's edits (its
// Exposure2012 above all) define the brightness wanted there, and the frames between
// follow a smooth curve from one keyframe to the next. The adjustments make every
// frame's estimated brightness meet that curve; a keyframe keeps its own exposure.
// usesTime reports whether the x values are capture times (false if any frame lacks one).
std::vector<FrameExposure> ComputeExposures(const std::vector<SequenceFrame>& frames, const FitSettings& settings,
                                            const std::vector<int>& keyframes, bool* usesTime = nullptr);

// Automatic keyframes: every frame with edits, provided there is at least one, at
// most kMaxAutoKeyframes and no more than a third of the frames.
inline constexpr int kMaxAutoKeyframes = 10;
struct KeyframeDetection {
    std::vector<int> edited;  // the frames with edits
    bool ok = false;          // they can serve as keyframes
    std::string reason;       // why not, for the user
};
KeyframeDetection DetectKeyframes(const std::vector<SequenceFrame>& frames);

// Smooth interpolation through (xs, ys), xs ascending: a monotone cubic (no
// overshoot) that is flat at the first and last point and constant beyond them.
double InterpolateSmooth(const std::vector<double>& xs, const std::vector<double>& ys, double x);

// The CSV written next to the sequence: one row per frame.
std::filesystem::path CsvPath(const std::filesystem::path& folder);
std::expected<void, std::string> WriteCsv(const std::filesystem::path& path, const std::vector<SequenceFrame>& frames,
                                          const std::vector<FrameExposure>& exposures, const FitSettings& settings,
                                          const std::vector<int>& keyframes);
