#include "io/video_writer.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
// initguid.h makes this file define the GUIDs the headers below declare, so no
// extra import libraries are needed for the codec API properties.
#include <initguid.h>
#include <cguid.h>
#include <codecapi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mftransform.h>
#include <strmif.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstring>
#include <format>

using Microsoft::WRL::ComPtr;

namespace {

std::string Narrow(const wchar_t* text) {
    if (!text || !*text) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(std::max(0, size - 1)), '\0');
    if (size > 1) {
        WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), size, nullptr, nullptr);
    }
    return out;
}

std::string Describe(HRESULT hr, const char* what) {
    std::string hint;
    switch (hr) {
        case MF_E_INVALIDMEDIATYPE:
        case MF_E_TOPO_CODEC_NOT_FOUND:
        case MF_E_INVALIDREQUEST:
            hint = "; no encoder accepts this format. Try a smaller frame size or H.264";
            break;
        case MF_E_UNSUPPORTED_FORMAT: hint = "; the format is not supported"; break;
        case E_OUTOFMEMORY: hint = "; out of memory"; break;
        case HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED): hint = "; the output file is in use or not writable"; break;
        case HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND): hint = "; the output folder does not exist"; break;
        default: break;
    }
    return std::format("{} failed (0x{:08X}){}", what, static_cast<uint32_t>(hr), hint);
}

void SetColorInfo(IMFMediaType* type) {
    type->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
    type->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
    type->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709);
    type->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709);
}

}  // namespace

const char* VideoCodecName(VideoCodec codec) {
    return codec == VideoCodec::H264 ? "H.264" : "HEVC";
}

struct VideoWriter::Impl {
    bool comInitialized = false;
    bool mfStarted = false;
    ComPtr<IMFSinkWriter> writer;
    DWORD stream = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t fps = 30;
    uint64_t frames = 0;
    bool finished = false;
    std::string encoder = "encoder";

    ~Impl() {
        writer.Reset();
        if (mfStarted) {
            MFShutdown();
        }
        if (comInitialized) {
            CoUninitialize();
        }
    }

    LONGLONG TimeOf(uint64_t frame) const {
        return static_cast<LONGLONG>(frame * 10'000'000ull / fps);
    }

    void DescribeEncoder() {
        ComPtr<IMFSinkWriterEx> ex;
        if (FAILED(writer.As(&ex))) {
            return;
        }
        for (DWORD i = 0;; ++i) {
            GUID category{};
            ComPtr<IMFTransform> transform;
            if (FAILED(ex->GetTransformForStream(stream, i, &category, &transform))) {
                break;
            }
            if (category != MFT_CATEGORY_VIDEO_ENCODER) {
                continue;
            }
            ComPtr<IMFAttributes> attributes;
            bool hardware = false;
            std::string name;
            if (SUCCEEDED(transform->GetAttributes(&attributes))) {
                UINT32 length = 0;
                hardware = SUCCEEDED(attributes->GetStringLength(MFT_ENUM_HARDWARE_URL_Attribute, &length));
                WCHAR buffer[256];
                if (SUCCEEDED(attributes->GetString(MFT_FRIENDLY_NAME_Attribute, buffer, 256, &length))) {
                    name = Narrow(buffer);
                }
            }
            encoder = hardware ? "hardware encoder" : "software encoder";
            if (!name.empty()) {
                encoder += " (" + name + ")";
            }
            return;
        }
    }
};

VideoWriter::VideoWriter() : impl_(std::make_unique<Impl>()) {}

VideoWriter::~VideoWriter() = default;

