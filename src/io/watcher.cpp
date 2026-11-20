#include "io/watcher.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>

#if defined(__linux__)
#include <poll.h>
#include <sys/inotify.h>
#include <unistd.h>
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
#define TRACESPACE_KQUEUE 1
#include <fcntl.h>
#include <sys/event.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace tracespace {

bool read_file(const std::string& path, std::string* out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    *out = ss.str();
    return true;
}

namespace {

namespace fs = std::filesystem;

// ------------------------------------------------------------ polling

class PollingWatcher final : public FileWatcher {
public:
    explicit PollingWatcher(int interval_ms) : interval_(interval_ms) {}
    ~PollingWatcher() override { stop(); }

    bool start(const std::string& path, std::function<void()> on_change) override {
        stop();
        path_ = path;
        on_change_ = std::move(on_change);
        quit_ = false;
        last_ = stamp();
        thread_ = std::thread([this] { run(); });
        return true;
    }
    void stop() override {
        {
            std::lock_guard<std::mutex> lock(mu_);
            quit_ = true;
        }
        cv_.notify_all();
        if (thread_.joinable()) thread_.join();
    }
    const char* name() const override { return "polling"; }

private:
    struct Stamp {
        bool exists = false;
        fs::file_time_type mtime{};
        std::uintmax_t size = 0;
        bool operator==(const Stamp&) const = default;
    };
    Stamp stamp() const {
        Stamp s;
        std::error_code ec;
        auto t = fs::last_write_time(path_, ec);
        if (ec) return s;
        auto size = fs::file_size(path_, ec);
        if (ec) return s;
        s.exists = true;
        s.mtime = t;
        s.size = size;
        return s;
    }
    void run() {
        std::unique_lock<std::mutex> lock(mu_);
        while (!quit_) {
            cv_.wait_for(lock, std::chrono::milliseconds(interval_), [this] { return quit_; });
            if (quit_) break;
            Stamp now = stamp();
            // a file that vanished mid-save is reported when it reappears
            if (now.exists && !(now == last_)) {
                last_ = now;
                lock.unlock();
                on_change_();
                lock.lock();
            } else if (!now.exists) {
                last_ = now;
            }
        }
    }

    int interval_;
    std::string path_;
    std::function<void()> on_change_;
    Stamp last_;
    std::mutex mu_;
    std::condition_variable cv_;
    bool quit_ = true;
    std::thread thread_;
};

// ------------------------------------------------------------ inotify

#if defined(__linux__)
// Watches the containing directory, because editors usually save by writing a
// new file and renaming it over the old one.
class InotifyWatcher final : public FileWatcher {
public:
    ~InotifyWatcher() override { stop(); }

    bool start(const std::string& path, std::function<void()> on_change) override {
        stop();
        fs::path p = fs::absolute(path);
        dir_ = p.parent_path().string();
        file_ = p.filename().string();
        on_change_ = std::move(on_change);
        fd_ = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        if (fd_ < 0) return false;
        int wd = inotify_add_watch(fd_, dir_.c_str(), IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE);
        if (wd < 0 || pipe(wake_) != 0) {
            close(fd_);
            fd_ = -1;
            return false;
        }
        thread_ = std::thread([this] { run(); });
        return true;
    }
    void stop() override {
        if (fd_ < 0) return;
        char byte = 1;
        (void)!write(wake_[1], &byte, 1);
        if (thread_.joinable()) thread_.join();
        close(fd_);
        close(wake_[0]);
        close(wake_[1]);
        fd_ = -1;
    }
    const char* name() const override { return "inotify"; }

private:
    void run() {
        alignas(inotify_event) char buf[4096];
        while (true) {
            pollfd fds[2] = {{fd_, POLLIN, 0}, {wake_[0], POLLIN, 0}};
            if (poll(fds, 2, -1) < 0) continue;
            if (fds[1].revents) return;
            ssize_t n = read(fd_, buf, sizeof buf);
            bool hit = false;
            for (ssize_t off = 0; off < n;) {
                auto* ev = reinterpret_cast<inotify_event*>(buf + off);
                if (ev->len && file_ == ev->name) hit = true;
                off += (ssize_t)sizeof(inotify_event) + ev->len;
            }
            if (hit) on_change_();
        }
    }

