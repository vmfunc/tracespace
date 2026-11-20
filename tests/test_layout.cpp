#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <fstream>
#include <functional>
#include <set>
#include <sstream>

#include "core/layout.h"
#include "core/paint.h"
#include "core/parser.h"

using namespace tracespace;

namespace {

const std::string kDir = TRACESPACE_SOURCE_DIR "/tests/";

std::string slurp(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    std::string s = ss.str(), out;
    for (char c : s)
        if (c != '\r') out += c;
    return out;
}

Model parse(const std::string& text, const std::string& path = "t.cpp") {
    return make_tree_sitter_parser()->parse(path, text);
}

std::string render(const Model& m, ViewState v, PaintOptions po = {}) {
    po.theme = v.theme;
    return paint(compute_layout(m, v), po).to_text();
}

std::string render(const std::string& text, Theme theme = Theme::Rounded) {
    ViewState v;
    v.theme = theme;
    return render(parse(text), v);
}

// Compare against tests/snapshots/<name>.txt. Run the tests with
// TRACESPACE_UPDATE_SNAPSHOTS=1 to (re)write the files, then review the diff.
void check_snapshot(const std::string& name, const std::string& actual) {
    std::string path = kDir + "snapshots/" + name + ".txt";
    if (std::getenv("TRACESPACE_UPDATE_SNAPSHOTS")) {
        std::ofstream out(path, std::ios::binary);
        out << actual;
        SUCCEED("snapshot written: " + name);
        return;
    }
    INFO("snapshot: " << name);
    CHECK(actual == slurp(path));
}

const char* kExample = R"(void a1() {}
void a2() {}
void funcA() {
    for (int i = 0; i < 2; ++i) { a1(); a2(); }
}
void funcC() {}
int main() { funcA(); funcC(); return 0; }
)";

bool is_ascii(const std::string& s) {
    for (unsigned char c : s)
        if (c >= 0x80) return false;
    return true;
}

}  // namespace

TEST_CASE("basic example, rounded theme") {
    CHECK(render(kExample, Theme::Rounded) ==
          "             ╭─── ×2 ───╮\n"
          "             ▼          │\n"
          "┌──────┐    ┌────────────┐    ┌────────────┐\n"
          "│ main │───▶│   funcA    │───▶│   funcC    │\n"
          "└──────┘    └────────────┘    └────────────┘\n"
          "            ┌─────┐┌─────┐\n"
          "            │ a1  ││ a2  │\n"
          "            └─────┘└─────┘\n");
}

TEST_CASE("basic example in the ascii theme is pure 7-bit") {
    std::string out = render(kExample, Theme::Ascii);
    CHECK(is_ascii(out));
    CHECK(out ==
          "             .-~~ x2 ~~-.\n"
          "             v          |\n"
          "+------+    +------------+    +------------+\n"
          "| main |--->|   funcA    |--->|   funcC    |\n"
          "+------+    +------------+    +------------+\n"
          "            +-----++-----+\n"
          "            | a1  || a2  |\n"
          "            +-----++-----+\n");
}

TEST_CASE("braille theme draws the arc with braille dots computed from the curve") {
    std::string out = render(kExample, Theme::Braille);
    bool braille = false;
    for (char32_t c : decode(out)) braille = braille || (c > 0x2800 && c <= 0x28FF);
    CHECK(braille);
    CHECK(out.find("×2") != std::string::npos);
    CHECK(out.find("▼") != std::string::npos);

    // the dots form one connected curve from the right leg to the left leg
    Layout l = compute_layout(parse(kExample), ViewState{});
    REQUIRE(l.arcs.size() == 1);
    auto dots = arc_braille_dots(l.arcs[0]);
    REQUIRE(dots.size() > 8);
    for (size_t i = 1; i < dots.size(); ++i) {
        CHECK(std::abs(dots[i].first - dots[i - 1].first) <= 1);
        CHECK(std::abs(dots[i].second - dots[i - 1].second) <= 1);
    }
    CHECK(dots.front().first == 2 * l.arcs[0].x1);
    CHECK(dots.back().first == 2 * l.arcs[0].x0 + 1);
}

