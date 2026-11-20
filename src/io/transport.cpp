#include "io/transport.h"

#include <mutex>
#include <thread>

#include "core/protocol.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <cstring>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace tracespace {

#ifdef _WIN32

std::string socket_address(const std::string& path) {
    const std::string prefix = "\\\\.\\pipe\\";
    if (path.rfind(prefix, 0) == 0) return path;
    std::string name;
    for (char c : path) name += (c == '/' || c == '\\' || c == ':') ? '-' : c;
    while (!name.empty() && name.front() == '-') name.erase(name.begin());
    return prefix + name;
}

namespace {

class PipeServer final : public Server {
public:
    ~PipeServer() override { stop(); }

    bool start(const std::string& path, Callbacks callbacks, std::string* error) override {
        stop();
        name_ = socket_address(path);
        cb_ = std::move(callbacks);
        stop_event_ = CreateEventA(nullptr, TRUE, FALSE, nullptr);
        HANDLE first = create_instance(true);
        if (first == INVALID_HANDLE_VALUE) {
            if (error) *error = "cannot create pipe " + name_ + " (error " + std::to_string(GetLastError()) + ")";
            CloseHandle(stop_event_);
            stop_event_ = nullptr;
            return false;
        }
        thread_ = std::thread([this, first] { run(first); });
        return true;
    }

    bool send(const std::string& line) override {
        std::lock_guard<std::mutex> lock(mu_);
        if (client_ == INVALID_HANDLE_VALUE) return false;
        std::string data = line + "\n";
        OVERLAPPED ov{};
        ov.hEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
        DWORD written = 0;
        BOOL ok = WriteFile(client_, data.data(), (DWORD)data.size(), &written, &ov);
        if (!ok && GetLastError() == ERROR_IO_PENDING) ok = GetOverlappedResult(client_, &ov, &written, TRUE);
        CloseHandle(ov.hEvent);
        return ok && written == data.size();
    }

    void stop() override {
        if (!stop_event_) return;
        SetEvent(stop_event_);
        if (thread_.joinable()) thread_.join();
        CloseHandle(stop_event_);
        stop_event_ = nullptr;
    }

private:
    HANDLE create_instance(bool first) {
        DWORD open = PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0);
        return CreateNamedPipeA(name_.c_str(), open, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                                PIPE_UNLIMITED_INSTANCES, 1 << 16, 1 << 16, 0, nullptr);
    }
    // Waits for an overlapped operation or the stop event. False means "stop".
    bool wait(HANDLE pipe, OVERLAPPED& ov) {
        HANDLE handles[2] = {ov.hEvent, stop_event_};
        if (WaitForMultipleObjects(2, handles, FALSE, INFINITE) == WAIT_OBJECT_0) return true;
        CancelIo(pipe);
        return false;
    }
    void run(HANDLE pipe) {
        OVERLAPPED ov{};
        ov.hEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
        bool running = true;
        while (running && pipe != INVALID_HANDLE_VALUE) {
            ResetEvent(ov.hEvent);
            BOOL ok = ConnectNamedPipe(pipe, &ov);
            DWORD err = ok ? 0 : GetLastError();
            if (!ok && err == ERROR_IO_PENDING) {
                if (!wait(pipe, ov)) break;
            } else if (!ok && err != ERROR_PIPE_CONNECTED) {
                break;
            }
            {
                std::lock_guard<std::mutex> lock(mu_);
                client_ = pipe;
            }
            if (cb_.on_connect) cb_.on_connect();

            LineSplitter lines;
            char buf[1 << 16];
            while (true) {
                ResetEvent(ov.hEvent);
                DWORD n = 0;
                ok = ReadFile(pipe, buf, sizeof buf, &n, &ov);
                if (!ok && GetLastError() == ERROR_IO_PENDING) {
                    if (!wait(pipe, ov)) {
                        running = false;
                        break;
                    }
                    ok = GetOverlappedResult(pipe, &ov, &n, FALSE);
                }
                if (!ok || n == 0) break;  // client went away
                lines.feed(buf, n, [&](const std::string& l) {
                    if (cb_.on_line) cb_.on_line(l);
                });
            }
            {
                std::lock_guard<std::mutex> lock(mu_);
                client_ = INVALID_HANDLE_VALUE;
            }
            DisconnectNamedPipe(pipe);
            CloseHandle(pipe);
            pipe = INVALID_HANDLE_VALUE;
            if (cb_.on_disconnect) cb_.on_disconnect();
            if (running) pipe = create_instance(false);  // wait for the next client
        }
        if (pipe != INVALID_HANDLE_VALUE) CloseHandle(pipe);
        CloseHandle(ov.hEvent);
    }

