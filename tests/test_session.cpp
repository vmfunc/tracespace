#include <catch2/catch_test_macros.hpp>

#include "core/parser.h"
#include "core/session.h"

using namespace tracespace;

namespace {

Model parse(const std::string& text, const std::string& path = "/src/demo.cpp") {
    return make_tree_sitter_parser()->parse(path, text);
}

const char* kExample = R"(void a1() {}
void a2() {}
void funcA() {
    for (int i = 0; i < 2; ++i) { a1(); a2(); }
}
void funcC() {}
int main() { funcA(); funcC(); return 0; }
)";

}  // namespace

TEST_CASE("session: selection starts on the entry box and moves with the arrow keys") {
    Session s;
    s.set_model(parse(kExample));
    CHECK(s.selected_id() == "main");
    s.move(Dir::Right);
    CHECK(s.selected_id() == "main/funcA#0");
    s.move(Dir::Down);
    CHECK(s.selected_id() == "main/funcA#0/a1#0");
    s.move(Dir::Right);
    CHECK(s.selected_id() == "main/funcA#0/a2#1");
    s.move(Dir::Up);
    CHECK(s.selected_id() == "main/funcA#0");
    s.move(Dir::Left);
    s.move(Dir::Left);  // nowhere further to go
    CHECK(s.selected_id() == "main");
}

TEST_CASE("session: mouse click selects the box under it") {
    Session s;
    s.set_model(parse(kExample));
    const LBox& c = s.layout().boxes[find_box(s.layout(), "main/funcC#1")];
    s.select_at(c.x + 2, c.y + 1);
    CHECK(s.selected_id() == "main/funcC#1");
    s.select_at(c.x + c.w + 1, c.y + 1);  // empty space: selection unchanged
    CHECK(s.selected_id() == "main/funcC#1");
}

TEST_CASE("session: selection survives edits and falls back to the nearest ancestor") {
    Session s;
    s.set_model(parse(kExample));
    s.select_id("main/funcA#0/a2#1");
    // an unrelated edit: the same box stays selected
    s.set_model(parse(std::string(kExample) + "\nvoid extra() {}\n"));
    CHECK(s.selected_id() == "main/funcA#0/a2#1");
    // a2 is no longer called from funcA: fall back to funcA
    s.set_model(parse("void a1() {}\nvoid funcA() { a1(); }\nvoid funcC() {}\nint main() { funcA(); funcC(); }\n"));
    CHECK(s.selected_id() == "main/funcA#0");
    // everything gone
    s.set_model(parse("int x;\n"));
    CHECK(s.selected() == -1);
    s.set_model(parse(kExample));
    CHECK(s.selected_id() == "main");
}

TEST_CASE("session: space collapses and expands, and only boxes with children") {
    Session s;
    s.set_model(parse(kExample));
    s.select_id("main/funcA#0");
    s.toggle_collapse();
    CHECK(find_box(s.layout(), "main/funcA#0/a1#0") < 0);
    CHECK(s.layout().boxes[s.selected()].label == "funcA ▸ 2");
    CHECK(s.selected_id() == "main/funcA#0");
    s.toggle_collapse();
    CHECK(find_box(s.layout(), "main/funcA#0/a1#0") >= 0);
    s.select_id("main/funcC#1");
    s.toggle_collapse();  // no children: nothing happens
    CHECK(s.view.collapsed.empty());
}

TEST_CASE("session: tab switches ordering mode and stays on the same function") {
    Session s;
    s.set_model(parse(kExample));
    s.select_id("main/funcA#0");
    s.toggle_order();
    CHECK(s.view.order == OrderMode::Source);
    REQUIRE(s.selected() >= 0);
    CHECK(s.layout().boxes[s.selected()].label == "funcA");
    s.toggle_order();
    CHECK(s.view.order == OrderMode::Execution);
    CHECK(s.layout().boxes[s.selected()].label == "funcA");
}

TEST_CASE("session: T cycles themes; without UTF-8 the ascii theme is locked") {
    Session s;
    s.set_model(parse(kExample));
    CHECK(s.view.theme == Theme::Braille);
    s.cycle_theme();
    CHECK(s.view.theme == Theme::Rounded);
    s.cycle_theme();
    CHECK(s.view.theme == Theme::Ascii);
    s.cycle_theme();
    CHECK(s.view.theme == Theme::Braille);

    Session plain;
    plain.utf8 = false;
    plain.set_model(parse(kExample));
    CHECK(plain.view.theme == Theme::Ascii);
    plain.cycle_theme();
    CHECK(plain.view.theme == Theme::Ascii);
    for (unsigned char ch : plain.render(0).to_text()) CHECK(ch < 0x80);
    for (unsigned char ch : plain.status()) CHECK(ch < 0x80);
}

TEST_CASE("session: L toggles library calls") {
    Session s;
    s.set_model(parse("#include <cstdio>\nint main() { puts(\"hi\"); }\n"));
    CHECK(s.layout().boxes.size() == 1);
    s.toggle_library();
    CHECK(s.layout().boxes.size() == 2);
    CHECK(s.status().find("lib") != std::string::npos);
    s.toggle_library();
    CHECK(s.layout().boxes.size() == 1);
}

