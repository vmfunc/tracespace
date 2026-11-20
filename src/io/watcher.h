// File watching behind an interface: inotify on Linux, kqueue on macOS/BSD,
// polling everywhere else (and as a fallback when the native one fails to start).
#pragma once

#include <functional>
#include <memory>
#include <string>

namespace tracespace {

class FileWatcher {
public:
    virtual ~FileWatcher() = default;
    // on_change is called on a background thread each time the file's contents may
    // have changed (including saves that replace the file). Returns false on failure.
    virtual bool start(const std::string& path, std::function<void()> on_change) = 0;
    virtual void stop() = 0;
    virtual const char* name() const = 0;
};

std::unique_ptr<FileWatcher> make_polling_watcher(int interval_ms = 150);
std::unique_ptr<FileWatcher> make_native_watcher();  // nullptr if the platform has none

// Starts the native watcher, falling back to polling. Never returns nullptr.
std::unique_ptr<FileWatcher> start_file_watcher(const std::string& path, std::function<void()> on_change);

bool read_file(const std::string& path, std::string* out);

}  // namespace tracespace
