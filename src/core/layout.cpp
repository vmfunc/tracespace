#include "core/layout.h"

#include <algorithm>
#include <climits>
#include <cstdlib>
#include <memory>

namespace tracespace {
namespace {

constexpr int kMaxDepth = 12;      // deeper call chains are shown collapsed
constexpr int kBoxBudget = 4000;   // upper bound on expanded boxes
constexpr int kCountCap = 999;     // cap on the "hidden descendants" count

// Intermediate layout tree. Everything is measured bottom-up, then placed top-down.
struct Node;
using NodeP = std::unique_ptr<Node>;

struct Node {
    enum Kind { Box, Seq, Fork, Loop, Point, Slot } kind;
    bool arrows;  // true: on an unrolled row joined by arrows; false: packed under a box

    LBox box;       // Box, Slot
    NodeP body;     // Box: packed sequence of the callee's body
    int goto_rows = 0;  // Box / root Seq: rows reserved below for goto arrows
    std::vector<NodeP> kids;  // Seq: items. Fork: lanes (Seqs). Loop: [inner Seq]
    std::string label;        // lane label / loop label / goto or label name
    std::string count;        // Loop
    int terminal = 0;         // lane Seq: 1 ends in a return, 2 ends in a goto
    bool is_goto = false;     // Point
    bool brk = false, cont = false;  // Loop
    SrcRange src;
    int minw = 0;  // Loop: narrowest span that fits the arc and its label
    int arc_depth = 0;  // Loop: arcs enclosing this one

    // measured
    int w = 0, cw = 0;  // width; x offset of the outgoing connector
    int asc = 0, desc = 0;  // rows above / from the box-top baseline
    bool head = false, boxy = false;  // wants an arrowhead on entry; exit is a box edge
    int off = 0, cx = 0;  // Fork lane: baseline offset. Fork: x offset of lane content
    int px = 0;           // Loop: placed x

    Node(Kind k, bool a) : kind(k), arrows(a) {}
};

// Arc height grows with the span it covers (the legs sit one column inside).
int own_arc_height(int width, int arc_depth) {
    int span = width - 2 - 6 * arc_depth;
    return span < 20 ? 2 : span < 60 ? 3 : 4;
}

struct Engine {
    const Model& m;
    const ViewState& view;
    const Glyphs& g;
    int budget = kBoxBudget;
    std::vector<signed char> early_cache;
    Layout out;

    Engine(const Model& model, const ViewState& v)
        : m(model), view(v), g(glyphs(v.theme)), early_cache(model.functions.size(), -1) {}

    // ------------------------------------------------------------ build

    static bool has_nested_return(const std::vector<Region>& rs) {
        for (const auto& r : rs)
            if (r.kind == RegionKind::Return || has_nested_return(r.children)) return true;
        return false;
    }
    // A return anywhere except as the final statement of the body.
    bool early_return(int fn) {
        if (early_cache[fn] >= 0) return early_cache[fn];
        const auto& body = m.functions[fn].body;
        bool early = false;
        for (size_t i = 0; i < body.size(); ++i) {
            if (body[i].kind == RegionKind::Return) early = early || i + 1 != body.size();
            else early = early || has_nested_return(body[i].children);
        }
        early_cache[fn] = early;
        return early;
    }

    void count_desc(const std::vector<Region>& rs, std::vector<int>& path, int depth, int& total) {
        for (const auto& r : rs) {
            if (total >= kCountCap) return;
            if (r.kind == RegionKind::IndirectCall) {
                ++total;
            } else if (r.kind == RegionKind::Call) {
                if (r.target < 0) {
                    if (view.show_library) ++total;
                    continue;
                }
                ++total;
                bool rec = std::find(path.begin(), path.end(), r.target) != path.end();
                if (!rec && depth < kMaxDepth) {
                    path.push_back(r.target);
                    count_desc(m.functions[r.target].body, path, depth + 1, total);
                    path.pop_back();
                }
            } else {
                count_desc(r.children, path, depth, total);
            }
        }
    }

