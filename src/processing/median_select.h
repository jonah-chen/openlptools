#pragma once

// Per-sample medians across a stack of frames. Every function takes `n` rows of
// `count` 16-bit samples (the same sample position in each frame) and writes the
// median of each position to out. The method depends on n, from fastest to most
// general:
//
//   3, 5, 7, 9     a fixed min/max network per sample, no branches, vectorizable
//   up to 16       insertion sort of the n samples
//   larger         std::nth_element (quickselect) in a scratch buffer
//
// Even n gives the mean of the two middle samples, rounded half up. The header has
// no dependencies beyond the standard library so the kernels can be tested alone.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace median {

namespace detail {

inline void Sort2(uint16_t& a, uint16_t& b) {
    const uint16_t lo = std::min(a, b);
    const uint16_t hi = std::max(a, b);
    a = lo;
    b = hi;
}

// Median networks after N. Devillard, "Fast median search: an ANSI C implementation".
inline uint16_t Median3(uint16_t* p) {
    Sort2(p[0], p[1]);
    Sort2(p[1], p[2]);
    Sort2(p[0], p[1]);
    return p[1];
}

inline uint16_t Median5(uint16_t* p) {
    Sort2(p[0], p[1]);
    Sort2(p[3], p[4]);
    Sort2(p[0], p[3]);
    Sort2(p[1], p[4]);
    Sort2(p[1], p[2]);
    Sort2(p[2], p[3]);
    Sort2(p[1], p[2]);
    return p[2];
}

inline uint16_t Median7(uint16_t* p) {
    Sort2(p[0], p[5]);
    Sort2(p[0], p[3]);
    Sort2(p[1], p[6]);
    Sort2(p[2], p[4]);
    Sort2(p[0], p[1]);
    Sort2(p[3], p[5]);
    Sort2(p[2], p[6]);
    Sort2(p[2], p[3]);
    Sort2(p[3], p[6]);
    Sort2(p[4], p[5]);
    Sort2(p[1], p[4]);
    Sort2(p[1], p[3]);
    Sort2(p[3], p[4]);
    return p[3];
}

inline uint16_t Median9(uint16_t* p) {
    Sort2(p[1], p[2]);
    Sort2(p[4], p[5]);
    Sort2(p[7], p[8]);
    Sort2(p[0], p[1]);
    Sort2(p[3], p[4]);
    Sort2(p[6], p[7]);
    Sort2(p[1], p[2]);
    Sort2(p[4], p[5]);
    Sort2(p[7], p[8]);
    Sort2(p[0], p[3]);
    Sort2(p[5], p[8]);
    Sort2(p[4], p[7]);
    Sort2(p[3], p[6]);
    Sort2(p[1], p[4]);
    Sort2(p[2], p[5]);
    Sort2(p[4], p[7]);
    Sort2(p[4], p[2]);
    Sort2(p[6], p[4]);
    Sort2(p[4], p[2]);
    return p[4];
}

template <int N>
void Network(const uint16_t* const* rows, uint16_t* out, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        uint16_t p[N];
        for (int k = 0; k < N; ++k) {
            p[k] = rows[k][i];
        }
        if constexpr (N == 3) {
            out[i] = Median3(p);
        } else if constexpr (N == 5) {
            out[i] = Median5(p);
        } else if constexpr (N == 7) {
            out[i] = Median7(p);
        } else {
            out[i] = Median9(p);
        }
    }
}

// The median of the first n entries of p, which may be reordered. mid = n / 2.
inline uint16_t MiddleOfSorted(const uint16_t* p, int n) {
    const int mid = n / 2;
    if (n % 2 == 1) {
        return p[mid];
    }
    return static_cast<uint16_t>((static_cast<uint32_t>(p[mid - 1]) + p[mid] + 1) / 2);
}

inline void InsertionSort(const uint16_t* const* rows, int n, uint16_t* out, size_t count) {
    uint16_t p[16];
    for (size_t i = 0; i < count; ++i) {
        for (int k = 0; k < n; ++k) {
            const uint16_t v = rows[k][i];
            int j = k;
            while (j > 0 && p[j - 1] > v) {
                p[j] = p[j - 1];
                --j;
            }
            p[j] = v;
        }
        out[i] = MiddleOfSorted(p, n);
    }
}

inline void Select(const uint16_t* const* rows, int n, uint16_t* out, size_t count, std::vector<uint16_t>& scratch) {
    scratch.resize(static_cast<size_t>(n));
    uint16_t* p = scratch.data();
    const int mid = n / 2;
    for (size_t i = 0; i < count; ++i) {
        for (int k = 0; k < n; ++k) {
            p[k] = rows[k][i];
        }
        std::nth_element(p, p + mid, p + n);
        if (n % 2 == 1) {
            out[i] = p[mid];
        } else {
            // nth_element leaves the lower half before mid; its largest is the other middle value.
            const uint16_t lower = *std::max_element(p, p + mid);
            out[i] = static_cast<uint16_t>((static_cast<uint32_t>(lower) + p[mid] + 1) / 2);
        }
    }
}

}  // namespace detail

inline constexpr int kSortLimit = 16;  // n up to this sorts; larger n selects

// Which method MedianRows uses for n frames, for display.
inline const char* MethodName(int n) {
    switch (n) {
        case 3:
        case 5:
        case 7:
        case 9:
            return "min/max network";
        default:
            return n <= kSortLimit ? "insertion sort" : "quickselect";
    }
}

// Medians of `count` sample positions across n rows. scratch is per-thread
// working memory for the selection method.
inline void MedianRows(const uint16_t* const* rows, int n, uint16_t* out, size_t count,
                       std::vector<uint16_t>& scratch) {
    switch (n) {
        case 1:
            std::copy_n(rows[0], count, out);
            return;
        case 3:
            detail::Network<3>(rows, out, count);
            return;
        case 5:
            detail::Network<5>(rows, out, count);
            return;
        case 7:
            detail::Network<7>(rows, out, count);
            return;
        case 9:
            detail::Network<9>(rows, out, count);
            return;
        default:
            if (n <= kSortLimit) {
                detail::InsertionSort(rows, n, out, count);
            } else {
                detail::Select(rows, n, out, count, scratch);
            }
            return;
    }
}

}  // namespace median
