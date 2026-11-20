// Painter: Layout -> a grid of styled character cells. Shared by --print (plain
// text) and the TUI (which maps the cells onto an FTXUI canvas).
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "core/layout.h"

namespace tracespace {

enum CellAttr : uint8_t { kBold = 1, kDim = 2 };
enum CellColor : uint8_t {
    kColorDefault = 0,
    kColorArcFirst = 1,  // 1..6: arc palette, cycled by nesting depth
    kColorArcLast = 6,
    kColorSelect = 7,  // the TUI selection
    kColorCursor = 8,  // where the editor cursor is
    kColorError = 9,
};

struct Cell {
    char32_t ch = U' ';
    uint8_t braille = 0;  // non-zero: ch is U+2800 + braille (dots of a curve)
    uint8_t color = kColorDefault;
    uint8_t attr = 0;
};

struct Grid {
    int w = 0, h = 0;
    std::vector<Cell> cells;
    Cell* at(int x, int y) {
        return x < 0 || y < 0 || x >= w || y >= h ? nullptr : &cells[(size_t)y * (size_t)w + (size_t)x];
    }
    const Cell* at(int x, int y) const { return const_cast<Grid*>(this)->at(x, y); }
    std::string to_text() const;  // rows joined by '\n', trailing spaces trimmed
};

struct PaintOptions {
    Theme theme = Theme::Rounded;
    int selected_box = -1;
    std::vector<int> hot_arcs;  // drawn heavy / bold
    long tick = -1;             // >= 0: animate a runner along the hot arcs
    int cursor_function = -1;   // function under the editor cursor
    int cursor_line = 0, cursor_col = 0;
};

Grid paint(const Layout& layout, const PaintOptions& opt);

// Cells the animated runner visits, in travel order (up the right leg, left
// along the top, down to the arrowhead).
std::vector<std::pair<int, int>> arc_path(const LArc& arc, Theme theme);

// Braille dots of an arc's curve in sub-cell coordinates (2 per column, 4 per row),
// in travel order. Computed from quarter ellipses, not picked by hand.
std::vector<std::pair<int, int>> arc_braille_dots(const LArc& arc);

// The loops to emphasise: the innermost loop containing the editor cursor if there
// is one, otherwise the loop under the selected box.
std::vector<int> pick_hot_arcs(const Layout& layout, int selected_box, int cursor_line, int cursor_col);

}  // namespace tracespace
