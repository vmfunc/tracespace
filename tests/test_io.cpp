#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/protocol.h"
#include "core/updater.h"
#include "io/transport.h"
#include "io/watcher.h"

#ifdef _WIN32
#include <process.h>
#include <windows.h>
#else
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

using namespace tracespace;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

// A value other threads append to, with a timed wait.
template <class T>
struct Collected {
    std::mutex mu;
    std::condition_variable cv;
    std::vector<T> items;
    void add(T v) {
        {
            std::lock_guard<std::mutex> lock(mu);
            items.push_back(std::move(v));
        }
        cv.notify_all();
    }
    bool wait_for_count(size_t n, std::chrono::milliseconds timeout = 5000ms) {
        std::unique_lock<std::mutex> lock(mu);
        return cv.wait_for(lock, timeout, [&] { return items.size() >= n; });
    }
    size_t count() {
        std::lock_guard<std::mutex> lock(mu);
        return items.size();
    }
    T get(size_t i) {
        std::lock_guard<std::mutex> lock(mu);
        return items.at(i);
    }
};

int process_id() {
#ifdef _WIN32
    return _getpid();
#else
    return (int)getpid();
#endif
}

fs::path temp_file(const std::string& name) {
    return fs::temp_directory_path() / ("tracespace-test-" + std::to_string(process_id()) + "-" + name);
}

void write_file(const fs::path& p, const std::string& text) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << text;
}

// Minimal client for the editor socket.
class Client {
public:
    bool connect_to(const std::string& address) {
        for (int attempt = 0; attempt < 100; ++attempt) {
#ifdef _WIN32
            h_ = CreateFileA(address.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            if (h_ != INVALID_HANDLE_VALUE) return true;
#else
            fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
            sockaddr_un addr{};
            addr.sun_family = AF_UNIX;
            std::snprintf(addr.sun_path, sizeof addr.sun_path, "%s", address.c_str());
            if (::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0) return true;
            ::close(fd_);
            fd_ = -1;
#endif
            std::this_thread::sleep_for(20ms);
        }
        return false;
    }
    void send(const std::string& data) {
#ifdef _WIN32
        DWORD n = 0;
        WriteFile(h_, data.data(), (DWORD)data.size(), &n, nullptr);
#else
        (void)!::write(fd_, data.data(), data.size());
#endif
    }
    std::string read_line() {
        std::string line;
        char c = 0;
        while (true) {
#ifdef _WIN32
            DWORD n = 0;
            if (!ReadFile(h_, &c, 1, &n, nullptr) || n == 0) break;
#else
            if (::read(fd_, &c, 1) <= 0) break;
#endif
            if (c == '\n') break;
            line += c;
        }
        return line;
    }
    void close() {
#ifdef _WIN32
        if (h_ != INVALID_HANDLE_VALUE) CloseHandle(h_);
        h_ = INVALID_HANDLE_VALUE;
#else
        if (fd_ >= 0) ::close(fd_);
        fd_ = -1;
#endif
    }
    ~Client() { close(); }

private:
#ifdef _WIN32
    HANDLE h_ = INVALID_HANDLE_VALUE;
#else
    int fd_ = -1;
#endif
};

void exercise_watcher(FileWatcher& w) {
    fs::path file = temp_file(std::string("watch-") + w.name() + ".cpp");
    write_file(file, "int main() {}\n");
    Collected<int> changes;
    REQUIRE(w.start(file.string(), [&] { changes.add(1); }));
    std::this_thread::sleep_for(300ms);
    CHECK(changes.count() == 0);  // nothing happened yet

    // a save in place
    write_file(file, "int main() { return 1; }\n");
    CHECK(changes.wait_for_count(1));

    // a save that replaces the file (write a new one, rename over the old)
    std::this_thread::sleep_for(300ms);
    size_t before = changes.count();
    fs::path tmp = file;
    tmp += ".tmp";
    write_file(tmp, "int main() { return 22; }\n");
    fs::rename(tmp, file);
    CHECK(changes.wait_for_count(before + 1));

    w.stop();
    std::this_thread::sleep_for(100ms);
    size_t after_stop = changes.count();
    write_file(file, "int main() { return 333; }\n");
    std::this_thread::sleep_for(400ms);
    CHECK(changes.count() == after_stop);  // no callbacks once stopped
    fs::remove(file);
}

}  // namespace