    static void scan_exits(const std::vector<Region>& rs, bool& brk, bool& cont) {
        for (const auto& r : rs) {
            if (r.kind == RegionKind::Break) brk = true;
            else if (r.kind == RegionKind::Continue) cont = true;
            else if (r.kind != RegionKind::Loop) scan_exits(r.children, brk, cont);
        }
    }

    static bool has_box(const Node& n) {
        if (n.kind == Node::Box) return true;
        for (const auto& k : n.kids)
            if (has_box(*k)) return true;
        return false;
    }
    static void collect_points(const Node& n, std::vector<const Node*>& pts) {
        if (n.kind == Node::Point) pts.push_back(&n);
        for (const auto& k : n.kids) collect_points(*k, pts);  // never enters Box bodies
    }
    static int matched_gotos(const Node& n) {
        std::vector<const Node*> pts;
        collect_points(n, pts);
        int count = 0;
        for (const Node* a : pts) {
            if (!a->is_goto) continue;
            for (const Node* b : pts)
                if (!b->is_goto && b->label == a->label) {
                    ++count;
                    break;
                }
        }
        return count;
    }

    std::string loop_label(const std::string& count) const {
        return utf8(g.times) + (count == "inf" ? std::string(g.inf) : count);
    }

    struct Scope {
        bool arrows;
        int depth;      // box depth for calls made here
        int arc_depth;  // arcs already enclosing this spot
        std::vector<int>& path;  // functions being expanded (recursion guard)
        std::string pid;         // id of the owning box
        int& ordinal;            // call-site counter within the owning function
    };

    NodeP build_seq(const std::vector<Region>& rs, const Scope& s) {
        auto seq = std::make_unique<Node>(Node::Seq, s.arrows);
        for (const auto& r : rs) add(*seq, r, s);
        return seq;
    }

    void finish_label(Node& b, const std::string& name) {
        std::string l;
        if (b.box.error) l += utf8(g.warn) + " ";
        l += truncate(name, 28, g.ellipsis);
        if (b.box.recursive) l += " " + utf8(g.rec);
        if (b.box.hidden > 0) l += " " + utf8(g.collapsed) + " " + std::to_string(b.box.hidden);
        b.box.label = l;
    }

    NodeP call_box(const Region& r, const std::string& id, const Scope& s) {
        const Function& f = m.functions[r.target];
        auto b = std::make_unique<Node>(Node::Box, s.arrows);
        b->box.id = id;
        b->box.style = BoxStyle::Normal;
        b->box.depth = s.depth;
        b->box.function = r.target;
        b->box.site = r.range;
        b->box.def = f.range;
        b->box.early_return = early_return(r.target);
        b->box.error = f.has_error || f.stale;
        --budget;
        if (std::find(s.path.begin(), s.path.end(), r.target) != s.path.end()) {
            b->box.recursive = true;
        } else if (view.collapsed.count(id) || s.depth >= kMaxDepth || budget <= 0) {
            int total = 0;
            s.path.push_back(r.target);
            count_desc(f.body, s.path, s.depth + 1, total);
            s.path.pop_back();
            b->box.hidden = total;
            b->box.collapsible = total > 0;
        } else {
            int ordinal = 0;
            s.path.push_back(r.target);
            Scope inner{false, s.depth + 1, s.arrows ? s.arc_depth : 0, s.path, id, ordinal};
            b->body = build_seq(f.body, inner);
            s.path.pop_back();
            b->box.collapsible = has_box(*b->body);
            b->goto_rows = matched_gotos(*b->body);
        }
        finish_label(*b, f.name);
        return b;
    }

