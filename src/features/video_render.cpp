#include "features/video_render.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <format>
#include <iterator>

#include "app/app.h"
#include "app/ui.h"
#include "common/util.h"
#include "io/dng_loader.h"

namespace r2r {

namespace {

constexpr float kColumnWidth = 640.0f;
constexpr float kPagePadding = 24.0f;

// Width presets; the last entry of the combo stands for "custom".
constexpr int kVideoWidths[] = {1280, 1920, 2560, 3840};
constexpr const char* kVideoWidthLabels = "1280 (HD)\0" "1920 (Full HD)\0" "2560\0" "3840 (4K UHD)\0" "Custom\0";
constexpr int kVideoFps[] = {24, 25, 30, 48, 50, 60};
constexpr const char* kVideoFpsLabels = "24\0" "25\0" "30\0" "48\0" "50\0" "60\0";

}  // namespace

VideoRenderFeature::VideoRenderFeature(App& app)
    : Feature(app, {"Video Render",
                    "Encode a folder of TIFFs exported from Lightroom into an MP4 video, with the frame size, "
                    "rate and bitrate of your choice."}) {
    AddAction("Render TIFF folder to video...", "Ctrl+R", ImGuiMod_Ctrl | ImGuiKey_R, [this] { BrowseForFolder(); });
}

// ---- Opening and closing ------------------------------------------------------------------

int VideoRenderFeature::OpenScore(const fs::path& path) const {
    std::error_code ec;
    if (fs::is_directory(path, ec)) {
        // A folder of TIFFs without DNGs (the Lightroom export of a sequence).
        return !FolderHas(path, IsDngPath) && FolderHas(path, IsTiffPath) ? 10 : 0;
    }
    return IsTiffPath(path) ? 10 : 0;
}

void VideoRenderFeature::Open(const fs::path& path) {
    std::error_code ec;
    OpenFolder(fs::is_directory(path, ec) ? path : path.parent_path());
}

void VideoRenderFeature::BrowseForFolder(Feature* returnTo) {
    const std::string start = files_.empty() ? std::string() : Utf8(folder_);
    app_.Dialogs().PickFolder(start, [this, returnTo](const fs::path& folder) {
        app_.Activate(this, returnTo);
        OpenFolder(folder);
    });
}

void VideoRenderFeature::Shutdown() {
    CloseFolder();
}

void VideoRenderFeature::CloseFolder() {
    job_.reset();  // cancels and joins a running job
    SDL_DestroyTexture(preview_);
    preview_ = nullptr;
    previewVersion_ = 0;
    files_.clear();
}

// Leaves the feature, discarding the folder and any finished job.
void VideoRenderFeature::Close() {
    CloseFolder();
    app_.GoBack();
}

// Lists the folder's TIFFs and shows the options with their defaults.
void VideoRenderFeature::OpenFolder(const fs::path& folder) {
    CloseFolder();
    std::vector<fs::path> files = ListTiffs(folder);
    if (files.empty()) {
        error_ = std::format("No TIFF files in {}", Utf8(folder));
        return;
    }
    folder_ = folder;
    files_ = std::move(files);
    first_.reset();
    firstError_.clear();
    if (auto info = ProbeTiff(files_.front())) {
        first_ = *info;
    } else {
        firstError_ = info.error();
    }
    std::string name = Utf8(folder.filename());
    if (name.empty()) {
        name = "video";
    }
    SetOutput(folder / (name + ".mp4"));
    bitrateEdited_ = false;
    SyncBitrate();
    error_.clear();
}

void VideoRenderFeature::SetOutput(fs::path path) {
    if (!path.has_extension()) {
        path += ".mp4";
    }
    options_.output = path;
    const std::string text = Utf8(path);
    std::snprintf(outputText_, sizeof(outputText_), "%s", text.c_str());
}

std::pair<uint32_t, uint32_t> VideoRenderFeature::VideoSize() const {
    const uint32_t w = first_ ? first_->width : 0;
    const uint32_t h = first_ ? first_->height : 0;
    return VideoFrameSize(options_, w, h);
}

// Keeps the bitrate proportional to the frame size until the user edits it.
void VideoRenderFeature::SyncBitrate() {
    if (!bitrateEdited_) {
        const auto [w, h] = VideoSize();
        options_.megabitsPerSecond = std::round(DefaultMegabits(w, h, options_.fps));
    }
}

void VideoRenderFeature::StartJob() {
    SetOutput(PathFromUtf8(outputText_));
    const auto [w, h] = VideoSize();
    SDL_DestroyTexture(preview_);
    preview_ = nullptr;
    previewVersion_ = 0;
    job_ = std::make_unique<VideoExportJob>(files_, options_, w, h);
    job_->Start();
}

void VideoRenderFeature::BrowseForOutput() {
    static const SDL_DialogFileFilter filters[] = {{"MP4 video", "mp4"}};
    app_.Dialogs().PickSaveFile(filters, Utf8(options_.output), [this](const fs::path& path) { SetOutput(path); });
}

// ---- Drawing -----------------------------------------------------------------------------------

void VideoRenderFeature::Draw() {
    const float dpi = Dpi();
    ImGui::BeginChild("##video", ImVec2(0, 0), ImGuiChildFlags_None);
    const float padding = kPagePadding * dpi;
    const float available = ImGui::GetContentRegionAvail().x;
    const float width = std::max(100.0f, std::min(available - 2 * padding, kColumnWidth * dpi));
    ImGui::SetCursorPos(ImVec2(std::max(padding, (available - width) / 2), padding));
    ImGui::BeginChild("##column", ImVec2(width, 0), ImGuiChildFlags_AutoResizeY);
    if (files_.empty()) {
        DrawEmpty();
    } else {
        DrawForm();
    }
    ImGui::EndChild();
    ImGui::EndChild();
}

void VideoRenderFeature::DrawEmpty() {
    ImGui::SeparatorText("Render video");
    ImGui::TextWrapped("%s", Info().description.c_str());
    ImGui::Spacing();
    if (ImGui::Button("Choose TIFF folder...", ImVec2(220 * Dpi(), 0))) {
        BrowseForFolder();
    }
    ImGui::Spacing();
    if (!error_.empty()) {
        ErrorText(error_);
        ImGui::Spacing();
    }
    ImGui::TextDisabled("Or drop a folder of TIFFs on the window, or press Ctrl+R.");
}

void VideoRenderFeature::DrawForm() {
    const float dpi = Dpi();
    VideoOptions& o = options_;
    VideoExportJob* job = job_.get();
    const bool running = job && job->Running();

    ImGui::SeparatorText("Render video");
    ImGui::Text("%zu TIFF files in %s", files_.size(), Utf8(folder_).c_str());
    if (first_) {
        const TiffInfo& f = *first_;
        ImGui::TextWrapped("%s: %u x %u, %u-bit %s, %s compression, color profile %s%s",
                           Utf8(files_.front().filename()).c_str(), f.width, f.height, f.bitsPerSample,
                           f.samplesPerPixel >= 3 ? "RGB" : "grayscale", f.compressionName.c_str(),
                           f.colorProfile.c_str(), f.software.empty() ? "" : (", written by " + f.software).c_str());
    } else {
        ErrorText("Cannot read the first file: " + firstError_);
    }

    ImGui::Spacing();
    ImGui::SeparatorText("Video");
    ImGui::BeginDisabled(running);
    const float itemWidth = 220 * dpi;
    ImGui::SetNextItemWidth(itemWidth);
    int codec = static_cast<int>(o.codec);
    if (ImGui::Combo("Codec", &codec, "H.264 (plays everywhere)\0HEVC / H.265 (smaller files)\0")) {
        o.codec = static_cast<VideoCodec>(codec);
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
        ImGui::SetTooltip("Both use the GPU's encoder when it has one. Without one, Windows provides\n"
                          "an H.264 encoder; HEVC then fails to start.");
    }

    ImGui::SetNextItemWidth(itemWidth);
    if (ImGui::Combo("Width", &widthPreset_, kVideoWidthLabels)) {
        if (widthPreset_ < static_cast<int>(std::size(kVideoWidths))) {
            o.width = static_cast<uint32_t>(kVideoWidths[widthPreset_]);
        } else {
            o.width = static_cast<uint32_t>(customWidth_);
        }
        SyncBitrate();
    }
    if (widthPreset_ >= static_cast<int>(std::size(kVideoWidths))) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(100 * dpi);
        if (ImGui::InputInt("pixels", &customWidth_, 0, 0)) {
            customWidth_ = std::clamp(customWidth_, 16, 16384);
            o.width = static_cast<uint32_t>(customWidth_);
            SyncBitrate();
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
            ImGui::SetTooltip("Most encoders stop at 4096 pixels.");
        }
    }

