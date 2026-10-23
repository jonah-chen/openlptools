#pragma once

// Combines several DNGs of one scene, shot in sequence with unchanged settings,
// into one DNG: the per-pixel mean (a longer exposure) or median (what stays put).
// The result carries the metadata and Camera Raw settings of one chosen frame.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// The most frames a stack may hold. A median keeps every frame in memory, so the
// limit guards against running out. Raise it with the CMake option
// RAW2RAW2_MAX_STACK_FRAMES (e.g. -DRAW2RAW2_MAX_STACK_FRAMES=300).
#ifndef R2R_MAX_STACK_FRAMES
#define R2R_MAX_STACK_FRAMES 100
#endif

enum class StackMode {
    Mean,    // average of the linearized values, written as 16-bit linear data
    Median,  // per-pixel median of the stored values, written in the frames' own encoding
};

const char* StackModeName(StackMode mode);  // "mean", "median"

struct StackRequest {
    std::vector<std::filesystem::path> frames;
    std::filesystem::path metadataSource;  // one of frames: its metadata and XMP go into the result
    StackMode mode = StackMode::Mean;
    std::filesystem::path output;
};

// Memory a median of the frames needs, and what the machine has free, in bytes.
struct StackMemory {
    uint64_t needed = 0;
    uint64_t available = 0;
};

// How a stack of count frames of width x height x planes samples is computed
// and what it needs, for display before it runs.
std::string StackMethodText(StackMode mode, int count);
uint64_t MedianMemoryBytes(int count, uint64_t width, uint64_t height, uint64_t planes);
uint64_t AvailableMemoryBytes();

// Runs one stack on a background thread. Frames are first checked against each
// other; any difference in camera, lens, exposure settings or raw format stops
// the job with a report of what differs.
class StackJob {
public:
    explicit StackJob(StackRequest request);
    ~StackJob();  // cancels and waits

    void Start();
    void Cancel();

    bool Running() const { return running_; }
    bool Finished() const { return finished_; }
    bool Failed() const { return failed_; }
    bool Cancelled() const { return cancelled_; }

    // The current phase and progress within it (frames done of total; total 0 =
    // indeterminate), or how the job ended (including the full error report).
    std::string Status() const;
    int Completed() const { return completed_; }
    int Total() const { return total_; }
    double Seconds() const;
    uint64_t OutputBytes() const { return outputBytes_; }

    const StackRequest& Request() const { return request_; }

private:
    void Run(std::stop_token stop);
    std::expected<void, std::string> Execute(std::stop_token stop);
    void SetPhase(std::string text, int total);

    StackRequest request_;
    std::jthread worker_;
    std::atomic<bool> running_{false};
    std::atomic<bool> finished_{false};
    std::atomic<bool> failed_{false};
    std::atomic<bool> cancelled_{false};
    std::atomic<int> completed_{0};
    std::atomic<int> total_{0};
    std::atomic<uint64_t> outputBytes_{0};
    std::chrono::steady_clock::time_point start_;
    std::atomic<double> seconds_{0.0};

    mutable std::mutex mutex_;
    std::string status_;
};
