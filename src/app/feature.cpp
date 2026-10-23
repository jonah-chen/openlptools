#include "app/feature.h"

#include <algorithm>

namespace r2r {

namespace {

struct Entry {
    int order;
    FeatureFactory factory;
};

// A function-local static, so registrations from other translation units are
// safe whatever the order of static initialization.
std::vector<Entry>& Registry() {
    static std::vector<Entry> entries;
    return entries;
}

}  // namespace

FeatureRegistration::FeatureRegistration(int order, FeatureFactory factory) {
    Registry().push_back({order, factory});
}

std::vector<std::unique_ptr<Feature>> CreateRegisteredFeatures(App& app) {
    std::vector<Entry> entries = Registry();
    std::ranges::stable_sort(entries, {}, &Entry::order);
    std::vector<std::unique_ptr<Feature>> features;
    for (const Entry& e : entries) {
        features.push_back(e.factory(app));
    }
    return features;
}

}  // namespace r2r