    void add(Node& seq, const Region& r, const Scope& s) {
        switch (r.kind) {
            case RegionKind::Call: {
                std::string id = s.pid + "/" + r.name + "#" + std::to_string(s.ordinal++);
                if (r.target >= 0) {
                    seq.kids.push_back(call_box(r, id, s));
                } else if (view.show_library) {
                    auto b = std::make_unique<Node>(Node::Box, s.arrows);
                    b->box.id = id;
                    b->box.style = BoxStyle::Library;
                    b->box.depth = s.depth;
                    b->box.site = b->box.def = r.range;
                    b->box.label = truncate(r.name, 28, g.ellipsis);
                    seq.kids.push_back(std::move(b));
                }
                break;
            }
            case RegionKind::IndirectCall: {
                auto b = std::make_unique<Node>(Node::Box, s.arrows);
                b->box.id = s.pid + "/?" + std::to_string(s.ordinal++);
                b->box.style = BoxStyle::Indirect;
                b->box.depth = s.depth;
                b->box.site = b->box.def = r.range;
                b->box.label = "?(" + truncate(r.name, 20, g.ellipsis) + ")";
                size_t shown = std::min<size_t>(r.candidates.size(), 4);
                for (size_t i = 0; i < shown; ++i)
                    b->box.lines.push_back(truncate(r.candidates[i], 24, g.ellipsis));
                if (r.candidates.size() > shown)
                    b->box.lines.push_back("+" + std::to_string(r.candidates.size() - shown) + " more");
                seq.kids.push_back(std::move(b));
                break;
            }
            case RegionKind::Loop: {
                auto loop = std::make_unique<Node>(Node::Loop, s.arrows);
                loop->count = r.count;
                loop->label = loop_label(r.count);
                loop->src = r.range;
                scan_exits(r.children, loop->brk, loop->cont);
                Scope inner{s.arrows, s.depth, s.arc_depth + 1, s.path, s.pid, s.ordinal};
                NodeP body = build_seq(r.children, inner);
                if (body->kids.empty()) {
                    auto slot = std::make_unique<Node>(Node::Slot, s.arrows);
                    slot->box.id = s.pid + "/loop@" + std::to_string(r.range.line) + ":" +
                                   std::to_string(r.range.col);
                    slot->box.style = BoxStyle::Slot;
                    slot->box.depth = s.depth;
                    slot->box.site = slot->box.def = r.range;
                    slot->box.label = utf8(g.dot);
                    body->kids.push_back(std::move(slot));
                }
                bool numeric = !r.count.empty() && r.count != "?" && r.count != "inf";
                int label_w = std::max(ulen(loop->label), numeric ? 2 * (int)r.count.size() + 1 : 0);
                // corners, a dash and a space each side of the label: "╭─ ×2 ─╮",
                // plus room to step inside every enclosing arc
                loop->minw = label_w + 8 + 6 * s.arc_depth;
                loop->arc_depth = s.arc_depth;
                loop->kids.push_back(std::move(body));
                seq.kids.push_back(std::move(loop));
                break;
            }
            case RegionKind::If:
            case RegionKind::Switch: {
                auto fork = std::make_unique<Node>(Node::Fork, s.arrows);
                fork->src = r.range;
                bool covers_all = false, any = false;
                std::vector<NodeP> lanes;
                for (const auto& lane : r.children) {
                    NodeP ls = build_seq(lane.children, s);
                    ls->label = truncate(lane.name, s.arrows ? 18 : 14, g.ellipsis);
                    ls->src = lane.range;
                    if (!lane.children.empty()) {
                        RegionKind last = lane.children.back().kind;
                        ls->terminal = last == RegionKind::Return ? 1 : last == RegionKind::Goto ? 2 : 0;
                    }
                    if (lane.name == "else" || lane.name == "default" ||
                        (lane.name.size() > 9 && lane.name.rfind(", default") == lane.name.size() - 9))
                        covers_all = true;
                    any = any || !ls->kids.empty();
                    lanes.push_back(std::move(ls));
                }
                if (!any) break;  // nothing to draw in any branch
                if (s.arrows) {
                    // no else / default: execution can bypass every lane
                    if (!covers_all) lanes.push_back(std::make_unique<Node>(Node::Seq, true));
                    fork->kids = std::move(lanes);
                } else {
                    for (auto& l : lanes)
                        if (!l->kids.empty()) fork->kids.push_back(std::move(l));
                }
                seq.kids.push_back(std::move(fork));
                break;
            }
            case RegionKind::Goto:
            case RegionKind::Label: {
                auto p = std::make_unique<Node>(Node::Point, s.arrows);
                p->is_goto = r.kind == RegionKind::Goto;
                p->label = r.name;
                p->src = r.range;
                seq.kids.push_back(std::move(p));
                break;
            }
            default: break;  // Return / Break / Continue become markers, Lane is handled above
        }
    }

