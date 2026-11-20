// Editor connection: a line-oriented server for one client at a time.
// Unix domain socket on POSIX; a named pipe on Windows (which is what
// vim.uv.new_pipe() connects to there).
#pragma once

#include <functional>
#include <memory>
#include <string>

namespace tracespace {

class Server {
public:
    struct Callbacks {  // all called on the server's own thread
        std::function<void()> on_connect;
        std::function<void(const std::string& line)> on_line;
        std::function<void()> on_disconnect;
    };

    virtual ~Server() = default;
    virtual bool start(const std::string& path, Callbacks callbacks, std::string* error) = 0;
    virtual bool send(const std::string& line) = 0;  // a newline is appended; false if no client
    virtual void stop() = 0;
};

std::unique_ptr<Server> make_server();

// The address actually listened on. On Windows a plain path is mapped into the
// pipe namespace (\\.\pipe\...); elsewhere the path is used as given.
std::string socket_address(const std::string& path);

}  // namespace tracespace