TEST_CASE("protocol: buffer and cursor messages") {
    Message b = parse_message(R"({"type":"buffer","path":"/tmp/a.cpp","text":"int main() {}\n","cursor":[3,7]})");
    CHECK(b.type == Message::Type::Buffer);
    CHECK(b.path == "/tmp/a.cpp");
    CHECK(b.text == "int main() {}\n");
    CHECK(b.has_cursor);
    CHECK(b.line == 3);
    CHECK(b.col == 7);

    Message no_cursor = parse_message(R"({"type":"buffer","path":"a.c","text":""})");
    CHECK(no_cursor.type == Message::Type::Buffer);
    CHECK_FALSE(no_cursor.has_cursor);

    Message c = parse_message(R"({"type":"cursor","cursor":[1,0]})");
    CHECK(c.type == Message::Type::Cursor);
    CHECK(c.line == 1);
    CHECK(c.col == 0);

    Message unicode = parse_message("{\"type\":\"buffer\",\"path\":\"a.cpp\",\"text\":\"// caf\\u00e9 \\\"q\\\"\\n\"}");
    CHECK(unicode.text == "// café \"q\"\n");
}

TEST_CASE("protocol: malformed input is rejected without throwing") {
    for (const char* bad : {"", "not json", "[]", "42", "{}", R"({"type":"buffer"})", R"({"type":"buffer","path":1,"text":"x"})",
                            R"({"type":"cursor"})", R"({"type":"cursor","cursor":[1]})", R"({"type":"cursor","cursor":["a","b"]})",
                            R"({"type":"cursor","cursor":[0,0]})", R"({"type":"jump","path":"x","line":1,"col":0})",
                            R"({"type":"buffer","path":"a","text":"x")"}) {
        INFO(bad);
        CHECK(parse_message(bad).type == Message::Type::Invalid);
    }
}

TEST_CASE("protocol: jump message is one line of JSON") {
    std::string line = jump_message("C:\\src\\a \"b\".cpp", 12, 4);
    CHECK(line.find('\n') == std::string::npos);
    auto j = nlohmann::json::parse(line);
    CHECK(j["type"] == "jump");
    CHECK(j["path"] == "C:\\src\\a \"b\".cpp");
    CHECK(j["line"] == 12);
    CHECK(j["col"] == 4);
}

TEST_CASE("protocol: lines are reassembled from arbitrary chunks") {
    LineSplitter s;
    std::vector<std::string> lines;
    auto on_line = [&](const std::string& l) { lines.push_back(l); };
    std::string stream = "{\"a\":1}\n{\"b\":2}\r\n\n{\"c\":3}\n{\"partial\"";
    for (size_t i = 0; i < stream.size(); i += 3) s.feed(stream.data() + i, std::min<size_t>(3, stream.size() - i), on_line);
    CHECK(lines == std::vector<std::string>{"{\"a\":1}", "{\"b\":2}", "{\"c\":3}"});
    s.feed(":0}\n", 4, on_line);
    CHECK(lines.back() == "{\"partial\":0}");
}

TEST_CASE("updater: a burst of edits is debounced into one parse of the latest text") {
    Collected<Model> models;
    Updater u([&](Model m) { models.add(std::move(m)); }, 50);
    for (int i = 0; i < 5; ++i) u.submit({"a.cpp", "void f" + std::to_string(i) + "() {}\n"});
    REQUIRE(models.wait_for_count(1));
    std::this_thread::sleep_for(250ms);
    CHECK(models.count() == 1);
    CHECK(models.get(0).functions.at(0).name == "f4");

    u.submit({"a.cpp", "void later() {}\n"});
    REQUIRE(models.wait_for_count(2));
    CHECK(models.get(1).functions.at(0).name == "later");
}

TEST_CASE("updater: continuous typing still produces updates") {
    Collected<Model> models;
    Updater u([&](Model m) { models.add(std::move(m)); }, 50);
    auto stop = std::chrono::steady_clock::now() + 700ms;
    int n = 0;
    while (std::chrono::steady_clock::now() < stop) {
        u.submit({"a.cpp", "void f" + std::to_string(n++) + "() {}\n"});
        std::this_thread::sleep_for(10ms);  // faster than the debounce interval
    }
    CHECK(models.count() >= 2);
}

TEST_CASE("updater: keeps the last good version of a function while it does not parse") {
    Collected<Model> models;
    Updater u([&](Model m) { models.add(std::move(m)); }, 10);
    u.submit({"a.cpp", "void a() {}\nvoid helper() { a(); }\nint main() { helper(); }\n"});
    REQUIRE(models.wait_for_count(1));
    u.submit({"a.cpp", "void a() {}\nvoid helper( { a(); }\nint main() { helper(); }\n"});
    REQUIRE(models.wait_for_count(2));
    Model broken = models.get(1);
    CHECK_FALSE(broken.errors.empty());
    int h = broken.find("helper");
    REQUIRE(h >= 0);
    CHECK(broken.functions[h].body.size() == 1);

    // switching to another file must not carry functions over
    u.submit({"b.cpp", "int other( {\n"});
    REQUIRE(models.wait_for_count(3));
    CHECK(models.get(2).find("helper") < 0);
}

