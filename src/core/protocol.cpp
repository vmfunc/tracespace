#include "core/protocol.h"

#include <nlohmann/json.hpp>

namespace tracespace {

using nlohmann::json;

static bool read_cursor(const json& j, Message* m) {
    auto it = j.find("cursor");
    if (it == j.end() || !it->is_array() || it->size() != 2) return false;
    if (!(*it)[0].is_number_integer() || !(*it)[1].is_number_integer()) return false;
    m->line = (*it)[0].get<int>();
    m->col = (*it)[1].get<int>();
    m->has_cursor = m->line >= 1 && m->col >= 0;
    return m->has_cursor;
}

Message parse_message(const std::string& json_line) {
    Message m;
    json j = json::parse(json_line, nullptr, /*allow_exceptions=*/false);
    if (!j.is_object()) return m;
    auto type = j.find("type");
    if (type == j.end() || !type->is_string()) return m;
    const std::string t = type->get<std::string>();
    if (t == "buffer") {
        auto path = j.find("path"), text = j.find("text");
        if (path == j.end() || !path->is_string() || text == j.end() || !text->is_string()) return m;
        m.path = path->get<std::string>();
        m.text = text->get<std::string>();
        read_cursor(j, &m);  // optional
        m.type = Message::Type::Buffer;
    } else if (t == "cursor") {
        if (read_cursor(j, &m)) m.type = Message::Type::Cursor;
    }
    return m;
}

std::string jump_message(const std::string& path, int line, int col) {
    json j = {{"type", "jump"}, {"path", path}, {"line", line}, {"col", col}};
    // replace invalid UTF-8 in the path rather than throwing
    return j.dump(-1, ' ', false, json::error_handler_t::replace);
}

}  // namespace tracespace
