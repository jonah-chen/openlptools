#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <exception>
#include <mutex>
#include <thread>
#include <vector>

// Runs f(threadIndex, itemIndex) for every item on up to `threads` threads (0 = one
// per core), handing items out dynamically so uneven items balance. The first
// exception thrown by f is rethrown on the calling thread after all threads stop.
template <typename F>
void ParallelForEach(size_t count, unsigned threads, F&& f) {
    if (count == 0) {
        return;
    }
    if (threads == 0) {
        threads = std::max(1u, std::thread::hardware_concurrency());
    }
    threads = static_cast<unsigned>(std::min<size_t>(threads, count));

    std::atomic<size_t> next{0};
    std::exception_ptr error;
    std::mutex errorMutex;
    auto worker = [&](unsigned thread) {
        try {
            for (size_t i = next++; i < count; i = next++) {
                f(thread, i);
            }
        } catch (...) {
            std::scoped_lock lock(errorMutex);
            if (!error) {
                error = std::current_exception();
            }
            next = count;  // the other threads stop at their next item
        }
    };
    {
        std::vector<std::jthread> pool;
        for (unsigned t = 1; t < threads; ++t) {
            pool.emplace_back(worker, t);
        }
        worker(0);
    }
    if (error) {
        std::rethrow_exception(error);
    }
}
