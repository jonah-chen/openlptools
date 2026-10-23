#include "tools/photoshop_export/photoshop.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <format>
#include <stdexcept>

#include "tools/common/process.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <objbase.h>
#include <oleauto.h>

namespace r2r::tools {

namespace {

constexpr const wchar_t* kProgId = L"Photoshop.Application";
constexpr auto kLaunchTimeout = std::chrono::seconds(180);
constexpr auto kBusyRetry = std::chrono::minutes(30);  // a script blocks while a long export runs

std::string Lower(std::string s) {
    std::ranges::transform(s, s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string HresultText(HRESULT hr) {
    wchar_t* text = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                   static_cast<DWORD>(hr), 0, reinterpret_cast<wchar_t*>(&text), 0, nullptr);
    std::string s = text ? Narrow(text) : std::string();
    LocalFree(text);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
        s.pop_back();
    }
    return std::format("{} (0x{:08X})", s.empty() ? "COM error" : s, static_cast<unsigned>(hr));
}

bool IsBusy(HRESULT hr) {
    return hr == RPC_E_CALL_REJECTED || hr == RPC_E_SERVERCALL_RETRYLATER || hr == RPC_E_SERVERCALL_REJECTED;
}

// Invokes a method or property on an automation object with the given arguments
// (in call order). Returns the HRESULT; result receives the value.
HRESULT Invoke(IDispatch* object, const wchar_t* name, WORD flags, std::vector<VARIANT> args, VARIANT* result) {
    DISPID id = 0;
    LPOLESTR names[] = {const_cast<LPOLESTR>(name)};
    HRESULT hr = object->GetIDsOfNames(IID_NULL, names, 1, LOCALE_USER_DEFAULT, &id);
    if (FAILED(hr)) {
        return hr;
    }
    std::ranges::reverse(args);  // DISPPARAMS wants them last to first
    DISPPARAMS params{args.data(), nullptr, static_cast<UINT>(args.size()), 0};
    EXCEPINFO excep{};
    UINT argErr = 0;
    hr = object->Invoke(id, IID_NULL, LOCALE_USER_DEFAULT, flags, &params, result, &excep, &argErr);
    if (hr == DISP_E_EXCEPTION) {
        std::string message = excep.bstrDescription ? Narrow(excep.bstrDescription) : "script exception";
        SysFreeString(excep.bstrSource);
        SysFreeString(excep.bstrDescription);
        SysFreeString(excep.bstrHelpFile);
        throw std::runtime_error(message);
    }
    return hr;
}

VARIANT StringVariant(const std::wstring& s) {
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_BSTR;
    v.bstrVal = SysAllocStringLen(s.data(), static_cast<UINT>(s.size()));
    return v;
}

VARIANT MissingVariant() {
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_ERROR;
    v.scode = DISP_E_PARAMNOTFOUND;
    return v;
}

// The ExtendScript that performs one export; PARAMS is replaced by a JSON object.
constexpr const char* kExportScript = R"JS(
(function (p) {
    var old = app.displayDialogs;
    var doc = null;
    try {
        app.displayDialogs = DialogModes.NO;
        var o = new CameraRAWOpenOptions();
        o.settings = p.cameraDefault ? CameraRAWSettingsType.CAMERADEFAULT : CameraRAWSettingsType.SELECTEDIMAGE;
        if (p.bitDepth) o.bitsPerChannel = p.bitDepth == 16 ? BitsPerChannelType.SIXTEEN : BitsPerChannelType.EIGHT;
        if (p.colorSpace) o.colorSpace = ColorSpaceType[p.colorSpace];
        if (p.size) o.size = CameraRAWSize[p.size];
        if (p.ppi) o.resolution = p.ppi;
        doc = app.open(new File(p.input), o);
        var w = doc.width.as("px"), h = doc.height.as("px");
        if (p.longEdge > 0 && Math.max(w, h) > p.longEdge) {
            var s = p.longEdge / Math.max(w, h);
            doc.resizeImage(UnitValue(Math.round(w * s), "px"), UnitValue(Math.round(h * s), "px"), null, ResampleMethod.BICUBICSHARPER);
            w = doc.width.as("px"); h = doc.height.as("px");
        }
        if (p.format == "jpeg" && doc.bitsPerChannel != BitsPerChannelType.EIGHT) doc.bitsPerChannel = BitsPerChannelType.EIGHT;
        var so;
        if (p.format == "tiff") {
            so = new TiffSaveOptions();
            so.imageCompression = TIFFEncoding[p.tiffCompression];
            so.embedColorProfile = true;
            so.byteOrder = ByteOrder.IBM;
            so.layers = false; so.alphaChannels = false; so.transparency = false; so.saveImagePyramid = false; so.annotations = false; so.spotColors = false;
        } else if (p.format == "jpeg") {
            so = new JPEGSaveOptions();
            so.quality = p.jpegQuality;
            so.embedColorProfile = true;
            so.formatOptions = FormatOptions.STANDARDBASELINE;
            so.matte = MatteType.NONE;
        } else if (p.format == "png") {
            so = new PNGSaveOptions();
            so.compression = 6; so.interlaced = false;
        } else if (p.format == "psd") {
            so = new PhotoshopSaveOptions();
            so.embedColorProfile = true; so.layers = false; so.alphaChannels = false; so.annotations = false; so.spotColors = false;
        } else {
            throw new Error("unsupported format: " + p.format);
        }
        doc.saveAs(new File(p.output), so, true, Extension.LOWERCASE);
        doc.close(SaveOptions.DONOTSAVECHANGES);
        doc = null;
        return "OK " + w + " " + h;
    } catch (e) {
        try { if (doc) doc.close(SaveOptions.DONOTSAVECHANGES); } catch (e2) {}
        return "ERR " + (e.message || e);
    } finally {
        app.displayDialogs = old;
    }
})(PARAMS)
)JS";

std::string ColorSpaceEnum(const std::string& name) {
    const std::string n = Lower(name);
    if (n.empty()) {
        return {};
    }
    if (n == "srgb") return "SRGB";
    if (n == "adobergb" || n == "adobergb1998" || n == "adobe rgb") return "ADOBERGB";
    if (n == "prophotorgb" || n == "prophoto") return "PROPHOTORGB";
    if (n == "colormatchrgb" || n == "colormatch") return "COLORMATCHRGB";
    throw std::runtime_error("colorSpace must be sRGB, AdobeRGB, ProPhotoRGB or ColorMatchRGB");
}

std::string SizeEnum(const std::string& name) {
    const std::string n = Lower(name);
    if (n.empty() || n == "native" || n == "default") {
        return {};  // not set: Camera Raw opens at the native size
    }
    if (n == "minimum") return "MINIMUM";
    if (n == "small") return "SMALL";
    if (n == "medium") return "MEDIUM";
    if (n == "large") return "LARGE";
    if (n == "extralarge") return "EXTRALARGE";
    if (n == "maximum") return "MAXIMUM";
    throw std::runtime_error("photoshop.size must be native, minimum, small, medium, large, extraLarge or maximum");
}

std::string TiffCompressionEnum(const std::string& name) {
    const std::string n = Lower(name);
    if (n.empty() || n == "none") return "NONE";
    if (n == "lzw") return "TIFFLZW";
    if (n == "zip" || n == "deflate") return "TIFFZIP";
    if (n == "jpeg") return "JPEG";
    throw std::runtime_error("photoshop.tiffCompression must be none, lzw, zip or jpeg");
}

}  // namespace

