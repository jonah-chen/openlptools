#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "io/tiff_loader.h"
#include "io/video_writer.h"

// How a photo is placed in a video frame of a different shape.
enum class FrameFit {
    Crop,       // scaled to fill the frame, the overhang cut off equally on both sides
    Letterbox,  // scaled to fit inside the frame, black bars on the remaining sides
    Photo,      // the frame takes the photo's own shape (bars only for frames of another shape)
};

struct VideoOptions {
    uint32_t width = 3840;             // video width; the height follows from fit and aspect
    FrameFit fit = FrameFit::Crop;
    double aspect = 16.0 / 9.0;        // frame shape for Crop and Letterbox
    uint32_t fps = 30;
    double megabitsPerSecond = 40.0;
    VideoCodec codec = VideoCodec::H264;
    std::filesystem::path output;
};

// The TIFFs of a folder in name order.
std::vector<std::filesystem::path> ListTiffs(const std::filesystem::path& folder);

// Video frame size (even numbers) for photos of the given size.
std::pair<uint32_t, uint32_t> VideoFrameSize(const VideoOptions& options, uint32_t photoWidth, uint32_t photoHeight);

// A sensible bitrate for the frame size: about 40 Mbit/s for 4K at 30 fps.
double DefaultMegabits(uint32_t width, uint32_t height, uint32_t fps);

// 8-bit sRGB pixels.
struct RgbImage {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> rgb;  // width * height * 3
};

// Scales a decoded TIFF into a width x height frame according to fit, converting
// its colors to sRGB; uncovered parts of the frame are black.
RgbImage FitFrame(const TiffImage& image, uint32_t width, uint32_t height, FrameFit fit, unsigned threads = 0);

// Encodes a list of TIFFs into a video on a background thread.
class VideoExportJob {
public:
    VideoExportJob(std::vector<std::filesystem::path> files, VideoOptions options, uint32_t width, uint32_t height);
    ~VideoExportJob();  // cancels and waits

    void Start();
    void Cancel();

    bool Running() const { return running_; }
    bool Finished() const { return finished_; }
    bool Failed() const { return failed_; }
    bool Cancelled() const { return cancelled_; }
    int Completed() const { return completed_; }  // frames encoded
    size_t Count() const { return files_.size(); }
    uint32_t Width() const { return width_; }
    uint32_t Height() const { return height_; }
    const VideoOptions& Options() const { return options_; }

    // What is happening now, or how it ended (including the error message).
    std::string Status() const;
    std::string Encoder() const;
    double Seconds() const;         // wall time spent
    uint64_t OutputBytes() const;   // size of the finished file

    // Copies the most recent encoded frame, reduced for display, if it changed
    // since `version`; updates version and returns true when it did.
    bool Preview(uint64_t& version, uint32_t& width, uint32_t& height, std::vector<uint8_t>& rgba) const;

private:
    void Run(std::stop_token stop);
    void SetStatus(std::string text);

    std::vector<std::filesystem::path> files_;
    VideoOptions options_;
    uint32_t width_;
    uint32_t height_;

    std::jthread worker_;
    std::atomic<bool> running_{false};
    std::atomic<bool> finished_{false};
    std::atomic<bool> failed_{false};
    std::atomic<bool> cancelled_{false};
    std::atomic<int> completed_{0};
    std::atomic<double> seconds_{0.0};
    std::atomic<uint64_t> outputBytes_{0};

    mutable std::mutex mutex_;
    std::string status_;
    std::string encoder_;
    uint64_t previewVersion_ = 0;
    uint32_t previewWidth_ = 0;
    uint32_t previewHeight_ = 0;
    std::vector<uint8_t> previewRgba_;
};
