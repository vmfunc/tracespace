#include "core/paint.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace tracespace {

std::string Grid::to_text() const {
    std::string out;
    for (int y = 0; y < h; ++y) {
        int last = -1;
        for (int x = 0; x < w; ++x)
            if (at(x, y)->ch != U' ') last = x;
        for (int x = 0; x <= last; ++x) out += utf8(at(x, y)->ch);
        out += '\n';
    }
    return out;
}

std::vector<std::pair<int, int>> arc_braille_dots(const LArc& a) {
    std::vector<std::pair<int, int>> dots;
    auto add = [&](int x, int y) {
        if (dots.empty() || dots.back() != std::make_pair(x, y)) dots.push_back({x, y});
    };
    const double kHalfPi = 1.5707963267948966;
    int leg_row = a.y_base - 1;                        // row of the arrowhead
    int xl = 2 * a.x0 + 1, xr = 2 * a.x1;              // dot columns of the two legs
    int y_top = 4 * a.y_top + 1, y_bot = 4 * leg_row - 1;
    int ry = std::max(1, y_bot - y_top);
    int rx = std::max(1, std::min((xr - xl) / 2, std::max(2 * ry, 4)));
    int steps = 4 * (rx + ry) + 4;

    for (int y = 4 * leg_row + 3; y >= 4 * leg_row; --y) add(xr, y);  // right leg
    for (int i = 0; i <= steps; ++i) {  // right shoulder: quarter ellipse
        double t = kHalfPi * i / steps;
        add((int)std::lround(xr - rx * (1 - std::cos(t))), (int)std::lround(y_bot - ry * std::sin(t)));
    }
    for (int x = xr - rx; x >= xl + rx; --x) add(x, y_top);  // top
    for (int i = steps; i >= 0; --i) {  // left shoulder
        double t = kHalfPi * i / steps;
        add((int)std::lround(xl + rx * (1 - std::cos(t))), (int)std::lround(y_bot - ry * std::sin(t)));
    }
    return dots;
}

std::vector<std::pair<int, int>> arc_path(const LArc& a, Theme theme) {
    std::vector<std::pair<int, int>> p;
    auto add = [&](int x, int y) {
        if (p.empty() || p.back() != std::make_pair(x, y)) p.push_back({x, y});
    };
    int last = a.y_base - 1, height = a.y_base - a.y_top;
    if (theme == Theme::Braille) {
        for (auto [dx, dy] : arc_braille_dots(a)) add(dx / 2, dy / 4);
        add(a.x0, last);
    } else if (theme == Theme::Ascii && height >= 3) {
        for (int y = last; y >= a.y_top + 2; --y) add(a.x1, y);
        add(a.x1 - 1, a.y_top + 1);
        for (int x = a.x1 - 2; x >= a.x0 + 2; --x) add(x, a.y_top);
        add(a.x0 + 1, a.y_top + 1);
        for (int y = a.y_top + 2; y <= last; ++y) add(a.x0, y);
    } else {
        for (int y = last; y > a.y_top; --y) add(a.x1, y);
        for (int x = a.x1; x >= a.x0; --x) add(x, a.y_top);
        for (int y = a.y_top + 1; y <= last; ++y) add(a.x0, y);
    }
    return p;
}

std::vector<int> pick_hot_arcs(const Layout& l, int selected_box, int cursor_line, int cursor_col) {
    std::vector<int> hot;
    if (cursor_line > 0) {
        // innermost source loop containing the cursor = the one that starts last
        const SrcRange* best = nullptr;
        for (const auto& a : l.arcs) {
            if (!a.src.contains(cursor_line, cursor_col)) continue;
            if (!best || a.src.line > best->line || (a.src.line == best->line && a.src.col > best->col))
                best = &a.src;
        }
        if (best) {
            SrcRange r = *best;
            for (size_t i = 0; i < l.arcs.size(); ++i)
                if (l.arcs[i].src == r) hot.push_back((int)i);
            return hot;
        }
    }
    if (selected_box < 0 || selected_box >= (int)l.boxes.size()) return hot;
    const LBox& b = l.boxes[selected_box];
    int pick = -1;
    // a loop in the selected function's own body: take the outermost
    for (size_t i = 0; i < l.arcs.size(); ++i)
        if (l.arcs[i].owner == selected_box && (pick < 0 || l.arcs[i].lane > l.arcs[pick].lane)) pick = (int)i;
    if (pick < 0) {
        // otherwise the innermost loop the selected box runs inside
        for (size_t i = 0; i < l.arcs.size(); ++i) {
            const LArc& a = l.arcs[i];
            bool over = a.x0 - 1 < b.x + b.w && b.x <= a.x1 + 1;
            bool same_row = a.owner == -1 ? a.y_base == b.y : a.owner == b.parent;
            if (a.owner == -1 && b.parent >= 0 && l.boxes[b.parent].style != BoxStyle::Entry) same_row = false;
            if (over && same_row && (pick < 0 || a.lane < l.arcs[pick].lane)) pick = (int)i;
        }
    }
    if (pick >= 0) hot.push_back(pick);
    return hot;
}