    // ------------------------------------------------------------ measure

    static int min_width(const LBox& b) {
        switch (b.style) {
            case BoxStyle::Entry:
            case BoxStyle::Library:
            case BoxStyle::Slot: return 5;
            default: return b.depth <= 1 ? 14 : b.depth == 2 ? 7 : 5;  // narrower with depth
        }
    }

    static int gap(const Node& prev, const Node& next) { return next.head ? (prev.boxy ? 4 : 3) : 2; }

    // Height of the stack of arcs drawn above a box for the loops in its packed body.
    static int arc_height(const Node& n) {
        if (n.kind == Node::Box || n.kind == Node::Point || n.kind == Node::Slot) return 0;
        int h = 0;
        for (const auto& k : n.kids) h = std::max(h, arc_height(*k));
        if (n.kind == Node::Loop) h = std::max(h + 1, own_arc_height(n.w, n.arc_depth));
        return h;
    }
    // Number of arc levels nested inside a node (0 = none), for lane numbers.
    static int arc_levels(const Node& n) {
        if (n.kind == Node::Point || n.kind == Node::Slot) return 0;
        if (n.kind == Node::Box) {
            // the arcs above a box on an unrolled row nest under that row's loops
            return n.arrows && n.body ? arc_levels(*n.body) : 0;
        }
        int h = 0;
        for (const auto& k : n.kids) h = std::max(h, arc_levels(*k));
        return n.kind == Node::Loop ? h + 1 : h;
    }

    // Widen the last box of a node so a loop arc has room. False if there is none.
    static bool stretch(Node& n, int extra) {
        switch (n.kind) {
            case Node::Box:
            case Node::Slot:
                if (n.kind == Node::Box || n.arrows) n.box.w += extra;
                n.w += extra;
                n.cw += extra;
                return true;
            case Node::Seq:
                if (n.kids.empty() || !stretch(*n.kids.back(), extra)) return false;
                n.w += extra;
                n.cw += extra;
                return true;
            case Node::Loop:
                if (n.w != n.kids[0]->w || !stretch(*n.kids[0], extra)) return false;
                n.w += extra;
                n.cw += extra;
                if (n.arrows) n.asc = std::max(n.kids[0]->asc + 1, own_arc_height(n.w, n.arc_depth));
                return true;
            default: return false;
        }
    }