TEST_CASE("snapshots: every theme and both ordering modes") {
    for (const char* input : {"example", "branches", "constructs", "nested"}) {
        Model m = parse(slurp(kDir + "inputs/" + input + ".cpp"), std::string(input) + ".cpp");
        REQUIRE_FALSE(m.functions.empty());
        for (Theme t : {Theme::Rounded, Theme::Ascii, Theme::Braille}) {
            ViewState v;
            v.theme = t;
            check_snapshot(std::string(input) + "." + theme_name(t), render(m, v));
        }
        ViewState v;
        v.theme = Theme::Rounded;
        v.order = OrderMode::Source;
        v.show_library = true;
        check_snapshot(std::string(input) + ".source", render(m, v));
    }
}

TEST_CASE("lane assignment: nested loops get one lane per level and never cross") {
    Model m = parse(R"(void a() {} void b() {} void c() {} void d() {}
int main() {
  for (int i = 0; i < 2; ++i) {
    a();
    for (int j = 0; j < 3; ++j) {
      b();
      for (int k = 0; k < 4; ++k) {
        c();
        for (;;) { d(); }
      }
    }
  }
})");
    ViewState v;
    v.theme = Theme::Rounded;
    Layout l = compute_layout(m, v);
    REQUIRE(l.arcs.size() == 4);
    auto arc = [&](const std::string& count) -> const LArc& {
        for (const auto& a : l.arcs)
            if (a.count == count) return a;
        FAIL("no arc with count " << count);
        return l.arcs[0];
    };
    // innermost loop is lane 0; each enclosing loop sits one lane higher
    CHECK(arc("inf").lane == 0);
    CHECK(arc("4").lane == 1);
    CHECK(arc("3").lane == 2);
    CHECK(arc("2").lane == 3);
    CHECK(arc("2").depth == 0);
    CHECK(arc("inf").depth == 3);
    // outer arcs are strictly higher and strictly wider
    const char* order[] = {"inf", "4", "3", "2"};
    for (int i = 0; i + 1 < 4; ++i) {
        const LArc& in = arc(order[i]);
        const LArc& outer = arc(order[i + 1]);
        CHECK(outer.y_top < in.y_top);
        CHECK(outer.x0 < in.x0);
        CHECK(outer.x1 > in.x1);
        CHECK(outer.y_base == in.y_base);
    }
}

TEST_CASE("arcs never cross: any two are either side by side or properly nested") {
    for (const char* input : {"branches", "constructs", "nested"}) {
        Model m = parse(slurp(kDir + "inputs/" + input + ".cpp"));
        for (OrderMode mode : {OrderMode::Execution, OrderMode::Source}) {
            ViewState v;
            v.order = mode;
            Layout l = compute_layout(m, v);
            for (size_t i = 0; i < l.arcs.size(); ++i) {
                for (size_t j = i + 1; j < l.arcs.size(); ++j) {
                    const LArc &a = l.arcs[i], &b = l.arcs[j];
                    if (a.y_base != b.y_base) continue;  // different rows of boxes
                    bool disjoint = a.x1 < b.x0 || b.x1 < a.x0;
                    bool a_in_b = b.x0 < a.x0 && a.x1 < b.x1 && b.y_top < a.y_top;
                    bool b_in_a = a.x0 < b.x0 && b.x1 < a.x1 && a.y_top < b.y_top;
                    INFO(input << " arcs " << i << " and " << j);
                    CHECK((disjoint || a_in_b || b_in_a));
                }
            }
        }
    }
}