    std::string dir_, file_;
    std::function<void()> on_change_;
    int fd_ = -1;
    int wake_[2] = {-1, -1};
    std::thread thread_;
};
#endif

// ------------------------------------------------------------ kqueue

#if defined(TRACESPACE_KQUEUE)
#ifndef O_EVTONLY
#define O_EVTONLY O_RDONLY
#endif
// Watches the file's vnode and re-opens it after a save that replaces the file.
class KqueueWatcher final : public FileWatcher {
public:
    ~KqueueWatcher() override { stop(); }

    bool start(const std::string& path, std::function<void()> on_change) override {
        stop();
        path_ = path;
        on_change_ = std::move(on_change);
        kq_ = kqueue();
        if (kq_ < 0) return false;
        if (pipe(wake_) != 0) {
            close(kq_);
            kq_ = -1;
            return false;
        }
        struct kevent ev;
        EV_SET(&ev, wake_[0], EVFILT_READ, EV_ADD, 0, 0, nullptr);
        kevent(kq_, &ev, 1, nullptr, 0, nullptr);
        if (!watch_file()) {
            close(kq_);
            close(wake_[0]);
            close(wake_[1]);
            kq_ = -1;
            return false;
        }
        thread_ = std::thread([this] { run(); });
        return true;
    }
    void stop() override {
        if (kq_ < 0) return;
        char byte = 1;
        (void)!write(wake_[1], &byte, 1);
        if (thread_.joinable()) thread_.join();
        if (file_fd_ >= 0) close(file_fd_);
        close(kq_);
        close(wake_[0]);
        close(wake_[1]);
        kq_ = file_fd_ = -1;
    }
    const char* name() const override { return "kqueue"; }

private:
    bool watch_file() {
        if (file_fd_ >= 0) close(file_fd_);  // also drops its kevent registration
        file_fd_ = open(path_.c_str(), O_EVTONLY);
        if (file_fd_ < 0) return false;
        struct kevent ev;
        EV_SET(&ev, file_fd_, EVFILT_VNODE, EV_ADD | EV_CLEAR,
               NOTE_WRITE | NOTE_EXTEND | NOTE_DELETE | NOTE_RENAME | NOTE_ATTRIB, 0, nullptr);
        return kevent(kq_, &ev, 1, nullptr, 0, nullptr) == 0;
    }
    void run() {
        while (true) {
            struct kevent ev;
            // while the file is missing (mid-save), look for it again every 100 ms
            timespec retry = {0, 100 * 1000 * 1000};
            int n = kevent(kq_, nullptr, 0, &ev, 1, file_fd_ < 0 ? &retry : nullptr);
            if (n < 0) continue;
            if (n == 0) {
                if (watch_file()) on_change_();
                continue;
            }
            if ((int)ev.ident == wake_[0]) return;
            if (ev.fflags & (NOTE_DELETE | NOTE_RENAME)) {
                if (!watch_file()) {
                    file_fd_ = -1;
                    continue;  // reported once it reappears
                }
            }
            on_change_();
        }
    }

    std::string path_;
    std::function<void()> on_change_;
    int kq_ = -1, file_fd_ = -1;
    int wake_[2] = {-1, -1};
    std::thread thread_;
};
#endif

}  // namespace

std::unique_ptr<FileWatcher> make_polling_watcher(int interval_ms) {
    return std::make_unique<PollingWatcher>(interval_ms);
}

std::unique_ptr<FileWatcher> make_native_watcher() {
#if defined(__linux__)
    return std::make_unique<InotifyWatcher>();
#elif defined(TRACESPACE_KQUEUE)
    return std::make_unique<KqueueWatcher>();
#else
    return nullptr;
#endif
}

std::unique_ptr<FileWatcher> start_file_watcher(const std::string& path, std::function<void()> on_change) {
    if (auto native = make_native_watcher()) {
        if (native->start(path, on_change)) return native;
    }
    auto polling = make_polling_watcher();
    polling->start(path, std::move(on_change));
    return polling;
}

}  // namespace tracespace
