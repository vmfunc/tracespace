// The interactive terminal app (FTXUI). Everything testable lives in core/.
#pragma once

#include <string>

#include "core/glyphs.h"

namespace tracespace {

struct AppOptions {
    std::string file;         // may be empty when --socket is given
    std::string socket_path;  // empty: no editor connection
    std::string jump_cmd;     // run on Enter when no editor is connected
    Theme theme = Theme::Braille;
    OrderMode order = OrderMode::Execution;
    bool show_library = false;
    bool animate = true;
    bool utf8 = true;
    bool color = true;
};

int run_app(const AppOptions& options);

// One frame of the interface as plain text, without a terminal (--frame WxH).
std::string render_frame(const AppOptions& options, int width, int height, bool help);

}  // namespace tracespace