PhotoshopBackend::PhotoshopBackend(fs::path photoshopExe) : exe_(std::move(photoshopExe)) {}

PhotoshopBackend::~PhotoshopBackend() {
    if (thread_.joinable()) {
        thread_.request_stop();
        wake_.notify_all();
        thread_.join();
    }
}

std::string PhotoshopBackend::Connect() {
    std::promise<std::string> connected;
    std::future<std::string> result = connected.get_future();
    thread_ = std::jthread([this, p = std::move(connected)](std::stop_token stop) mutable { ComThread(stop, std::move(p)); });
    return result.get();
}

// Photoshop's automation object must be used from the thread that created it.
void PhotoshopBackend::ComThread(std::stop_token stop, std::promise<std::string> connected) {
    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(init)) {
        connected.set_value("CoInitializeEx failed: " + HresultText(init));
        return;
    }
    connected.set_value(ConnectOnComThread());

    while (true) {
        Job* job = nullptr;
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [&] { return stop.stop_requested() || !jobs_.empty(); });
            if (jobs_.empty()) {
                break;
            }
            job = jobs_.front();
            jobs_.pop_front();
        }
        try {
            job->result.set_value(RunScript(job->script));
        } catch (...) {
            job->result.set_exception(std::current_exception());
        }
    }
    if (app_) {
        app_->Release();
        app_ = nullptr;
    }
    CoUninitialize();
}