    void measure(Node& n) {
        switch (n.kind) {
            case Node::Box: {
                int own = 0, body_w = 0, body_h = 0;
                if (n.body) {
                    measure(*n.body);
                    own = arc_height(*n.body);
                    if (!n.body->kids.empty()) {
                        body_w = n.body->w;
                        body_h = n.body->asc + n.body->desc;
                    }
                }
                int text_w = ulen(n.box.label);
                for (const auto& l : n.box.lines) text_w = std::max(text_w, ulen(l));
                n.box.h = 3 + (int)n.box.lines.size();
                n.box.w = std::max({min_width(n.box), text_w + 4, body_w});
                n.w = n.cw = n.box.w;
                n.asc = own;
                n.desc = n.box.h + body_h + n.goto_rows;
                n.head = n.boxy = true;
                break;
            }
            case Node::Slot:
                n.box.w = n.arrows ? 5 : 0;
                n.box.h = 3;
                n.w = n.cw = n.box.w;
                n.asc = 0;
                n.desc = n.arrows ? 3 : 0;
                n.head = n.boxy = n.arrows;
                break;
            case Node::Point:
                n.w = n.cw = 1;
                n.asc = 0;
                n.desc = n.arrows ? 2 : 3;
                break;
            case Node::Seq: {
                int x = 0;
                n.asc = 0;
                n.desc = n.arrows && n.kids.empty() ? 2 : 0;
                n.cw = 0;
                for (size_t i = 0; i < n.kids.size(); ++i) {
                    Node& k = *n.kids[i];
                    measure(k);
                    if (n.arrows && i > 0) x += gap(*n.kids[i - 1], k);
                    n.cw = x + k.cw;
                    x += k.w;
                    n.asc = std::max(n.asc, k.asc);
                    n.desc = std::max(n.desc, k.desc);
                }
                n.w = x;
                n.head = !n.kids.empty() && n.kids.front()->head;
                n.boxy = !n.kids.empty() && n.kids.back()->boxy;
                break;
            }
            case Node::Loop: {
                Node& inner = *n.kids[0];
                measure(inner);
                if (inner.w < n.minw) stretch(inner, n.minw - inner.w);
                n.w = std::max(inner.w, n.minw);
                n.cw = n.arrows ? inner.cw : n.w;
                n.asc = n.arrows ? std::max(inner.asc + 1, own_arc_height(n.w, n.arc_depth)) : inner.asc;
                n.desc = inner.desc;
                n.head = inner.head;
                n.boxy = inner.boxy;
                break;
            }
            case Node::Fork: {
                int max_label = 0, max_w = 0, max_asc = 0, max_desc = 0;
                for (auto& lane : n.kids) {
                    measure(*lane);
                    max_label = std::max(max_label, ulen(lane->label));
                    if (!n.arrows) lane->w = std::max(lane->w, ulen(lane->label) + 1);
                    max_w = std::max(max_w, lane->w);
                    max_asc = std::max(max_asc, lane->asc);
                    max_desc = std::max(max_desc, lane->desc);
                }
                if (n.arrows) {
                    // split column, "─ label ─▶", lane content, "──", merge column
                    n.cx = max_label + 6;
                    bool marks = false;  // a return marker needs a column of its own
                    for (auto& lane : n.kids) marks = marks || lane->terminal == 1;
                    n.w = n.cw = n.cx + max_w + 3 + (marks ? 1 : 0);
                    int base = 0;
                    for (size_t i = 0; i < n.kids.size(); ++i) {
                        Node& lane = *n.kids[i];
                        if (i > 0) {
                            const Node& prev = *n.kids[i - 1];
                            // an empty lane is a bare line tucked right under the previous lane
                            base += prev.desc + (lane.kids.empty() ? -1 : 1 + lane.asc);
                        }
                        lane.off = base;
                    }
                    n.asc = n.kids.front()->asc;
                    n.desc = base + n.kids.back()->desc;
                } else {
                    // lanes side by side, each under a one-row label
                    int total = 0;
                    for (auto& lane : n.kids) total += lane->w;
                    n.w = n.cw = total + (int)n.kids.size() - 1;
                    n.asc = max_asc + 1;
                    n.desc = max_desc;
                }
                n.head = n.boxy = false;
                break;
            }
        }
    }

    // ------------------------------------------------------------ place

    struct ArcRef {
        int L, R, x0, x1;
    };
    struct Pt {
        bool is_goto;
        std::string name;
        int x, y;
    };
    std::vector<Pt>* points = nullptr;
    int cur_parent = -1;

    void line(int x0, int y0, int x1, int y1, bool dashed = false, bool arrow = false) {
        out.lines.push_back({x0, y0, x1, y1, dashed, arrow});
    }
    void text(int x, int y, TextKind kind, const std::string& s = {}, SrcRange src = {}) {
        out.texts.push_back({x, y, s, kind, src});
    }

    ArcRef emit_arc(const Node& loop, int L, int R, int y_base, int height,
                    const std::vector<ArcRef>& stack, int owner) {
        ArcRef a{L, R, L + 1, R - 1};
        if (!stack.empty()) {
            // share a leg position with the enclosing arc? step inside it
            if (stack.back().L == L) a.x0 = stack.back().x0 + 3;
            if (stack.back().R == R) a.x1 = stack.back().x1 - 3;
        }
        LArc arc;
        arc.x0 = a.x0;
        arc.x1 = a.x1;
        arc.y_base = y_base;
        arc.y_top = y_base - height;
        arc.label = loop.label;
        arc.count = loop.count;
        arc.lane = arc_levels(*loop.kids[0]);
        arc.depth = (int)stack.size();
        arc.has_break = loop.brk;
        arc.has_continue = loop.cont;
        arc.src = loop.src;
        arc.owner = owner;
        out.arcs.push_back(std::move(arc));
        return a;
    }