    ImGui::SetNextItemWidth(itemWidth);
    int fitMode = static_cast<int>(o.fit);
    if (ImGui::Combo("Frame", &fitMode, "Crop to 16:9\0Letterbox to 16:9\0Photo's own shape\0")) {
        o.fit = static_cast<FrameFit>(fitMode);
        SyncBitrate();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
        ImGui::SetTooltip("Crop: the photo fills a 16:9 frame; the top and bottom (of a 3:2 photo) are cut off.\n"
                          "Letterbox: the whole photo in a 16:9 frame, with black bars.\n"
                          "Photo's own shape: the video has the photo's aspect ratio, e.g. 3:2.");
    }
    {
        const auto [w, h] = VideoSize();
        ImGui::SameLine();
        ImGui::TextDisabled("%u x %u", w, h);
    }

    ImGui::SetNextItemWidth(itemWidth);
    if (ImGui::Combo("Frame rate", &fpsIndex_, kVideoFpsLabels)) {
        o.fps = static_cast<uint32_t>(kVideoFps[fpsIndex_]);
        SyncBitrate();
    }
    {
        const double seconds = static_cast<double>(files_.size()) / o.fps;
        ImGui::SameLine();
        ImGui::TextDisabled("%zu frames = %s of video", files_.size(),
                            seconds >= 60 ? ClockText(seconds).c_str() : std::format("{:.1f} s", seconds).c_str());
    }