    std::string name_;
    Callbacks cb_;
    HANDLE stop_event_ = nullptr;
    HANDLE client_ = INVALID_HANDLE_VALUE;
    std::mutex mu_;
    std::thread thread_;
};

}  // namespace

std::unique_ptr<Server> make_server() { return std::make_unique<PipeServer>(); }

#else  // POSIX

std::string socket_address(const std::string& path) { return path; }

namespace {

class UnixServer final : public Server {
public:
    ~UnixServer() override { stop(); }

    bool start(const std::string& path, Callbacks callbacks, std::string* error) override {
        stop();
        path_ = path;
        cb_ = std::move(callbacks);
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        if (path.size() >= sizeof addr.sun_path) {
            if (error) *error = "socket path too long: " + path;
            return false;
        }
        std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
        listen_fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
        if (listen_fd_ < 0) return fail(error, "socket");
        unlink(path.c_str());  // a stale socket from a previous run
        if (bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) return fail(error, "bind");
        if (listen(listen_fd_, 1) != 0) return fail(error, "listen");
        if (pipe(wake_) != 0) return fail(error, "pipe");
        bound_ = true;
        thread_ = std::thread([this] { run(); });
        return true;
    }

    bool send(const std::string& line) override {
        std::lock_guard<std::mutex> lock(mu_);
        if (client_ < 0) return false;
        std::string data = line + "\n";
        size_t off = 0;
        while (off < data.size()) {
#ifdef MSG_NOSIGNAL
            ssize_t n = ::send(client_, data.data() + off, data.size() - off, MSG_NOSIGNAL);
#else
            ssize_t n = ::write(client_, data.data() + off, data.size() - off);
#endif
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) return false;
            off += (size_t)n;
        }
        return true;
    }

    void stop() override {
        if (thread_.joinable()) {
            char byte = 1;
            (void)!write(wake_[1], &byte, 1);
            thread_.join();
        }
        if (listen_fd_ >= 0) close(listen_fd_);
        if (wake_[0] >= 0) close(wake_[0]);
        if (wake_[1] >= 0) close(wake_[1]);
        listen_fd_ = wake_[0] = wake_[1] = -1;
        if (bound_) unlink(path_.c_str());
        bound_ = false;
    }

private:
    bool fail(std::string* error, const char* what) {
        if (error) *error = std::string(what) + " " + path_ + ": " + std::strerror(errno);
        if (listen_fd_ >= 0) close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }
    void run() {
        while (true) {
            pollfd fds[2] = {{listen_fd_, POLLIN, 0}, {wake_[0], POLLIN, 0}};
            if (poll(fds, 2, -1) < 0) {
                if (errno == EINTR) continue;
                return;
            }
            if (fds[1].revents) return;
            int fd = accept(listen_fd_, nullptr, nullptr);
            if (fd < 0) continue;
#ifdef SO_NOSIGPIPE
            int one = 1;
            setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
            {
                std::lock_guard<std::mutex> lock(mu_);
                client_ = fd;
            }
            if (cb_.on_connect) cb_.on_connect();

            LineSplitter lines;
            char buf[1 << 16];
            bool quit = false;
            while (true) {
                pollfd cfds[2] = {{fd, POLLIN, 0}, {wake_[0], POLLIN, 0}};
                if (poll(cfds, 2, -1) < 0) {
                    if (errno == EINTR) continue;
                    break;
                }
                if (cfds[1].revents) {
                    quit = true;
                    break;
                }
                ssize_t n = read(fd, buf, sizeof buf);
                if (n < 0 && errno == EINTR) continue;
                if (n <= 0) break;  // client went away
                lines.feed(buf, (size_t)n, [&](const std::string& l) {
                    if (cb_.on_line) cb_.on_line(l);
                });
            }
            {
                std::lock_guard<std::mutex> lock(mu_);
                client_ = -1;
            }
            close(fd);
            if (cb_.on_disconnect) cb_.on_disconnect();
            if (quit) return;
        }
    }

    std::string path_;
    Callbacks cb_;
    int listen_fd_ = -1, client_ = -1;
    int wake_[2] = {-1, -1};
    bool bound_ = false;
    std::mutex mu_;
    std::thread thread_;
};

}  // namespace

std::unique_ptr<Server> make_server() { return std::make_unique<UnixServer>(); }

#endif

}  // namespace tracespace