TEST_CASE("arc height scales with its span") {
    auto height = [](const std::string& body) {
        Model m = parse("void a() {} void b() {} void c() {} void d() {} void e() {}\nint main() { for (;;) { " +
                        body + " } }");
        Layout l = compute_layout(m, ViewState{});
        REQUIRE(l.arcs.size() == 1);
        return l.arcs[0].y_base - l.arcs[0].y_top;
    };
    CHECK(height("a();") == 2);                           // narrower than ~20 columns
    CHECK(height("a(); b();") == 3);                      // wider
    CHECK(height("a(); b(); c(); d(); e();") == 4);       // capped at 4 rows
    CHECK(height("a(); b(); c(); d(); e(); a(); b(); c(); d(); e();") == 4);
}

TEST_CASE("loop labels: literal count, unknown, infinite; break and continue markers") {
    std::string out = render(R"(void w() {}
int main() {
  for (int i = 0; i < 7; ++i) { w(); }
  for (int i = 0; i < n; ++i) { w(); if (i) break; }
  while (true) { w(); if (x) continue; }
})");
    CHECK(out.find("×7") != std::string::npos);
    CHECK(out.find("×?") != std::string::npos);
    CHECK(out.find("×∞") != std::string::npos);
    CHECK(out.find("⇥╮") != std::string::npos);  // break at the arc's right end
    CHECK(out.find("╭↺") != std::string::npos);  // continue at the arc's left end
    std::string ascii = render("void w() {}\nint main() { while (1) { w(); } }", Theme::Ascii);
    CHECK(ascii.find("xinf") != std::string::npos);
}

TEST_CASE("a loop in a caller spans the sibling calls it wraps") {
    Model m = parse("void a() {} void b() {} void c() {}\nint main() { a(); for (int i = 0; i < 3; ++i) { b(); c(); } }");
    Layout l = compute_layout(m, ViewState{});
    REQUIRE(l.arcs.size() == 1);
    int bi = find_box(l, "main/b#1"), ci = find_box(l, "main/c#2");
    REQUIRE(bi >= 0);
    REQUIRE(ci >= 0);
    CHECK(l.arcs[0].x0 == l.boxes[bi].x + 1);
    CHECK(l.arcs[0].x1 == l.boxes[ci].x + l.boxes[ci].w - 2);
    CHECK(l.arcs[0].y_base == l.boxes[bi].y);
    CHECK(l.arcs[0].owner == -1);
}

TEST_CASE("if/else forks into labelled lanes that merge again") {
    std::string out = render(R"(void a() {} void b() {} void c() {}
int main(int argc, char**) { if (argc > 1) a(); else b(); c(); })");
    CHECK(out ==
          "┌──────┐                     ┌────────────┐      ┌────────────┐\n"
          "│ main │──┬─ if (argc > 1) ─▶│     a      │──┬──▶│     c      │\n"
          "└──────┘  │                  └────────────┘  │   └────────────┘\n"
          "          │                                  │\n"
          "          │                  ┌────────────┐  │\n"
          "          └─ else ──────────▶│     b      │──┘\n"
          "                             └────────────┘\n");
}

TEST_CASE("an if without else gets a bypass lane; a lane ending in return is marked") {
    std::string out = render(R"(void a() {} void c() {}
int main(int argc, char**) { if (argc > 1) { a(); return 1; } c(); })");
    CHECK(out ==
          "┌──────┐                     ┌────────────┐       ┌────────────┐\n"
          "│ main ⏎──┬─ if (argc > 1) ─▶│     a      │─⏎ ┌──▶│     c      │\n"
          "└──────┘  │                  └────────────┘   │   └────────────┘\n"
          "          └───────────────────────────────────┘\n");
}

TEST_CASE("switch draws one lane per case and default") {
    std::string out = render(R"(void a() {} void b() {} void c() {}
int main(int argc, char**) { switch (argc) { case 1: a(); break; case 2: b(); break; default: c(); } })");
    CHECK(out.find("─ case 1 ──") != std::string::npos);
    CHECK(out.find("─ case 2 ──") != std::string::npos);
    CHECK(out.find("─ default ─") != std::string::npos);
}