TEST_CASE("session: wide diagrams scroll sideways and keep the selection in view") {
    std::string src;
    for (int i = 0; i < 12; ++i) src += "void step" + std::to_string(i) + "() {}\n";
    src += "int main() {\n";
    for (int i = 0; i < 12; ++i) src += "  step" + std::to_string(i) + "();\n";
    src += "}\n";
    Session s;
    s.set_model(parse(src));
    const int view_w = 40, view_h = 10;
    REQUIRE(s.layout().width > view_w * 3);
    s.fit(view_w, view_h);
    CHECK(s.scroll_x() == 0);

    // moving the selection off-screen scrolls the view to it
    for (int i = 0; i < 8; ++i) s.move(Dir::Right);
    s.fit(view_w, view_h);
    const LBox& b = s.layout().boxes[s.selected()];
    CHECK(b.x >= s.scroll_x());
    CHECK(b.x + b.w <= s.scroll_x() + view_w);

    // manual scrolling is not undone by the next frame, and is clamped
    int before = s.scroll_x();
    s.scroll_by(-8);
    s.fit(view_w, view_h);
    CHECK(s.scroll_x() == before - 8);
    s.scroll_by(-100000);
    s.fit(view_w, view_h);
    CHECK(s.scroll_x() == 0);
    s.scroll_by(100000);
    s.fit(view_w, view_h);
    CHECK(s.scroll_x() == s.layout().width - view_w);
    // rows never wrap: the diagram is still three rows of boxes tall
    CHECK(s.layout().height == 3);
}

TEST_CASE("session: Enter jumps to the selected box's code") {
    Session s;
    s.set_model(parse(kExample));
    s.select_id("main/funcA#0");
    Session::Jump j = s.jump_target();
    CHECK(j.ok);
    CHECK(j.path == "/src/demo.cpp");
    CHECK(j.line == 3);  // funcA's definition
    CHECK(j.col == 0);
    s.select_id("main");
    CHECK(s.jump_target().line == 7);

    Session lib;
    lib.view.show_library = true;
    lib.set_model(parse("#include <cstdio>\nint main() {\n  puts(\"hi\");\n}\n"));
    lib.select_id("main/puts#0");
    CHECK(lib.jump_target().line == 3);  // a library call jumps to the call site
    CHECK(lib.jump_target().col == 2);

    CHECK_FALSE(Session().jump_target().ok);
}

TEST_CASE("session: the editor cursor highlights its function and the innermost loop") {
    Session s;
    s.set_model(parse(kExample));
    s.set_cursor(4, 10);  // inside funcA's for loop
    Grid g = s.render(-1);
    const LBox& fa = s.layout().boxes[find_box(s.layout(), "main/funcA#0")];
    const LBox& fc = s.layout().boxes[find_box(s.layout(), "main/funcC#1")];
    CHECK(g.at(fa.x, fa.y)->color == kColorCursor);
    CHECK(g.at(fc.x, fc.y)->color == kColorDefault);
    const LArc& arc = s.layout().arcs[0];
    CHECK((g.at(arc.x1, arc.y_top)->attr & kBold));  // the loop is emphasised
    CHECK(s.animation_visible(80, 24));

    s.set_cursor(7, 3);  // inside main, outside any loop
    g = s.render(-1);
    CHECK(g.at(fa.x, fa.y)->color == kColorDefault);
    CHECK(g.at(0, s.layout().boxes[0].y)->color == kColorSelect);  // main is also the selection
    CHECK_FALSE((g.at(arc.x1, arc.y_top)->attr & kBold));
    CHECK_FALSE(s.animation_visible(80, 24));

    s.set_cursor(0, 0);  // editor went away
    CHECK(s.cursor_line() == 0);
}

TEST_CASE("session: animation only runs for a visible selected loop, and never with --no-anim") {
    Session s;
    s.set_model(parse(kExample));
    CHECK_FALSE(s.animation_visible(80, 24));  // main selected: no loop
    s.select_id("main/funcA#0");
    CHECK(s.animation_visible(80, 24));
    CHECK(s.render(0).to_text().find("1/2") != std::string::npos);
    CHECK_FALSE(s.animation_visible(10, 24));  // the arc is scrolled out of a narrow view
    s.animate = false;
    CHECK_FALSE(s.animation_visible(80, 24));
    CHECK(s.render(5).to_text().find("1/2") == std::string::npos);
}

TEST_CASE("session: status bar shows file, mode, theme, connection and parse errors") {
    Session s;
    s.view.theme = Theme::Rounded;
    s.set_model(parse(kExample));
    CHECK(s.status() == " demo.cpp │ execution │ rounded │ watching file");
    s.connection = "nvim ●";
    s.toggle_order();
    s.set_model(parse("int main() { foo(; }\n"));
    std::string st = s.status();
    CHECK(st.find("source") != std::string::npos);
    CHECK(st.find("nvim ●") != std::string::npos);
    CHECK(st.find("parse error") != std::string::npos);
    s.message = "demo.cpp:3";
    CHECK(s.status().find("demo.cpp:3") != std::string::npos);
}

TEST_CASE("jump command template") {
    CHECK(expand_jump_cmd("nvim --server $NVIM --remote-send ':{line}<CR>'", "a.cpp", 12, 3) ==
          "nvim --server $NVIM --remote-send ':12<CR>'");
    CHECK(expand_jump_cmd("code -g {file}:{line}:{col}", "/x/a b.cpp", 7, 0) == "code -g /x/a b.cpp:7:0");
    CHECK(expand_jump_cmd("{line}{line} {unknown} {", "f", 1, 2) == "11 {unknown} {");
}