    // Arcs for the loops in a box's packed body are drawn above the box itself,
    // spanning the columns of the children each loop wraps.
    void emit_body_arcs(const Node& n, std::vector<ArcRef>& stack, int y_base, int owner) {
        if (n.kind == Node::Box) return;
        if (n.kind == Node::Loop) {
            stack.push_back(emit_arc(n, n.px, n.px + n.w - 1, y_base, arc_height(n), stack, owner));
            emit_body_arcs(*n.kids[0], stack, y_base, owner);
            stack.pop_back();
            return;
        }
        for (const auto& k : n.kids) emit_body_arcs(*k, stack, y_base, owner);
    }

    void route_gotos(const std::vector<Pt>& pts, int bottom) {
        int row = bottom;
        for (const Pt& g0 : pts) {
            if (!g0.is_goto) continue;
            for (const Pt& l0 : pts) {
                if (l0.is_goto || l0.name != g0.name) continue;
                line(g0.x, g0.y + 1, g0.x, row, true);
                line(std::min(g0.x, l0.x), row, std::max(g0.x, l0.x), row, true);
                line(l0.x, row, l0.x, l0.y + 1, true, true);
                std::string name = " " + g0.name + " ";
                int span = std::abs(g0.x - l0.x) + 1;
                int tx = std::min(g0.x, l0.x) + std::max(1, (span - ulen(name)) / 2);
                text(tx, row, TextKind::GotoName, name);
                ++row;
                break;
            }
        }
    }

