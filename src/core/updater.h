// Updater: debounces incoming source text and parses it on a background thread,
// so typing never blocks the UI. Delivers a model merged with the last good one.
#pragma once

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "core/model.h"

namespace tracespace {

struct Source {
    std::string path;
    std::string text;
};

class Updater {
public:
    using Deliver = std::function<void(Model)>;  // called on the worker thread

    explicit Updater(Deliver deliver, int debounce_ms = 50);
    ~Updater();
    Updater(const Updater&) = delete;
    Updater& operator=(const Updater&) = delete;

    // Latest submission wins; a burst of submissions produces one parse.
    void submit(Source s);

private:
    void run();

    Deliver deliver_;
    std::chrono::milliseconds debounce_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::optional<Source> pending_;
    std::chrono::steady_clock::time_point first_, last_;
    bool quit_ = false;
    std::thread thread_;
};

}  // namespace tracespace
