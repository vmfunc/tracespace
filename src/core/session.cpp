#include "core/session.h"

#include <algorithm>

namespace tracespace {

void Session::relayout() {
    if (!utf8) view.theme = Theme::Ascii;
    layout_ = compute_layout(model_, view);
    // keep the selection; if its box is gone, fall back to the nearest ancestor
    std::string id = selected_id_;
    int found = find_box(layout_, id);
    while (found < 0 && !id.empty()) {
        auto cut = id.rfind('/');
        if (cut == std::string::npos) break;
        id.resize(cut);
        found = find_box(layout_, id);
    }
    if (found < 0 && !layout_.boxes.empty()) found = 0;
    if (found != selected_ || (found >= 0 && layout_.boxes[found].id != selected_id_)) reveal_ = true;
    selected_ = found;
    selected_id_ = found >= 0 ? layout_.boxes[found].id : std::string();
}

void Session::set_model(Model m) {
    model_ = std::move(m);
    cursor_function_ = cursor_line_ > 0 ? model_.function_at(cursor_line_, cursor_col_) : -1;
    relayout();
}

void Session::set_cursor(int line, int col) {
    cursor_line_ = line;
    cursor_col_ = col;
    int fn = line > 0 ? model_.function_at(line, col) : -1;
    if (fn != cursor_function_ && fn >= 0) reveal_cursor_ = true;
    cursor_function_ = fn;
}

void Session::move(Dir d) {
    int next = navigate(layout_, selected_, d);
    if (next < 0 || next == selected_) return;
    selected_ = next;
    selected_id_ = layout_.boxes[next].id;
    reveal_ = true;
}

void Session::select_at(int grid_x, int grid_y) {
    int hit = box_at(layout_, grid_x, grid_y);
    if (hit < 0) return;
    selected_ = hit;
    selected_id_ = layout_.boxes[hit].id;
    reveal_ = true;
}

void Session::select_id(const std::string& id) {
    int hit = find_box(layout_, id);
    if (hit < 0) return;
    selected_ = hit;
    selected_id_ = id;
    reveal_ = true;
}

void Session::toggle_collapse() {
    if (selected_ < 0 || !layout_.boxes[selected_].collapsible) return;
    if (!view.collapsed.erase(selected_id_)) view.collapsed.insert(selected_id_);
    relayout();
    reveal_ = true;
}

void Session::toggle_order() {
    view.order = view.order == OrderMode::Execution ? OrderMode::Source : OrderMode::Execution;
    // the same function usually exists in both views: stay on it
    int fn = selected_ >= 0 ? layout_.boxes[selected_].function : -1;
    relayout();
    if (fn >= 0 && (selected_ < 0 || layout_.boxes[selected_].function != fn)) {
        for (size_t i = 0; i < layout_.boxes.size(); ++i) {
            if (layout_.boxes[i].function == fn) {
                selected_ = (int)i;
                selected_id_ = layout_.boxes[i].id;
                break;
            }
        }
    }
    reveal_ = true;
}

void Session::cycle_theme() {
    if (!utf8) return;
    view.theme = view.theme == Theme::Braille ? Theme::Rounded
               : view.theme == Theme::Rounded ? Theme::Ascii : Theme::Braille;
    relayout();
}

void Session::toggle_library() {
    view.show_library = !view.show_library;
    relayout();
}

void Session::scroll_by(int dx) { scroll_x_ += dx; }

Session::Jump Session::jump_target() const {
    Jump j;
    if (selected_ < 0) return j;
    const LBox& b = layout_.boxes[selected_];
    j.ok = b.def.line > 0;
    j.path = model_.path;
    j.line = b.def.line;
    j.col = b.def.col;
    return j;
}

void Session::fit(int view_w, int view_h) {
    auto reveal_box = [&](const LBox& b) {
        const int margin = 2;
        // include the arcs above the box when there is room
        int top = b.y;
        for (const auto& a : layout_.arcs)
            if (a.y_base == b.y && a.x0 >= b.x && a.x1 < b.x + b.w) top = std::min(top, a.y_top);
        if (b.x + b.w + margin > scroll_x_ + view_w) scroll_x_ = b.x + b.w + margin - view_w;
        if (b.x - margin < scroll_x_) scroll_x_ = b.x - margin;
        if (b.y + b.h > scroll_y_ + view_h) scroll_y_ = b.y + b.h - view_h;
        if (top < scroll_y_) scroll_y_ = top;
    };
    if (reveal_cursor_ && cursor_function_ >= 0) {
        for (const auto& b : layout_.boxes)
            if (b.function == cursor_function_) {
                reveal_box(b);
                break;
            }
    }
    if (reveal_ && selected_ >= 0) reveal_box(layout_.boxes[selected_]);
    reveal_ = reveal_cursor_ = false;
    scroll_x_ = std::max(0, std::min(scroll_x_, layout_.width - view_w));
    scroll_y_ = std::max(0, std::min(scroll_y_, layout_.height - view_h));
}

PaintOptions Session::paint_options(long tick) const {
    PaintOptions po;
    po.theme = view.theme;
    po.selected_box = selected_;
    po.hot_arcs = pick_hot_arcs(layout_, selected_, cursor_line_, cursor_col_);
    po.tick = animate ? tick : -1;
    po.cursor_function = cursor_function_;
    po.cursor_line = cursor_line_;
    po.cursor_col = cursor_col_;
    return po;
}

Grid Session::render(long tick) const { return paint(layout_, paint_options(tick)); }

bool Session::animation_visible(int view_w, int view_h) const {
    if (!animate) return false;
    for (int i : pick_hot_arcs(layout_, selected_, cursor_line_, cursor_col_)) {
        const LArc& a = layout_.arcs[i];
        bool x = a.x0 < scroll_x_ + view_w && a.x1 >= scroll_x_;
        bool y = a.y_top < scroll_y_ + view_h && a.y_base > scroll_y_;
        if (x && y) return true;
    }
    return false;
}

std::string Session::status() const {
    std::string sep = view.theme == Theme::Ascii ? " | " : " │ ";
    std::string file = model_.path;
    auto slash = file.find_last_of("/\\");
    if (slash != std::string::npos) file = file.substr(slash + 1);
    if (file.empty()) file = "(no file)";
    std::string s = " " + file + sep + (view.order == OrderMode::Execution ? "execution" : "source") + sep +
                    theme_name(view.theme) + sep + connection;
    if (view.show_library) s += sep + "lib";
    size_t errors = model_.errors.size();
    if (errors) s += sep + std::to_string(errors) + (errors == 1 ? " parse error" : " parse errors");
    if (!message.empty()) s += sep + message;
    return s;
}

std::string expand_jump_cmd(const std::string& tmpl, const std::string& file, int line, int col) {
    std::string out;
    for (size_t i = 0; i < tmpl.size();) {
        auto take = [&](const std::string& key, const std::string& value) {
            if (tmpl.compare(i, key.size(), key) != 0) return false;
            out += value;
            i += key.size();
            return true;
        };
        if (take("{file}", file) || take("{line}", std::to_string(line)) || take("{col}", std::to_string(col)))
            continue;
        out += tmpl[i++];
    }
    return out;
}

}  // namespace tracespace