    void place(Node& n, int x, int base, std::vector<ArcRef>& stack) {
        switch (n.kind) {
            case Node::Box: {
                n.box.x = x;
                n.box.y = base;
                n.box.parent = cur_parent;
                int idx = (int)out.boxes.size();
                out.boxes.push_back(n.box);
                if (n.body) {
                    std::vector<Pt> mine;
                    std::vector<Pt>* saved_points = points;
                    int saved_parent = cur_parent;
                    points = &mine;
                    cur_parent = idx;
                    std::vector<ArcRef> none;
                    int body_base = base + n.box.h + n.body->asc;
                    place(*n.body, x, body_base, none);
                    points = saved_points;
                    cur_parent = saved_parent;
                    std::vector<ArcRef> own = n.arrows ? stack : std::vector<ArcRef>{};
                    emit_body_arcs(*n.body, own, base, idx);
                    route_gotos(mine, body_base + n.body->desc);
                }
                break;
            }
            case Node::Slot:
                if (n.arrows) {
                    n.box.x = x;
                    n.box.y = base;
                    n.box.parent = cur_parent;
                    out.boxes.push_back(n.box);
                }
                break;
            case Node::Point:
                text(x, base + 1, n.is_goto ? TextKind::GotoSite : TextKind::LabelSite, {}, n.src);
                if (points) points->push_back({n.is_goto, n.label, x, base + 1});
                break;
            case Node::Seq: {
                int cx = x, prev_end = x;
                for (size_t i = 0; i < n.kids.size(); ++i) {
                    Node& k = *n.kids[i];
                    if (n.arrows && i > 0) {
                        const Node& prev = *n.kids[i - 1];
                        int gp = gap(prev, k);
                        // start on the previous item's last cell unless that is a box edge
                        int from = prev.boxy ? prev_end : prev_end - 1;
                        int to = k.head ? cx + gp - 1 : cx + gp;
                        line(from, base + 1, to, base + 1, false, k.head);
                        cx += gp;
                    }
                    place(k, cx, base, stack);
                    if (k.kind == Node::Box && k.box.style == BoxStyle::Entry)
                        cur_parent = (int)out.boxes.size() - 1;
                    prev_end = cx + k.cw;
                    cx += k.w;
                }
                break;
            }
            case Node::Loop: {
                n.px = x;
                if (n.arrows) {
                    stack.push_back(emit_arc(n, x, x + n.w - 1, base, n.asc, stack, -1));
                    place(*n.kids[0], x, base, stack);
                    stack.pop_back();
                } else {
                    place(*n.kids[0], x, base, stack);
                }
                break;
            }
            case Node::Fork: {
                if (!n.arrows) {
                    int lx = x;
                    for (auto& lane : n.kids) {
                        text(lx, base - n.asc, TextKind::LaneLabel, lane->label, lane->src);
                        place(*lane, lx, base, stack);
                        lx += lane->w + 1;
                    }
                    break;
                }
                int split = x, content = x + n.cx, merge = x + n.w - 1;
                int first_join = INT_MIN, last_join = INT_MIN, last_mid = base + 1;
                for (auto& lp : n.kids) {
                    Node& lane = *lp;
                    int mid = base + lane.off + 1;
                    last_mid = mid;
                    place(lane, content, base + lane.off, stack);
                    bool empty = lane.kids.empty();
                    if (empty && !lane.terminal) {
                        line(split, mid, merge, mid);
                    } else {
                        line(split, mid, lane.head || empty ? content - 1 : content, mid, false, lane.head);
                        int tail = content + lane.cw - (lane.boxy || empty ? 0 : 1);
                        if (lane.terminal == 1) {
                            if (!empty) line(tail, mid, tail, mid);
                            text(empty ? content : tail + 1, mid, TextKind::ReturnMark);
                        } else if (lane.terminal == 2) {
                            // the goto site ends the lane; its dashed arrow carries on
                        } else {
                            line(tail, mid, merge, mid);
                        }
                    }
                    if (!lane.label.empty())
                        text(split + 2, mid, TextKind::LaneLabel, " " + lane.label + " ", lane.src);
                    if (!lane.terminal) {
                        if (first_join == INT_MIN) first_join = mid;
                        last_join = mid;
                    }
                }
                line(split, base + 1, split, last_mid);
                // lanes that carry on rejoin the row at the merge column
                if (first_join != INT_MIN && last_join > base + 1) line(merge, base + 1, merge, last_join);
                break;
            }
        }
    }

    // ------------------------------------------------------------ roots

    Layout run() {
        std::vector<NodeP> roots;
        std::vector<int> ids = view.order == OrderMode::Execution ? m.entry_points() : std::vector<int>{};
        if (view.order == OrderMode::Source)
            for (size_t i = 0; i < m.functions.size(); ++i) ids.push_back((int)i);

        for (int fi : ids) {
            const Function& f = m.functions[fi];
            std::vector<int> path{fi};
            int ordinal = 0;
            auto head = std::make_unique<Node>(Node::Box, view.order == OrderMode::Execution);
            head->box.id = f.qualified;
            head->box.function = fi;
            head->box.site = head->box.def = f.range;
            head->box.early_return = early_return(fi);
            head->box.error = f.has_error || f.stale;
            if (view.order == OrderMode::Execution) {
                // the entry's body is unrolled along the row
                head->box.style = BoxStyle::Entry;
                head->box.depth = 0;
                finish_label(*head, f.qualified);
                Scope s{true, 1, 0, path, f.qualified, ordinal};
                NodeP row = build_seq(f.body, s);
                head->box.collapsible = false;
                row->kids.insert(row->kids.begin(), std::move(head));
                row->goto_rows = matched_gotos(*row);
                roots.push_back(std::move(row));
            } else {
                head->box.style = BoxStyle::Normal;
                head->box.depth = 1;
                if (view.collapsed.count(f.qualified)) {
                    int total = 0;
                    count_desc(f.body, path, 2, total);
                    head->box.hidden = total;
                    head->box.collapsible = total > 0;
                } else {
                    Scope s{false, 2, 0, path, f.qualified, ordinal};
                    head->body = build_seq(f.body, s);
                    head->box.collapsible = has_box(*head->body);
                    head->goto_rows = matched_gotos(*head->body);
                }
                finish_label(*head, f.qualified);
                roots.push_back(std::move(head));
            }
        }

        int x = 0;
        for (auto& r : roots) {
            measure(*r);
            std::vector<ArcRef> stack;
            std::vector<Pt> pts;
            points = &pts;
            cur_parent = -1;
            place(*r, x, 0, stack);
            if (r->kind == Node::Seq) route_gotos(pts, r->desc);
            points = nullptr;
            x += r->w + (view.order == OrderMode::Execution ? 4 : 2);
        }
        normalize();
        return std::move(out);
    }

