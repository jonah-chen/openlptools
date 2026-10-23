#pragma once

// A feature is one self-contained tool of the app (image viewer, exposure
// matching, video rendering, ...). The app shows every registered feature as a
// card on the home screen and gives the chosen one the whole window.
//
// To add a feature:
//   1. Derive from Feature in src/features/<name>.h/.cpp and implement Draw().
//   2. End the .cpp with R2R_REGISTER_FEATURE(MyFeature, order);
// The CMake build picks up every .cpp under src/, so nothing else needs to change.

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "common/util.h"

namespace r2r {

class App;

struct FeatureInfo {
    std::string name;         // title of the card and of the window
    std::string description;  // one or two sentences for the card
};

// A command offered in the File menu, optionally with a keyboard shortcut. Actions
// of all features are available whichever feature is on screen.
struct FeatureAction {
    std::string label;
    std::string shortcutText;  // shown in the menu, e.g. "Ctrl+O"; may be empty
    int shortcut = 0;          // an ImGuiKeyChord, or 0 for none
    std::function<void()> run;
};

class Feature {
public:
    Feature(App& app, FeatureInfo info) : app_(app), info_(std::move(info)) {}
    virtual ~Feature() = default;
    Feature(const Feature&) = delete;
    Feature& operator=(const Feature&) = delete;

    const FeatureInfo& Info() const { return info_; }
    const std::vector<FeatureAction>& Actions() const { return actions_; }

    // Called every frame, also while the feature is not on screen: picks up
    // finished background work and starts new work.
    virtual void Poll() {}

    // True while background work runs, so the app keeps redrawing.
    virtual bool Busy() const { return false; }

    // Draws the feature into the main window (which has no padding).
    virtual void Draw() = 0;

    // Adds the feature's own menus to the menu bar; only called while it is on screen.
    virtual void DrawMenus() {}

    // Keyboard handling while the feature is on screen. Not called while text is
    // being edited or a popup is open.
    virtual void HandleShortcuts() {}

    // How well the feature can open a dropped or chosen path: 0 = not at all, higher
    // wins. Open() is called on the best match, after the feature is made current.
    virtual int OpenScore(const fs::path& path) const {
        (void)path;
        return 0;
    }
    virtual void Open(const fs::path& path) { (void)path; }

    // Several paths chosen or dropped at once. Features that can show a set of
    // files override this; the default opens the first.
    virtual void OpenFiles(const std::vector<fs::path>& paths) {
        if (!paths.empty()) {
            Open(paths.front());
        }
    }

    // Title of the window while the feature is on screen (the app name is appended).
    virtual std::string WindowTitle() const { return info_.name; }

    // Stops background work and frees resources. Called once, before the renderer and
    // the DNG SDK are torn down.
    virtual void Shutdown() {}

protected:
    void AddAction(std::string label, std::string shortcutText, int shortcut, std::function<void()> run) {
        actions_.push_back({std::move(label), std::move(shortcutText), shortcut, std::move(run)});
    }

    App& app_;

private:
    FeatureInfo info_;
    std::vector<FeatureAction> actions_;
};

// ---- Registry ---------------------------------------------------------------

using FeatureFactory = std::unique_ptr<Feature> (*)(App&);

// Registers a feature at startup; features appear on the home screen by ascending order.
struct FeatureRegistration {
    FeatureRegistration(int order, FeatureFactory factory);
};

// Creates one instance of every registered feature, in home screen order.
std::vector<std::unique_ptr<Feature>> CreateRegisteredFeatures(App& app);

}  // namespace r2r

#define R2R_REGISTER_FEATURE(Class, Order)                                                      \
    static const ::r2r::FeatureRegistration k##Class##Registration {                           \
        Order, [](::r2r::App& app) -> std::unique_ptr<::r2r::Feature> { return std::make_unique<Class>(app); } \
    }
