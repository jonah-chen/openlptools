#pragma once

// Drives Adobe Photoshop (and through it Camera Raw) over COM automation to open
// a raw file with its stored Camera Raw settings and save it as TIFF, JPEG, PNG or
// PSD. Photoshop runs one script at a time, so exports are queued onto a single
// COM thread and each caller waits for its own.
//
// Request section "photoshop":
//   settings        "image" (the file's own Camera Raw settings, default) | "cameraDefault"
//   size            "native" (default) | "minimum" | "small" | "medium" | "large" | "extraLarge" | "maximum"
//                   (Camera Raw's workflow sizes)
//   tiffCompression "none" (default) | "lzw" | "zip" | "jpeg"
//   jpegQuality     0..12 (Photoshop's scale; "quality" 1..100 is mapped otherwise)
// Common fields used: bitDepth (8/16), colorSpace (sRGB, AdobeRGB, ProPhotoRGB,
// ColorMatchRGB), quality, longEdge, ppi.

#include <condition_variable>
#include <deque>
#include <future>
#include <mutex>
#include <string>
#include <thread>

#include "tools/common/export_service.h"

struct IDispatch;

namespace r2r::tools {

class PhotoshopBackend : public ExportBackend {
public:
    explicit PhotoshopBackend(fs::path photoshopExe);
    ~PhotoshopBackend() override;

    // Starts Photoshop from the given path if none is running, and connects.
    // Returns an error message, or empty.
    std::string Connect();

    std::string Name() const override { return "photoshop"; }
    Json Status() override;
    Json Export(const ExportRequest& request) override;

private:
    struct Job {
        std::string script;
        std::promise<std::string> result;
    };

    void ComThread(std::stop_token stop, std::promise<std::string> connected);
    std::string ConnectOnComThread();
    std::string RunScript(const std::string& script);  // on the COM thread
    std::string ScriptFor(const ExportRequest& request) const;

    fs::path exe_;
    std::jthread thread_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Job*> jobs_;
    IDispatch* app_ = nullptr;
    std::string version_;
    bool launched_ = false;
};

}  // namespace r2r::tools