    ImGui::SetNextItemWidth(itemWidth);
    float mbps = static_cast<float>(o.megabitsPerSecond);
    if (ImGui::SliderFloat("Bitrate", &mbps, 2.0f, 200.0f, "%.0f Mbit/s", ImGuiSliderFlags_Logarithmic)) {
        o.megabitsPerSecond = mbps;
        bitrateEdited_ = true;
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
        ImGui::SetTooltip("Higher keeps more detail and makes a larger file. Ctrl+click to type a value.");
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Default")) {
        bitrateEdited_ = false;
        SyncBitrate();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("about %s",
                        ByteText(static_cast<uint64_t>(o.megabitsPerSecond * 1e6 / 8 * files_.size() / o.fps)).c_str());

    ImGui::Spacing();
    ImGui::SeparatorText("Output");
    ImGui::SetNextItemWidth(-(ImGui::CalcTextSize("Browse...").x + ImGui::GetStyle().FramePadding.x * 2 +
                              ImGui::GetStyle().ItemSpacing.x));
    if (ImGui::InputText("##output", outputText_, sizeof(outputText_))) {
        o.output = PathFromUtf8(outputText_);
    }
    ImGui::SameLine();
    if (ImGui::Button("Browse...")) {
        BrowseForOutput();
    }
    {
        std::error_code ec;
        if (!job && fs::exists(PathFromUtf8(outputText_), ec)) {
            ImGui::TextColored(kWarningColor, "This file exists and will be replaced.");
        }
    }
    ImGui::EndDisabled();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    DrawPreview();
    DrawButtons();
}

// Live preview of the frames as they are encoded.
void VideoRenderFeature::DrawPreview() {
    if (!job_) {
        return;
    }
    uint32_t pw = 0, ph = 0;
    std::vector<uint8_t> rgba;
    if (job_->Preview(previewVersion_, pw, ph, rgba)) {
        RenderedImage image;
        image.width = pw;
        image.height = ph;
        image.rgba = std::move(rgba);
        if (app_.UploadTexture(preview_, image)) {
            previewWidth_ = static_cast<float>(pw);
            previewHeight_ = static_cast<float>(ph);
        }
    }
    if (preview_) {
        const float availWidth = ImGui::GetContentRegionAvail().x;
        const float scale = std::min(availWidth / previewWidth_, (240 * Dpi()) / previewHeight_);
        const ImVec2 size{previewWidth_ * scale, previewHeight_ * scale};
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (availWidth - size.x) / 2);
        ImGui::Image(TextureRef(preview_), size);
    }
}

void VideoRenderFeature::DrawButtons() {
    const float dpi = Dpi();
    const ImVec2 buttonSize(120 * dpi, 0);
    VideoExportJob* job = job_.get();
    const VideoOptions& o = options_;

    if (!job) {
        ImGui::BeginDisabled(!first_ || outputText_[0] == '\0');
        if (ImGui::Button("Render", buttonSize)) {
            StartJob();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Other folder...", buttonSize)) {
            BrowseForFolder(app_.ReturnTarget());
        }
        ImGui::SameLine();
        if (ImGui::Button("Close", buttonSize)) {
            Close();
        }
    } else if (job->Running()) {
        const int completed = job->Completed();
        ImGui::ProgressBar(static_cast<float>(completed) / static_cast<float>(job->Count()), ImVec2(-FLT_MIN, 0),
                           std::format("{} / {}   {}", completed, job->Count(), job->Status()).c_str());
        const std::string encoder = job->Encoder();
        if (!encoder.empty()) {
            ImGui::TextDisabled("%u x %u %s, %s", job->Width(), job->Height(), VideoCodecName(o.codec),
                                encoder.c_str());
        }
        if (ImGui::Button("Cancel", buttonSize)) {
            job->Cancel();
        }
    } else {
        if (job->Failed()) {
            ErrorText(job->Status());
        } else {
            ImGui::TextWrapped("%s", job->Status().c_str());
            ImGui::TextDisabled("%s, %s, %u x %u %s, %s, %.1f s", Utf8(o.output).c_str(),
                                ByteText(job->OutputBytes()).c_str(), job->Width(), job->Height(),
                                VideoCodecName(o.codec), job->Encoder().c_str(), job->Seconds());
            if (ImGui::Button("Play", buttonSize)) {
                SDL_OpenURL(FileUrl(o.output).c_str());
            }
            ImGui::SameLine();
            if (ImGui::Button("Show in folder", buttonSize)) {
                SDL_OpenURL(FileUrl(o.output.parent_path()).c_str());
            }
            ImGui::SameLine();
        }
        if (ImGui::Button("Render again", buttonSize)) {
            job_.reset();
            return;
        }
        ImGui::SameLine();
        if (ImGui::Button("Close", buttonSize)) {
            Close();
        }
    }
}

R2R_REGISTER_FEATURE(VideoRenderFeature, 30);

}  // namespace r2r