namespace {

enum : uint8_t { N = 1, E = 2, S = 4, W = 8 };

struct Painter {
    const Layout& l;
    const PaintOptions& opt;
    const Glyphs& g;
    Grid grid;

    Painter(const Layout& layout, const PaintOptions& o) : l(layout), opt(o), g(glyphs(o.theme)) {
        grid.w = l.width;
        grid.h = l.height;
        grid.cells.assign((size_t)grid.w * (size_t)grid.h, Cell{});
    }

    void put(int x, int y, char32_t ch, uint8_t color = 0, uint8_t attr = 0) {
        if (Cell* c = grid.at(x, y)) *c = Cell{ch, 0, color, attr};
    }
    void put_text(int x, int y, const std::string& s, uint8_t color = 0, uint8_t attr = 0) {
        for (char32_t ch : decode(s)) put(x++, y, ch, color, attr);
    }

    void lines() {
        // Solid and dashed lines are kept apart: a dashed goto arrow passes behind
        // solid connectors instead of forming a junction with them.
        std::vector<uint8_t> solid((size_t)grid.w * (size_t)grid.h, 0), dash(solid.size(), 0);
        auto mark = [&](int x, int y, uint8_t b, bool d) {
            if (!grid.at(x, y)) return;
            (d ? dash : solid)[(size_t)y * (size_t)grid.w + (size_t)x] |= b;
        };
        for (const auto& s : l.lines) {
            if (s.y0 == s.y1) {
                int a = std::min(s.x0, s.x1), b = std::max(s.x0, s.x1);
                for (int x = a; x <= b; ++x)
                    mark(x, s.y0, a == b ? (E | W) : (uint8_t)((x > a ? W : 0) | (x < b ? E : 0)), s.dashed);
            } else {
                int a = std::min(s.y0, s.y1), b = std::max(s.y0, s.y1);
                for (int y = a; y <= b; ++y)
                    mark(s.x0, y, a == b ? (N | S) : (uint8_t)((y > a ? N : 0) | (y < b ? S : 0)), s.dashed);
            }
        }
        for (int y = 0; y < grid.h; ++y) {
            for (int x = 0; x < grid.w; ++x) {
                size_t i = (size_t)y * (size_t)grid.w + (size_t)x;
                bool dashed = !solid[i];
                uint8_t b = dashed ? dash[i] : solid[i];
                if (!b) continue;
                char32_t ch = g.junction[b];
                bool horizontal = (b & (E | W)) && !(b & (N | S)), vertical = (b & (N | S)) && !(b & (E | W));
                if (dashed && horizontal) ch = g.dh;
                if (dashed && vertical) ch = g.dv;
                put(x, y, ch, 0, dashed ? kDim : 0);
            }
        }
        for (const auto& s : l.lines) {
            if (!s.arrow) continue;
            char32_t head = s.y0 == s.y1 ? g.right : s.y1 < s.y0 ? g.up : g.down;
            put(s.x1, s.y1, head, 0, s.dashed ? kDim : 0);
        }
    }

    void boxes() {
        for (size_t i = 0; i < l.boxes.size(); ++i) {
            const LBox& b = l.boxes[i];
            uint8_t color = 0, attr = 0;
            if (b.style == BoxStyle::Library || b.style == BoxStyle::Slot) attr = kDim;
            if (b.function >= 0 && b.function == opt.cursor_function) color = kColorCursor;
            if ((int)i == opt.selected_box) color = kColorSelect, attr = kBold;
            bool dash = b.style == BoxStyle::Indirect;
            char32_t h = dash ? g.dh : g.h, v = dash ? g.dv : g.v;
            int x1 = b.x + b.w - 1, y1 = b.y + b.h - 1;
            for (int y = b.y; y <= y1; ++y) {
                for (int x = b.x; x <= x1; ++x) {
                    bool top = y == b.y, bottom = y == y1, left = x == b.x, right = x == x1;
                    char32_t ch = U' ';
                    if (top && left) ch = g.tl;
                    else if (top && right) ch = g.tr;
                    else if (bottom && left) ch = g.bl;
                    else if (bottom && right) ch = g.br;
                    else if (top || bottom) ch = h;
                    else if (left || right) ch = v;
                    put(x, y, ch, color, attr);
                }
            }
            auto centered = [&](int y, const std::string& s, uint8_t a) {
                int pad = std::max(0, (b.w - 2 - ulen(s)) / 2);
                put_text(b.x + 1 + pad, y, s, b.error && color == 0 ? (uint8_t)kColorError : color, a);
            };
            centered(b.y + 1, b.label, attr);
            for (size_t k = 0; k < b.lines.size(); ++k) centered(b.y + 2 + (int)k, b.lines[k], attr | kDim);
            if (b.early_return) put(x1, b.y + 1, g.ret, color, attr);
        }
    }