std::string PhotoshopBackend::ConnectOnComThread() {
    CLSID clsid{};
    if (FAILED(CLSIDFromProgID(kProgId, &clsid))) {
        return "Photoshop is not registered for automation on this machine (no Photoshop.Application class)";
    }
    auto tryConnect = [&]() -> bool {
        IUnknown* unknown = nullptr;
        if (FAILED(GetActiveObject(clsid, nullptr, &unknown))) {
            return false;
        }
        IDispatch* dispatch = nullptr;
        const HRESULT hr = unknown->QueryInterface(IID_IDispatch, reinterpret_cast<void**>(&dispatch));
        unknown->Release();
        if (FAILED(hr)) {
            return false;
        }
        app_ = dispatch;
        return true;
    };

    if (!tryConnect()) {
        std::error_code ec;
        if (!fs::is_regular_file(exe_, ec)) {
            return std::format("Photoshop is not running and {} does not exist", Narrow(exe_.wstring()));
        }
        std::printf("Starting %s ...\n", Narrow(exe_.wstring()).c_str());
        std::fflush(stdout);
        if (const std::string error = StartDetached(exe_, {}); !error.empty()) {
            return error;
        }
        launched_ = true;
        const auto deadline = std::chrono::steady_clock::now() + kLaunchTimeout;
        while (!tryConnect()) {
            if (std::chrono::steady_clock::now() > deadline) {
                return "Photoshop started but did not register for automation within three minutes";
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    }

    // Version, which also proves scripts run.
    try {
        version_ = RunScript("app.version + \" \" + app.name");
    } catch (const std::exception& e) {
        return std::string("Connected, but Photoshop does not run scripts: ") + e.what();
    }
    return {};
}

std::string PhotoshopBackend::RunScript(const std::string& script) {
    VARIANT result;
    VariantInit(&result);
    const auto deadline = std::chrono::steady_clock::now() + kBusyRetry;
    while (true) {
        const HRESULT hr = Invoke(app_, L"DoJavaScript", DISPATCH_METHOD,
                                  {StringVariant(Widen(script)), MissingVariant(), MissingVariant()}, &result);
        if (SUCCEEDED(hr)) {
            break;
        }
        // Photoshop rejects calls while it is busy (a dialog, another script): wait.
        if (IsBusy(hr) && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            continue;
        }
        throw std::runtime_error("Photoshop did not accept the script: " + HresultText(hr));
    }
    std::string text;
    if (result.vt == VT_BSTR && result.bstrVal) {
        text = Narrow(result.bstrVal);
    }
    VariantClear(&result);
    return text;
}

std::string PhotoshopBackend::ScriptFor(const ExportRequest& request) const {
    const Json ps = request.Section("photoshop");
    const std::string settings = Lower(ps.value("settings", "image"));
    if (settings != "image" && settings != "cameradefault") {
        throw std::runtime_error("photoshop.settings must be image or cameraDefault");
    }
    int jpegQuality = ps.value("jpegQuality", -1);
    if (jpegQuality < 0) {
        jpegQuality = request.quality > 0 ? static_cast<int>(std::lround(request.quality * 12.0 / 100.0)) : 10;
    }
    Json p = {
        {"input", Narrow(request.input.wstring())},
        {"output", Narrow(request.output.wstring())},
        {"format", request.format},
        {"cameraDefault", settings == "cameradefault"},
        {"bitDepth", request.bitDepth},
        {"colorSpace", ColorSpaceEnum(request.colorSpace)},
        {"size", SizeEnum(ps.value("size", ""))},
        {"ppi", request.ppi},
        {"longEdge", request.longEdge},
        {"tiffCompression", TiffCompressionEnum(ps.value("tiffCompression", ""))},
        {"jpegQuality", std::clamp(jpegQuality, 0, 12)},
    };
    if (request.format != "tiff" && request.format != "jpeg" && request.format != "png" && request.format != "psd") {
        throw std::runtime_error("the Photoshop tool writes tiff, jpeg, png or psd");
    }
    std::string script = kExportScript;
    script.replace(script.find("PARAMS"), 6, p.dump());
    return script;
}

Json PhotoshopBackend::Status() {
    std::scoped_lock lock(mutex_);
    return {{"photoshop", version_}, {"launchedByTool", launched_}, {"queued", jobs_.size()}};
}

Json PhotoshopBackend::Export(const ExportRequest& request) {
    Job job;
    job.script = ScriptFor(request);
    std::future<std::string> result = job.result.get_future();
    {
        std::scoped_lock lock(mutex_);
        jobs_.push_back(&job);
    }
    wake_.notify_one();
    const std::string text = result.get();  // waits for Photoshop
    if (text.starts_with("OK ")) {
        int w = 0, h = 0;
        if (sscanf_s(text.c_str() + 3, "%d %d", &w, &h) == 2) {
            return {{"width", w}, {"height", h}};
        }
        return Json::object();
    }
    if (text.starts_with("ERR ")) {
        throw std::runtime_error("Photoshop: " + text.substr(4));
    }
    throw std::runtime_error("unexpected script result: " + text);
}

}  // namespace r2r::tools
