#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <string>

enum class VideoCodec { H264, Hevc };

const char* VideoCodecName(VideoCodec codec);

// Writes an MP4 file with Windows Media Foundation, using a hardware encoder when
// the GPU has one and Windows' own encoder otherwise. Frames are NV12 (BT.709,
// limited range). All calls must come from the thread that called Create().
class VideoWriter {
public:
    static std::expected<std::unique_ptr<VideoWriter>, std::string> Create(const std::filesystem::path& output,
                                                                           uint32_t width, uint32_t height,
                                                                           uint32_t framesPerSecond,
                                                                           uint32_t bitsPerSecond, VideoCodec codec);
    ~VideoWriter();
    VideoWriter(const VideoWriter&) = delete;
    VideoWriter& operator=(const VideoWriter&) = delete;

    // nv12 holds width * height * 3 / 2 bytes: the Y plane, then interleaved Cb/Cr.
    std::expected<void, std::string> WriteFrame(const uint8_t* nv12, size_t bytes);

    // Flushes the encoder and completes the file. Without this the file is unplayable.
    std::expected<void, std::string> Finish();

    // "hardware encoder (Intel ...)" or "software encoder", once known.
    const std::string& EncoderDescription() const;

private:
    VideoWriter();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
