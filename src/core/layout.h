// Layout: a pure function from model + view state to boxes, arcs and connectors
// on a character grid. No FTXUI dependency.
#pragma once

#include <set>
#include <string>
#include <vector>

#include "core/glyphs.h"
#include "core/model.h"

namespace tracespace {

struct ViewState {
    OrderMode order = OrderMode::Execution;
    Theme theme = Theme::Braille;
    std::set<std::string> collapsed;  // box ids
    bool show_library = false;
};

enum class BoxStyle {
    Entry,     // root of an execution row (tight width)
    Normal,    // call to a function defined in the file
    Indirect,  // function pointer / virtual call (dashed border)
    Library,   // call to something not defined in the file (dim)
    Slot,      // placeholder for a loop with no calls in it (dim)
};

struct LBox {
    std::string id;  // stable across re-parses: "main/funcA#0/a1#0"
    int x = 0, y = 0, w = 0, h = 3;
    std::string label;               // final text, markers included
    std::vector<std::string> lines;  // extra rows (indirect-call candidates)
    BoxStyle style = BoxStyle::Normal;
    int depth = 0;       // 0 entry, 1 top row, 2 children, ...
    int function = -1;   // index into Model::functions, or -1
    SrcRange site;       // the call site (or the definition for roots)
    SrcRange def;        // where Enter jumps to
    bool early_return = false, error = false, recursive = false;
    int hidden = 0;            // > 0: collapsed, this many descendants hidden
    bool collapsible = false;  // has (or hides) children
    int parent = -1;           // index of the owning box, or -1
};

struct LArc {
    int x0 = 0, x1 = 0;         // columns of the left (arrowhead) and right legs
    int y_top = 0, y_base = 0;  // top row of the arc; row of the box tops it lands on
    std::string label;          // "×2", "×?", "×∞" (theme-specific spelling)
    std::string count;          // "2", "?", "inf"
    int lane = 0;               // 0 = innermost; outer loops get higher lanes
    int depth = 0;              // nesting depth from the outermost arc (for colour)
    bool has_break = false, has_continue = false;
    SrcRange src;
    int owner = -1;  // box whose body holds the loop (-1: loop in an unrolled entry row)
};

struct LLine {  // axis-aligned connector segment, inclusive ends
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    bool dashed = false;
    bool arrow = false;  // arrowhead at (x1, y1)
};

enum class TextKind { LaneLabel, GotoName, ReturnMark, GotoSite, LabelSite };

struct LText {
    int x = 0, y = 0;
    std::string text;  // empty for the *Mark / *Site kinds (the painter picks the glyph)
    TextKind kind = TextKind::LaneLabel;
    SrcRange src;
};

struct Layout {
    int width = 0, height = 0;
    std::vector<LBox> boxes;
    std::vector<LArc> arcs;
    std::vector<LLine> lines;
    std::vector<LText> texts;
};

Layout compute_layout(const Model& model, const ViewState& view);

// Hit testing and keyboard navigation (geometric).
enum class Dir { Left, Right, Up, Down };
int box_at(const Layout& l, int x, int y);         // -1 if none
int navigate(const Layout& l, int from, Dir dir);  // returns `from` if there is nowhere to go
int find_box(const Layout& l, const std::string& id);

}  // namespace tracespace
