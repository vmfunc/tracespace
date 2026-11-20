// Editor protocol: newline-delimited JSON in both directions.
//   nvim -> app  {"type":"buffer","path":"...","text":"...","cursor":[line,col]}
//                {"type":"cursor","cursor":[line,col]}
//   app -> nvim  {"type":"jump","path":"...","line":N,"col":M}
// Lines are 1-based and columns 0-based, matching nvim_win_get_cursor.
#pragma once

#include <string>

namespace tracespace {

struct Message {
    enum class Type { Invalid, Buffer, Cursor };
    Type type = Type::Invalid;
    std::string path, text;
    bool has_cursor = false;
    int line = 0, col = 0;
};

// Never throws; malformed input yields Type::Invalid.
Message parse_message(const std::string& json_line);

// One JSON object, without the trailing newline.
std::string jump_message(const std::string& path, int line, int col);

// Reassembles lines from a byte stream.
class LineSplitter {
public:
    template <class F>
    void feed(const char* data, size_t n, F&& on_line) {
        buf_.append(data, n);
        size_t start = 0, nl;
        while ((nl = buf_.find('\n', start)) != std::string::npos) {
            std::string line = buf_.substr(start, nl - start);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty()) on_line(line);
            start = nl + 1;
        }
        buf_.erase(0, start);
    }
    void reset() { buf_.clear(); }

private:
    std::string buf_;
};

}  // namespace tracespace