TEST_CASE("markers: early return, recursion, parse error, indirect call") {
    std::string out = render(R"(struct S { virtual void go() {} };
struct T : S { void go() override {} };
int fact(int n) { if (n < 2) return 1; return n * fact(n - 1); }
void broken() { fact(; }
int main() { S* s; fact(3); s->go(); broken(); })");
    CHECK(out.find("fact ↻") != std::string::npos);
    CHECK(out.find("fact    ⏎") != std::string::npos);
    CHECK(out.find("⚠ broken") != std::string::npos);
    CHECK(out.find("?(s->go)") != std::string::npos);
    CHECK(out.find("S::go") != std::string::npos);
    CHECK(out.find("T::go") != std::string::npos);
    CHECK(out.find("╌╌") != std::string::npos);  // dashed border
}

TEST_CASE("goto draws a dashed arrow to its label, carrying the label name") {
    std::string out = render(R"(void a() {} void b() {}
int main(int argc, char**) { if (argc) goto done; a(); done: b(); })");
    CHECK(out.find("○") != std::string::npos);
    CHECK(out.find("◆") != std::string::npos);
    CHECK(out.find("╌ done ╌") != std::string::npos);
    CHECK(out.find("▲") != std::string::npos);
}

TEST_CASE("library calls are hidden until toggled on") {
    Model m = parse("#include <cstdio>\nvoid a() {}\nint main() { printf(\"x\"); a(); }");
    ViewState v;
    v.theme = Theme::Rounded;
    CHECK(render(m, v).find("printf") == std::string::npos);
    v.show_library = true;
    CHECK(render(m, v).find("printf") != std::string::npos);
    Layout l = compute_layout(m, v);
    int p = find_box(l, "main/printf#0");
    REQUIRE(p >= 0);
    CHECK(l.boxes[p].style == BoxStyle::Library);
    CHECK(find_box(l, "main/a#1") >= 0);  // ids do not shift when the toggle changes
}

TEST_CASE("collapse hides children and shows how many descendants are hidden") {
    Model m = parse(kExample);
    ViewState v;
    v.theme = Theme::Rounded;
    Layout open = compute_layout(m, v);
    int fa = find_box(open, "main/funcA#0");
    REQUIRE(fa >= 0);
    CHECK(open.boxes[fa].collapsible);
    CHECK(find_box(open, "main/funcA#0/a1#0") >= 0);
    CHECK_FALSE(open.boxes[find_box(open, "main/funcC#1")].collapsible);

    v.collapsed.insert("main/funcA#0");
    Layout shut = compute_layout(m, v);
    CHECK(find_box(shut, "main/funcA#0/a1#0") < 0);
    fa = find_box(shut, "main/funcA#0");
    CHECK(shut.boxes[fa].hidden == 2);
    CHECK(shut.boxes[fa].label == "funcA ▸ 2");
    CHECK(shut.arcs.empty());
}

TEST_CASE("source order puts every definition on the top row") {
    Model m = parse(kExample);
    ViewState v;
    v.order = OrderMode::Source;
    Layout l = compute_layout(m, v);
    std::vector<std::string> top;
    for (const auto& b : l.boxes)
        if (b.parent < 0) top.push_back(b.id);
    CHECK(top == std::vector<std::string>{"a1", "a2", "funcA", "funcC", "main"});
    int mi = find_box(l, "main");
    int child = find_box(l, "main/funcA#0");
    REQUIRE(child >= 0);
    CHECK(l.boxes[child].parent == mi);
    CHECK(l.boxes[child].y == l.boxes[mi].y + 3 + 2);  // below main, under funcA's own arc
}

TEST_CASE("execution order without main starts from the uncalled functions") {
    Model m = parse("void leaf() {}\nvoid first() { leaf(); }\nvoid second() { leaf(); }\n");
    Layout l = compute_layout(m, ViewState{});
    REQUIRE(l.boxes.size() == 4);
    CHECK(l.boxes[0].id == "first");
    CHECK(l.boxes[0].style == BoxStyle::Entry);
    CHECK(l.boxes[2].id == "second");
    CHECK(l.boxes[2].x > l.boxes[1].x);
}

TEST_CASE("children get narrower with depth") {
    Model m = parse("void c() {}\nvoid b() { c(); }\nvoid a() { b(); }\nint main() { a(); }");
    Layout l = compute_layout(m, ViewState{});
    CHECK(l.boxes[find_box(l, "main/a#0")].w == 14);
    CHECK(l.boxes[find_box(l, "main/a#0/b#0")].w == 7);
    CHECK(l.boxes[find_box(l, "main/a#0/b#0/c#0")].w == 5);
}

TEST_CASE("keyboard navigation and hit testing") {
    Layout l = compute_layout(parse(kExample), ViewState{});
    int mn = find_box(l, "main"), fa = find_box(l, "main/funcA#0"), fc = find_box(l, "main/funcC#1");
    int a1 = find_box(l, "main/funcA#0/a1#0"), a2 = find_box(l, "main/funcA#0/a2#1");
    CHECK(navigate(l, mn, Dir::Right) == fa);
    CHECK(navigate(l, fa, Dir::Right) == fc);
    CHECK(navigate(l, fc, Dir::Right) == fc);
    CHECK(navigate(l, fc, Dir::Left) == fa);
    CHECK(navigate(l, fa, Dir::Down) == a1);
    CHECK(navigate(l, a1, Dir::Right) == a2);
    CHECK(navigate(l, a2, Dir::Up) == fa);
    CHECK(navigate(l, mn, Dir::Up) == mn);
    CHECK(box_at(l, l.boxes[a2].x + 1, l.boxes[a2].y + 1) == a2);
    CHECK(box_at(l, 9, 3) == -1);  // on the arrow between main and funcA
}

TEST_CASE("selected loop is drawn heavy and animates a runner with the pass count") {
    Model m = parse(kExample);
    ViewState v;
    v.theme = Theme::Rounded;
    Layout l = compute_layout(m, v);
    int fa = find_box(l, "main/funcA#0");
    std::vector<int> hot = pick_hot_arcs(l, fa, 0, 0);
    REQUIRE(hot == std::vector<int>{0});
    CHECK(pick_hot_arcs(l, find_box(l, "main/funcC#1"), 0, 0).empty());
    // the editor cursor inside the loop wins over the selection
    CHECK(pick_hot_arcs(l, -1, 4, 10) == std::vector<int>{0});
    CHECK(pick_hot_arcs(l, -1, 7, 2).empty());

    PaintOptions po;
    po.theme = Theme::Rounded;
    po.hot_arcs = hot;
    std::string still = paint(l, po).to_text();
    CHECK(still.find("┏━━━ ×2 ━━━┓") != std::string::npos);
    CHECK(still.find("┃") != std::string::npos);

    auto path = arc_path(l.arcs[0], Theme::Rounded);
    CHECK(path.front() == std::make_pair(l.arcs[0].x1, l.arcs[0].y_base - 1));
    CHECK(path.back() == std::make_pair(l.arcs[0].x0, l.arcs[0].y_base - 1));
    po.tick = 0;
    std::string first = paint(l, po).to_text();
    CHECK(first.find("1/2") != std::string::npos);
    CHECK(first.find("●") != std::string::npos);
    po.tick = (long)path.size();
    CHECK(paint(l, po).to_text().find("2/2") != std::string::npos);
    po.tick = 2 * (long)path.size();
    CHECK(paint(l, po).to_text().find("1/2") != std::string::npos);  // repeats

    // the runner moves from frame to frame
    po.tick = 1;
    Grid g1 = paint(l, po);
    po.tick = 2;
    Grid g2 = paint(l, po);
    CHECK(g1.to_text() != g2.to_text());
}

TEST_CASE("unknown and infinite loops animate without a count") {
    Model m = parse("void w() {}\nint main() { while (true) { w(); } }");
    Layout l = compute_layout(m, ViewState{});
    PaintOptions po;
    po.theme = Theme::Rounded;
    po.hot_arcs = {0};
    po.tick = 3;
    std::string out = paint(l, po).to_text();
    CHECK(out.find("×∞") != std::string::npos);
    CHECK(out.find("●") != std::string::npos);
    CHECK(out.find("/") == std::string::npos);
}

TEST_CASE("empty input and files with no functions give an empty layout") {
    Layout l = compute_layout(parse(""), ViewState{});
    CHECK(l.width == 0);
    CHECK(l.height == 0);
    CHECK(paint(l, PaintOptions{}).to_text().empty());
    CHECK(compute_layout(parse("int x = 3;\n"), ViewState{}).boxes.empty());
}

TEST_CASE("layout is fast enough for live use on a 5,000-line file") {
    std::string src;
    for (int i = 0; i < 500; ++i) {
        src += "int f" + std::to_string(i) + "(int n) {\n  int s = 0;\n  for (int i = 0; i < n; ++i) {\n"
               "    if (i % 2) {\n      s += i;\n    } else {\n      s -= i;\n    }\n  }\n";
        if (i > 0) src += "  s += f" + std::to_string(i - 1) + "(n);\n";
        src += "  return s;\n}\n";
    }
    src += "int main() {\n";
    for (int i = 0; i < 500; i += 50) src += "  f" + std::to_string(i) + "(1);\n";
    src += "}\n";
    Model m = parse(src);
    CHECK(m.functions.size() == 501);
    Layout l = compute_layout(m, ViewState{});
    CHECK(l.boxes.size() > 10);
    CHECK(l.boxes.size() < 10000);  // deep chains are collapsed, not expanded forever
}

TEST_CASE("examples/ covers every construct and renders in every theme and order") {
    const std::string dir = TRACESPACE_SOURCE_DIR "/examples/";
    std::set<RegionKind> kinds;
    std::set<std::string> counts;
    bool candidates = false, library = false, recursive = false, early = false;
    for (const char* name : {"basics.cpp", "control_flow.c", "dispatch.cpp", "broken.cpp"}) {
        std::string text = slurp(dir + name);
        REQUIRE_FALSE(text.empty());
        Model m = parse(text, name);
        INFO(name);
        CHECK_FALSE(m.functions.empty());
        CHECK(m.find("main") >= 0);
        // only the deliberately broken example has parse errors
        CHECK(m.errors.empty() == (std::string(name) != "broken.cpp"));

        std::function<void(const std::vector<Region>&)> visit = [&](const std::vector<Region>& rs) {
            for (const auto& r : rs) {
                kinds.insert(r.kind);
                if (r.kind == RegionKind::Loop) counts.insert(r.count == "?" || r.count == "inf" ? r.count : "N");
                if (r.kind == RegionKind::IndirectCall && !r.candidates.empty()) candidates = true;
                if (r.kind == RegionKind::Call && r.target < 0) library = true;
                visit(r.children);
            }
        };
        for (const auto& f : m.functions) visit(f.body);

        for (Theme t : {Theme::Braille, Theme::Rounded, Theme::Ascii}) {
            for (OrderMode o : {OrderMode::Execution, OrderMode::Source}) {
                ViewState v;
                v.theme = t;
                v.order = o;
                v.show_library = true;
                Layout l = compute_layout(m, v);
                CHECK(l.boxes.size() >= m.functions.size());
                CHECK_FALSE(paint(l, PaintOptions{t}).to_text().empty());
                for (const auto& b : l.boxes) {
                    recursive = recursive || b.recursive;
                    early = early || b.early_return;
                }
            }
        }
    }
    for (RegionKind k : {RegionKind::Call, RegionKind::IndirectCall, RegionKind::Loop, RegionKind::If,
                         RegionKind::Switch, RegionKind::Lane, RegionKind::Return, RegionKind::Break,
                         RegionKind::Continue, RegionKind::Goto, RegionKind::Label}) {
        INFO("region kind " << (int)k);
        CHECK(kinds.count(k) == 1);
    }
    CHECK(counts == std::set<std::string>{"?", "N", "inf"});
    CHECK(candidates);  // function pointers / virtual calls with known targets
    CHECK(library);
    CHECK(recursive);
    CHECK(early);
}