    void normalize() {
        int min_y = 0, max_x = 0, max_y = 0;
        auto see = [&](int x, int y) {
            min_y = std::min(min_y, y);
            max_x = std::max(max_x, x);
            max_y = std::max(max_y, y);
        };
        for (const auto& b : out.boxes) see(b.x + b.w - 1, b.y), see(b.x, b.y + b.h - 1);
        for (const auto& a : out.arcs) see(a.x1, a.y_top), see(a.x1, a.y_base - 1);
        for (const auto& l : out.lines) see(l.x0, l.y0), see(l.x1, l.y1);
        for (const auto& t : out.texts) see(t.x + std::max(1, ulen(t.text)) - 1, t.y);
        for (auto& b : out.boxes) b.y -= min_y;
        for (auto& a : out.arcs) a.y_top -= min_y, a.y_base -= min_y;
        for (auto& l : out.lines) l.y0 -= min_y, l.y1 -= min_y;
        for (auto& t : out.texts) t.y -= min_y;
        bool empty = out.boxes.empty() && out.arcs.empty() && out.lines.empty() && out.texts.empty();
        out.width = empty ? 0 : max_x + 1;
        out.height = empty ? 0 : max_y - min_y + 1;
    }
};

}  // namespace

Layout compute_layout(const Model& model, const ViewState& view) { return Engine(model, view).run(); }

int box_at(const Layout& l, int x, int y) {
    int best = -1;
    for (size_t i = 0; i < l.boxes.size(); ++i) {
        const LBox& b = l.boxes[i];
        if (x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h) best = (int)i;
    }
    return best;
}

int find_box(const Layout& l, const std::string& id) {
    for (size_t i = 0; i < l.boxes.size(); ++i)
        if (l.boxes[i].id == id) return (int)i;
    return -1;
}

int navigate(const Layout& l, int from, Dir dir) {
    if (from < 0 || from >= (int)l.boxes.size()) return l.boxes.empty() ? -1 : 0;
    const LBox& a = l.boxes[from];
    int best = from;
    long best_cost = LONG_MAX;
    for (size_t i = 0; i < l.boxes.size(); ++i) {
        if ((int)i == from) continue;
        const LBox& b = l.boxes[i];
        bool rows_overlap = b.y < a.y + a.h && a.y < b.y + b.h;
        bool cols_overlap = b.x < a.x + a.w && a.x < b.x + b.w;
        long cost = 0;
        switch (dir) {
            case Dir::Left:
                if (!rows_overlap || b.x >= a.x) continue;
                cost = (long)(a.x - b.x) * 4 + std::abs(a.y - b.y);
                break;
            case Dir::Right:
                if (!rows_overlap || b.x <= a.x) continue;
                cost = (long)(b.x - a.x) * 4 + std::abs(a.y - b.y);
                break;
            case Dir::Up:
                if (b.y + b.h > a.y) continue;
                cost = (long)(a.y - b.y) * 1000 + (cols_overlap ? 0 : 100000) + std::abs(a.x - b.x);
                // the owner is always the natural "up"
                if ((int)i == a.parent && cols_overlap) cost = -1;
                break;
            case Dir::Down:
                if (b.y < a.y + a.h) continue;
                cost = (long)(b.y - a.y) * 1000 + (cols_overlap ? 0 : 100000) + std::abs(a.x - b.x);
                break;
        }
        if (cost < best_cost) {
            best_cost = cost;
            best = (int)i;
        }
    }
    return best;
}

}  // namespace tracespace
