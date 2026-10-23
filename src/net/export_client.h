#pragma once

// Client side of the export services (src/tools/): asks a server how it is, and
// sends it files to export.

#include <atomic>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// What GET /health says.
struct ServerHealth {
    bool reachable = false;
    std::string tool;     // "photoshop", "lightcraft", ...
    std::string version;  // the application behind it
    int active = 0;
    int waiting = 0;
    std::string error;    // why it is not reachable
};

ServerHealth CheckExportServer(const std::string& host, uint16_t port);

// The options a client chooses; tool-specific sections get sensible defaults.
struct ExportOptions {
    std::string format = "tiff";  // tiff, jpeg, png
    int bitDepth = 16;            // 8 or 16 (jpeg is always 8)
    std::string colorSpace = "AdobeRGB";
    int quality = 92;             // jpeg
    int longEdge = 0;             // 0 = full size
};

std::string ExportFormatExtension(const std::string& format);  // "tif", "jpg", "png"

struct ExportItem {
    std::filesystem::path input;
    std::filesystem::path output;
    bool done = false;
    bool ok = false;
    std::string error;
    int width = 0;
    int height = 0;
    double seconds = 0.0;
};

// Exports the items through the server, `parallel` requests at a time (the server
// queues or runs them as it can), on background threads.
class ExportJob {
public:
    ExportJob(std::string host, uint16_t port, std::vector<ExportItem> items, ExportOptions options, int parallel);
    ~ExportJob();  // cancels and waits

    void Start();
    void Cancel();  // requests in flight finish; the rest are skipped

    bool Running() const { return running_; }
    bool Finished() const { return finished_; }
    bool Cancelled() const { return cancelled_; }
    int Completed() const { return completed_; }  // done, ok or not
    int Failed() const { return failed_; }
    size_t Count() const { return items_.size(); }
    std::vector<ExportItem> Items() const;  // a snapshot

private:
    void Run(std::stop_token stop);

    std::string host_;
    uint16_t port_;
    std::vector<ExportItem> items_;
    ExportOptions options_;
    int parallel_;
    mutable std::mutex mutex_;
    std::jthread worker_;
    std::atomic<bool> running_{false};
    std::atomic<bool> finished_{false};
    std::atomic<bool> cancelled_{false};
    std::atomic<int> completed_{0};
    std::atomic<int> failed_{0};
};