std::expected<std::unique_ptr<VideoWriter>, std::string> VideoWriter::Create(const std::filesystem::path& output,
                                                                             uint32_t width, uint32_t height,
                                                                             uint32_t framesPerSecond,
                                                                             uint32_t bitsPerSecond, VideoCodec codec) {
    if (width == 0 || height == 0 || width % 2 || height % 2) {
        return std::unexpected("the frame size must be even and not zero");
    }
    if (framesPerSecond == 0) {
        return std::unexpected("the frame rate must not be zero");
    }
    std::unique_ptr<VideoWriter> self(new VideoWriter());
    Impl& impl = *self->impl_;
    impl.width = width;
    impl.height = height;
    impl.fps = framesPerSecond;

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (SUCCEEDED(hr)) {
        impl.comInitialized = true;
    } else if (hr != RPC_E_CHANGED_MODE) {
        return std::unexpected(Describe(hr, "CoInitializeEx"));
    }
    hr = MFStartup(MF_VERSION, MFSTARTUP_FULL);
    if (FAILED(hr)) {
        return std::unexpected(Describe(hr, "starting Media Foundation"));
    }
    impl.mfStarted = true;

    ComPtr<IMFAttributes> attributes;
    hr = MFCreateAttributes(&attributes, 4);
    if (FAILED(hr)) {
        return std::unexpected(Describe(hr, "MFCreateAttributes"));
    }
    attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    attributes->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MPEG4);
    hr = MFCreateSinkWriterFromURL(output.c_str(), nullptr, attributes.Get(), &impl.writer);
    if (FAILED(hr)) {
        return std::unexpected(Describe(hr, "creating the output file"));
    }

    // The encoded stream.
    ComPtr<IMFMediaType> out;
    hr = MFCreateMediaType(&out);
    if (FAILED(hr)) {
        return std::unexpected(Describe(hr, "MFCreateMediaType"));
    }
    out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    out->SetGUID(MF_MT_SUBTYPE, codec == VideoCodec::H264 ? MFVideoFormat_H264 : MFVideoFormat_HEVC);
    out->SetUINT32(MF_MT_AVG_BITRATE, bitsPerSecond);
    out->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(out.Get(), MF_MT_FRAME_SIZE, width, height);
    MFSetAttributeRatio(out.Get(), MF_MT_FRAME_RATE, framesPerSecond, 1);
    MFSetAttributeRatio(out.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    out->SetUINT32(MF_MT_MPEG2_PROFILE, codec == VideoCodec::H264 ? static_cast<UINT32>(eAVEncH264VProfile_High)
                                                                  : static_cast<UINT32>(eAVEncH265VProfile_Main_420_8));
    SetColorInfo(out.Get());
    hr = impl.writer->AddStream(out.Get(), &impl.stream);
    if (FAILED(hr)) {
        return std::unexpected(Describe(hr, std::format("adding a {} stream", VideoCodecName(codec)).c_str()));
    }

    // What we feed in.
    ComPtr<IMFMediaType> in;
    hr = MFCreateMediaType(&in);
    if (FAILED(hr)) {
        return std::unexpected(Describe(hr, "MFCreateMediaType"));
    }
    in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    in->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    in->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    in->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
    in->SetUINT32(MF_MT_DEFAULT_STRIDE, width);
    MFSetAttributeSize(in.Get(), MF_MT_FRAME_SIZE, width, height);
    MFSetAttributeRatio(in.Get(), MF_MT_FRAME_RATE, framesPerSecond, 1);
    MFSetAttributeRatio(in.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    SetColorInfo(in.Get());
    hr = impl.writer->SetInputMediaType(impl.stream, in.Get(), nullptr);
    if (FAILED(hr)) {
        return std::unexpected(Describe(hr, std::format("setting up the {} encoder for {} x {}", VideoCodecName(codec),
                                                        width, height)
                                            .c_str()));
    }

    // Ask the encoder for its best quality; not every encoder takes the hint.
    ComPtr<ICodecAPI> codecApi;
    if (SUCCEEDED(impl.writer->GetServiceForStream(impl.stream, GUID_NULL, IID_PPV_ARGS(&codecApi)))) {
        VARIANT value;
        VariantInit(&value);
        value.vt = VT_UI4;
        value.ulVal = 100;
        codecApi->SetValue(&CODECAPI_AVEncCommonQualityVsSpeed, &value);
    }

    hr = impl.writer->BeginWriting();
    if (FAILED(hr)) {
        return std::unexpected(Describe(hr, "starting the encoder"));
    }
    impl.DescribeEncoder();
    return self;
}

std::expected<void, std::string> VideoWriter::WriteFrame(const uint8_t* nv12, size_t bytes) {
    Impl& impl = *impl_;
    const size_t expected = static_cast<size_t>(impl.width) * impl.height * 3 / 2;
    if (bytes != expected) {
        return std::unexpected("the frame has the wrong size");
    }
    ComPtr<IMFMediaBuffer> buffer;
    HRESULT hr = MFCreateMemoryBuffer(static_cast<DWORD>(bytes), &buffer);
    if (FAILED(hr)) {
        return std::unexpected(Describe(hr, "allocating a frame buffer"));
    }
    BYTE* data = nullptr;
    hr = buffer->Lock(&data, nullptr, nullptr);
    if (FAILED(hr)) {
        return std::unexpected(Describe(hr, "locking a frame buffer"));
    }
    std::memcpy(data, nv12, bytes);
    buffer->Unlock();
    buffer->SetCurrentLength(static_cast<DWORD>(bytes));

    ComPtr<IMFSample> sample;
    hr = MFCreateSample(&sample);
    if (FAILED(hr)) {
        return std::unexpected(Describe(hr, "creating a sample"));
    }
    sample->AddBuffer(buffer.Get());
    sample->SetSampleTime(impl.TimeOf(impl.frames));
    sample->SetSampleDuration(impl.TimeOf(impl.frames + 1) - impl.TimeOf(impl.frames));
    hr = impl.writer->WriteSample(impl.stream, sample.Get());
    if (FAILED(hr)) {
        return std::unexpected(Describe(hr, "encoding a frame"));
    }
    ++impl.frames;
    return {};
}

std::expected<void, std::string> VideoWriter::Finish() {
    Impl& impl = *impl_;
    if (impl.finished) {
        return {};
    }
    impl.finished = true;
    const HRESULT hr = impl.writer->Finalize();
    if (FAILED(hr)) {
        return std::unexpected(Describe(hr, "finishing the file"));
    }
    return {};
}

const std::string& VideoWriter::EncoderDescription() const {
    return impl_->encoder;
}
