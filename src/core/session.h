// Session: everything the interactive view does, minus the terminal. It owns the
// model, the view state, the selection and the scroll position, so the behaviour
// can be tested without FTXUI.
#pragma once

#include <string>

#include "core/layout.h"
#include "core/model.h"
#include "core/paint.h"

namespace tracespace {

class Session {
public:
    ViewState view;
    bool animate = true;   // --no-anim turns this off
    bool utf8 = true;      // false: the ascii theme is forced
    std::string connection = "watching file";  // shown in the status bar
    std::string message;   // transient status text (e.g. a jump target)

    // --- inputs
    void set_model(Model m);
    void set_cursor(int line, int col);  // editor cursor; line 0 clears it
    const Model& model() const { return model_; }
    const Layout& layout() const { return layout_; }

    // --- actions
    void move(Dir d);
    void select_at(int grid_x, int grid_y);  // mouse click, in diagram coordinates
    void select_id(const std::string& id);
    void toggle_collapse();
    void toggle_order();
    void cycle_theme();
    void toggle_library();
    void scroll_by(int dx);

    // --- queries
    int selected() const { return selected_; }
    const std::string& selected_id() const { return selected_id_; }
    int cursor_line() const { return cursor_line_; }
    int cursor_col() const { return cursor_col_; }
    int scroll_x() const { return scroll_x_; }
    int scroll_y() const { return scroll_y_; }

    struct Jump {
        bool ok = false;
        std::string path;
        int line = 0, col = 0;  // 1-based line, 0-based column
    };
    Jump jump_target() const;

    // Clamp the scroll position to a viewport and bring the selection into view
    // if it changed since the last call.
    void fit(int view_w, int view_h);
    Grid render(long tick) const;  // the whole diagram; the caller crops to the viewport
    bool animation_visible(int view_w, int view_h) const;

    // "file.cpp | execution | braille | watching file | 2 parse errors"
    std::string status() const;

private:
    void relayout();
    PaintOptions paint_options(long tick) const;

    Model model_;
    Layout layout_;
    int selected_ = -1;
    std::string selected_id_;
    int cursor_line_ = 0, cursor_col_ = 0;
    int cursor_function_ = -1;
    int scroll_x_ = 0, scroll_y_ = 0;
    bool reveal_ = true;         // selection moved: scroll it into view
    bool reveal_cursor_ = false; // editor cursor entered another function
};

// "{file}", "{line}" and "{col}" replaced in a --jump-cmd template.
std::string expand_jump_cmd(const std::string& tmpl, const std::string& file, int line, int col);

}  // namespace tracespace