TEST_CASE("file watcher: polling fallback sees saves and replaced files") {
    auto w = make_polling_watcher(50);
    CHECK(std::string(w->name()) == "polling");
    exercise_watcher(*w);
}

TEST_CASE("file watcher: native backend (inotify / kqueue) where available") {
    auto w = make_native_watcher();
    if (!w) {
        SUCCEED("no native watcher on this platform; polling is used");
        return;
    }
    INFO("backend: " << w->name());
    exercise_watcher(*w);
}

TEST_CASE("file watcher: start_file_watcher always yields a working watcher") {
    fs::path file = temp_file("watch-auto.cpp");
    write_file(file, "int a;\n");
    Collected<int> changes;
    auto w = start_file_watcher(file.string(), [&] { changes.add(1); });
    REQUIRE(w);
    std::this_thread::sleep_for(300ms);
    write_file(file, "int a; int b;\n");
    CHECK(changes.wait_for_count(1));
    w->stop();
    fs::remove(file);
}

TEST_CASE("editor socket: connect, lines both ways, disconnect, reconnect") {
#ifdef _WIN32
    std::string path = "\\\\.\\pipe\\tracespace-test-" + std::to_string(process_id());
#else
    std::string path = temp_file("sock").string();
#endif
    Collected<std::string> events;
    auto server = make_server();
    Server::Callbacks cb;
    cb.on_connect = [&] { events.add("connect"); };
    cb.on_line = [&](const std::string& l) { events.add("line:" + l); };
    cb.on_disconnect = [&] { events.add("disconnect"); };
    std::string error;
    REQUIRE(server->start(path, cb, &error));
    CHECK_FALSE(server->send("nobody is listening"));

    {
        Client c;
        REQUIRE(c.connect_to(socket_address(path)));
        REQUIRE(events.wait_for_count(1));
        CHECK(events.get(0) == "connect");

        // two messages split across writes at an awkward place
        c.send("{\"type\":\"cursor\",\"cur");
        std::this_thread::sleep_for(30ms);
        c.send("sor\":[2,5]}\n{\"type\":\"cursor\",\"cursor\":[3,0]}\n");
        REQUIRE(events.wait_for_count(3));
        Message m = parse_message(events.get(1).substr(5));
        CHECK(m.type == Message::Type::Cursor);
        CHECK(m.line == 2);
        CHECK(m.col == 5);
        CHECK(parse_message(events.get(2).substr(5)).line == 3);

        // a large buffer message arrives intact
        std::string big(200000, 'x');
        c.send(nlohmann::json({{"type", "buffer"}, {"path", "big.cpp"}, {"text", big}}).dump() + "\n");
        REQUIRE(events.wait_for_count(4));
        CHECK(parse_message(events.get(3).substr(5)).text.size() == big.size());

        // app -> editor
        REQUIRE(server->send(jump_message("big.cpp", 9, 1)));
        auto j = nlohmann::json::parse(c.read_line());
        CHECK(j["type"] == "jump");
        CHECK(j["line"] == 9);
    }
    REQUIRE(events.wait_for_count(5));
    CHECK(events.get(4) == "disconnect");
    CHECK_FALSE(server->send("gone"));

    // the editor can come back
    {
        Client again;
        REQUIRE(again.connect_to(socket_address(path)));
        REQUIRE(events.wait_for_count(6));
        CHECK(events.get(5) == "connect");
    }
    REQUIRE(events.wait_for_count(7));
    server->stop();
#ifndef _WIN32
    CHECK_FALSE(fs::exists(path));  // the socket file is cleaned up
#endif
}

TEST_CASE("editor socket: address mapping") {
#ifdef _WIN32
    CHECK(socket_address("\\\\.\\pipe\\ts-1") == "\\\\.\\pipe\\ts-1");
    CHECK(socket_address("C:\\Temp\\ts.sock") == "\\\\.\\pipe\\C--Temp-ts.sock");
    CHECK(socket_address("/tmp/ts.sock") == "\\\\.\\pipe\\tmp-ts.sock");
#else
    CHECK(socket_address("/tmp/ts.sock") == "/tmp/ts.sock");
#endif
}
