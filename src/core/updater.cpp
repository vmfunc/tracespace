#include "core/updater.h"

#include <algorithm>

#include "core/parser.h"

namespace tracespace {

Updater::Updater(Deliver deliver, int debounce_ms)
    : deliver_(std::move(deliver)), debounce_(debounce_ms), thread_([this] { run(); }) {}

Updater::~Updater() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        quit_ = true;
    }
    cv_.notify_all();
    thread_.join();
}

void Updater::submit(Source s) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        auto now = std::chrono::steady_clock::now();
        if (!pending_) first_ = now;
        last_ = now;
        pending_ = std::move(s);
    }
    cv_.notify_all();
}

void Updater::run() {
    auto parser = make_tree_sitter_parser();
    Model last_good;
    bool have_good = false;

    std::unique_lock<std::mutex> lock(mu_);
    while (true) {
        cv_.wait(lock, [&] { return quit_ || pending_; });
        if (quit_) return;
        // Wait for a quiet period, but never hold a continuous stream of
        // keystrokes back for more than four debounce intervals.
        while (!quit_) {
            auto deadline = std::min(last_ + debounce_, first_ + 4 * debounce_);
            if (std::chrono::steady_clock::now() >= deadline) break;
            cv_.wait_until(lock, deadline);
        }
        if (quit_) return;
        Source src = std::move(*pending_);
        pending_.reset();

        lock.unlock();
        Model fresh = parser->parse(src.path, src.text);
        if (have_good && last_good.path != fresh.path) have_good = false;  // another file
        Model out = have_good ? merge_last_good(fresh, last_good) : fresh;
        if (fresh.errors.empty()) {
            last_good = std::move(fresh);
            have_good = true;
        }
        deliver_(std::move(out));
        lock.lock();
    }
}

}  // namespace tracespace