    void arc(const LArc& a, bool hot) {
        uint8_t color = (uint8_t)(kColorArcFirst + a.depth % (kColorArcLast - kColorArcFirst + 1));
        uint8_t attr = hot ? kBold : 0;
        int last = a.y_base - 1, height = a.y_base - a.y_top;
        int inset = 0;  // how far the ends of the top row are pulled in
        if (opt.theme == Theme::Braille) {
            for (auto [dx, dy] : arc_braille_dots(a)) {
                Cell* c = grid.at(dx / 2, dy / 4);
                if (!c) continue;
                static const uint8_t kBit[2][4] = {{0x01, 0x02, 0x04, 0x40}, {0x08, 0x10, 0x20, 0x80}};
                c->braille |= kBit[dx & 1][dy & 3];
                c->ch = 0x2800 + c->braille;
                c->color = color;
                c->attr = attr;
            }
            put(a.x0, last, g.down, color, attr);
            inset = height >= 3 ? 3 : 1;
        } else if (opt.theme == Theme::Ascii) {
            inset = height >= 3 ? 2 : 0;
            int l0 = a.x0 + inset, r0 = a.x1 - inset;
            for (int x = l0; x <= r0; ++x) {
                char32_t ch = x == l0 || x == r0 ? U'.' : x == l0 + 1 || x == r0 - 1 ? U'-' : hot ? U'=' : U'~';
                put(x, a.y_top, ch, color, attr);
            }
            if (height >= 3) {
                put(a.x0 + 1, a.y_top + 1, U'/', color, attr);
                put(a.x1 - 1, a.y_top + 1, U'\\', color, attr);
            }
            for (int y = a.y_top + (height >= 3 ? 2 : 1); y <= last; ++y) {
                put(a.x0, y, y == last ? g.down : U'|', color, attr);
                put(a.x1, y, U'|', color, attr);
            }
        } else {
            char32_t h = hot ? U'━' : U'─', v = hot ? U'┃' : U'│';
            for (int x = a.x0 + 1; x < a.x1; ++x) put(x, a.y_top, h, color, attr);
            put(a.x0, a.y_top, hot ? U'┏' : U'╭', color, attr);
            put(a.x1, a.y_top, hot ? U'┓' : U'╮', color, attr);
            for (int y = a.y_top + 1; y <= last; ++y) {
                put(a.x0, y, y == last ? g.down : v, color, attr);
                put(a.x1, y, v, color, attr);
            }
        }
        if (a.has_continue) put(a.x0 + inset + 1, a.y_top, g.cont, color, attr);
        if (a.has_break) put(a.x1 - inset - 1, a.y_top, g.brk, color, attr);

        // label, centred; while animating it shows the current pass
        std::string label = a.label;
        std::vector<std::pair<int, int>> path;
        size_t runner = 0;
        if (hot && opt.tick >= 0) {
            path = arc_path(a, opt.theme);
            size_t step = std::max<size_t>(1, path.size() / 24);
            size_t pos = (size_t)opt.tick * step;
            runner = pos % path.size();
            bool numeric = !a.count.empty() && a.count != "?" && a.count != "inf";
            long total = numeric ? std::atol(a.count.c_str()) : 0;
            if (total > 0) label = std::to_string((long)(pos / path.size()) % total + 1) + "/" + a.count;
        }
        int room = a.x1 - a.x0 - 1;
        std::string shown = " " + label + " ";
        if (ulen(shown) > room) shown = label;
        int lx = a.x0 + 1 + std::max(0, (room - ulen(shown)) / 2);
        if (!path.empty()) {
            auto [rx, ry] = path[runner];
            bool under_label = ry == a.y_top && rx >= lx && rx < lx + ulen(shown);
            if (!under_label) put(rx, ry, g.runner, color, kBold);
        }
        put_text(lx, a.y_top, shown, color, attr | kBold);
    }

    void texts() {
        for (const auto& t : l.texts) {
            switch (t.kind) {
                case TextKind::LaneLabel: {
                    bool at_cursor = opt.cursor_line > 0 && t.src.contains(opt.cursor_line, opt.cursor_col);
                    put_text(t.x, t.y, t.text, at_cursor ? (uint8_t)kColorCursor : (uint8_t)0, at_cursor ? kBold : kDim);
                    break;
                }
                case TextKind::GotoName: put_text(t.x, t.y, t.text, 0, kDim); break;
                case TextKind::ReturnMark: put(t.x, t.y, g.ret); break;
                case TextKind::GotoSite: put(t.x, t.y, g.goto_site); break;
                case TextKind::LabelSite: put(t.x, t.y, g.label_site); break;
            }
        }
    }

    Grid run() {
        lines();
        boxes();
        for (size_t i = 0; i < l.arcs.size(); ++i) {
            bool hot = std::find(opt.hot_arcs.begin(), opt.hot_arcs.end(), (int)i) != opt.hot_arcs.end();
            arc(l.arcs[i], hot);
        }
        texts();
        return std::move(grid);
    }
};

}  // namespace

Grid paint(const Layout& layout, const PaintOptions& opt) { return Painter(layout, opt).run(); }

}  // namespace tracespace
