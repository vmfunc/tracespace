#include <catch2/catch_test_macros.hpp>

#include "core/model.h"
#include "core/parser.h"

using namespace tracespace;

namespace {
Model parse(const std::string& text, const std::string& path = "t.cpp") {
    return make_tree_sitter_parser()->parse(path, text);
}
std::string ir(const std::string& text, const std::string& path = "t.cpp") {
    return dump(parse(text, path), false);
}
}  // namespace

TEST_CASE("basic example: calls, a counted loop and a return") {
    const char* src = R"(void a1() {}
void a2() {}
void funcA() {
    for (int i = 0; i < 2; ++i) { a1(); a2(); }
}
void funcC() {}
int main() { funcA(); funcC(); return 0; }
)";
    CHECK(ir(src) == R"(fn a1
fn a2
fn funcA
  loop for x2
    call a1
    call a2
fn funcC
fn main
  call funcA
  call funcC
  return
)");
    Model m = parse(src);
    CHECK(m.errors.empty());
    CHECK(m.entry_points() == std::vector<int>{4});
    CHECK(m.functions[2].range == SrcRange{3, 0, 5, 1});
    const Region& loop = m.functions[2].body[0];
    CHECK(loop.range.line == 4);
    CHECK(loop.children[0].target == 0);
    CHECK(loop.children[0].range == SrcRange{4, 34, 4, 38});
}

TEST_CASE("loop iteration counts") {
    auto count = [](const std::string& head) {
        Model m = parse("void f() { " + head + " { g(); } }");
        REQUIRE(m.functions.size() == 1);
        REQUIRE(!m.functions[0].body.empty());
        return m.functions[0].body.back().count;
    };
    CHECK(count("for (int i = 0; i < 2; ++i)") == "2");
    CHECK(count("for (int i = 0; i <= 2; i++)") == "3");
    CHECK(count("for (i = 1; i < 10; i += 3)") == "3");
    CHECK(count("for (int i = 10; i > 0; --i)") == "10");
    CHECK(count("for (int i = 10; i >= 0; i -= 5)") == "3");
    CHECK(count("for (int i = 0; i != 4; ++i)") == "4");
    CHECK(count("for (unsigned i = 0u; i < 0x10; ++i)") == "16");
    CHECK(count("for (int i = 0; i < n; ++i)") == "?");
    CHECK(count("for (int i = 0; i < 4; i *= 2)") == "?");
    CHECK(count("for (auto& x : xs)") == "?");
    CHECK(count("for (;;)") == "inf");
    CHECK(count("while (true)") == "inf");
    CHECK(count("while (1)") == "inf");
    CHECK(count("while (x)") == "?");
}

TEST_CASE("do-while loops") {
    CHECK(ir("void f() { do { g(); } while (x); do { h(); } while (0); }") == R"(fn f
  loop do x?
    call g [lib]
  loop do x1
    call h [lib]
)");
}

TEST_CASE("for initializer runs before the loop, condition inside it") {
    CHECK(ir("int init(); bool more(int); void step();\n"
             "void f() { for (int i = init(); more(i); ++i) step(); }") == R"(fn f
  call init [lib]
  loop for x?
    call more [lib]
    call step [lib]
)");
}

TEST_CASE("nested loops") {
    CHECK(ir(R"(void a() {}
void f() {
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 2; ++j) {
      while (true) { a(); }
    }
    a();
  }
})") == R"(fn a
fn f
  loop for x3
    loop for x2
      loop while xinf
        call a
    call a
)");
}

TEST_CASE("if / else if / else lanes") {
    CHECK(ir(R"(void a() {} void b() {} void c() {} int check() { return 1; }
void f(int x) {
  if (check() > 0) { a(); }
  else if (x  ==  2) b();
  else { c(); }
})") == R"(fn a
fn b
fn c
fn check
  return
fn f
  call check
  if
    lane if (check() > 0)
      call a
    lane else if (x == 2)
      call b
    lane else
      call c
)");
}

TEST_CASE("if without else has a single lane") {
    CHECK(ir("void a(); void f(int x) { if (x) a(); }") == R"(fn f
  if
    lane if (x)
      call a [lib]
)");
}

TEST_CASE("switch: one lane per case, fall-through labels merged, break not an exit") {
    CHECK(ir(R"(void a() {} void b() {} void c() {}
void f(int x) {
  switch (x) {
    case 1: a(); break;
    case 2:
    case 3: b(); break;
    default: c();
  }
})") == R"(fn a
fn b
fn c
fn f
  switch (x)
    lane case 1
      call a
    lane case 2, 3
      call b
    lane default
      call c
)");
}

TEST_CASE("early exits: return, break, continue") {
    CHECK(ir(R"(void work() {}
int f(int n) {
  for (int i = 0; i < n; ++i) {
    if (i == 3) continue;
    if (i == 7) break;
    if (i == 9) return 1;
    work();
  }
  return 0;
})") == R"(fn work
fn f
  loop for x?
    if
      lane if (i == 3)
        continue
    if
      lane if (i == 7)
        break
    if
      lane if (i == 9)
        return
    call work
  return
)");
}

TEST_CASE("break inside a switch inside a loop belongs to the switch; continue to the loop") {
    CHECK(ir(R"(void f(int x) {
  while (x) {
    switch (x) { case 1: break; case 2: continue; }
  }
})") == R"(fn f
  loop while x?
    switch (x)
      lane case 1
      lane case 2
        continue
)");
}

TEST_CASE("goto and labels") {
    CHECK(ir(R"(void cleanup() {} int open_it() { return 0; }
int f() {
  if (open_it() < 0) goto fail;
  return 0;
fail:
  cleanup();
  return -1;
})", "t.c") == R"(fn cleanup
fn open_it
  return
fn f
  call open_it
  if
    lane if (open_it() < 0)
      goto fail
  return
  label fail
  call cleanup
  return
)");
}

TEST_CASE("function pointers are indirect calls with candidates found by name") {
    CHECK(ir(R"(void on_a(int) {}
void on_b(int) {}
void (*handler)(int) = on_a;
void (*table[2])(int) = { on_a, &on_b };
struct Ops { void (*run)(int); };
void call_param(void (*cb)(int)) { cb(1); }
void f(Ops* ops) {
  handler = &on_b;
  handler(1);
  (*handler)(2);
  table[0](3);
  ops->run(4);
  auto local = on_a;
  local(5);
})") == R"(fn on_a
fn on_b
fn call_param
  indirect ?(cb)
fn f
  indirect ?(handler) -> {on_a, on_b}
  indirect ?(*handler) -> {on_a, on_b}
  indirect ?(table[0]) -> {on_a, on_b}
  indirect ?(ops->run)
  indirect ?(local) -> {on_a}
)");
}

TEST_CASE("function pointers with an empty parameter list (parsed as expressions by tree-sitter-cpp)") {
    CHECK(ir(R"(void on_start() {}
void on_stop() {}
void (*hook)() = on_start;
void (*bare)();
void f() {
  void (*local)() = on_stop;
  void (*unset)();
  hook = on_stop;
  hook();
  bare();
  local();
  unset();
})") == R"(fn on_start
fn on_stop
fn f
  indirect ?(hook) -> {on_start, on_stop}
  indirect ?(bare)
  indirect ?(local) -> {on_stop}
  indirect ?(unset)
)");
}

TEST_CASE("virtual calls list every override in the file") {
    CHECK(ir(R"(struct Shape {
  virtual double area() const { return 0; }
  int id() { return 1; }
};
struct Circle : Shape { double area() const override { return 3.14; } };
struct Square : Shape { double area() const override; };
double Square::area() const { return 1; }
double total(Shape* s, Shape& r) { return s->area() + r.area() + s->id(); }
)") == R"(fn Shape::area
  return
fn Shape::id
  return
fn Circle::area
  return
fn Square::area
  return
fn total
  indirect ?(s->area) -> {Shape::area, Circle::area, Square::area}
  indirect ?(r.area) -> {Shape::area, Circle::area, Square::area}
  call id -> Shape::id
  return
)");
}

TEST_CASE("recursion: direct and through a cycle") {
    Model m = parse(R"(int fact(int n) { return n ? n * fact(n - 1) : 1; }
void ping(int n); void pong(int n) { ping(n); }
void ping(int n) { if (n) pong(n - 1); }
int main() { fact(3); ping(2); }
)");
    REQUIRE(m.functions.size() == 4);
    CHECK(m.functions[0].body[0].target == 0);     // fact -> fact
    CHECK(m.functions[1].body[0].target == 2);     // pong -> ping
    CHECK(m.entry_points() == std::vector<int>{3});
}

TEST_CASE("library calls are unresolved") {
    Model m = parse(R"(#include <cstdio>
#include <algorithm>
#include <vector>
void f(std::vector<int>& v) { printf("x"); std::sort(v.begin(), v.end()); v.push_back(1); }
)");
    CHECK(dump(m, false) == R"(fn f
  call printf [lib]
  call begin [lib]
  call end [lib]
  call std::sort [lib]
  call push_back [lib]
)");
}

TEST_CASE("methods, qualified calls, templates, namespaces and lambdas") {
    CHECK(ir(R"(namespace util { template <class T> T twice(T x) { return x + x; } }
class Foo {
 public:
  Foo() { init(); }
  void init() {}
  static int make() { return 1; }
  void run();
};
void init() {}
void Foo::run() { init(); auto fn = [&] { return Foo::make(); }; fn(); }
int main() { Foo f; f.run(); util::twice<int>(2); init(); }
)") == R"(fn twice
  return
fn Foo::Foo
  call init -> Foo::init
fn Foo::init
fn Foo::make
  return
fn init
fn Foo::run
  call init -> Foo::init
  call Foo::make -> Foo::make
  indirect ?(fn)
fn main
  call run -> Foo::run
  call util::twice
  call init
)");
}

TEST_CASE("entry points without main are the uncalled functions in source order") {
    Model m = parse("void leaf() {}\nvoid a() { leaf(); }\nvoid b() { leaf(); b(); }\n");
    CHECK(m.entry_points() == std::vector<int>{1, 2});
    Model cyc = parse("void x(); void y() { x(); } void x() { y(); }");
    CHECK(cyc.entry_points() == std::vector<int>{0});
}

TEST_CASE("C files use the C grammar") {
    Model m = parse("struct class { int new; };\nvoid f(void) { g(); }\n", "t.c");
    CHECK_FALSE(m.is_cpp);
    CHECK(m.errors.empty());
    CHECK(dump(m, false) == "fn f\n  call g [lib]\n");
    CHECK_FALSE(path_is_cpp("x.h"));
    CHECK(path_is_cpp("x.hpp"));
    CHECK(path_is_cpp("x.cc"));
}

TEST_CASE("parse errors: whatever parsed is kept and the function is flagged") {
    Model m = parse(R"(void a() {}
void b() {}
int main() {
  a();
  b(;
  if (x { a(); }
}
)");
    CHECK_FALSE(m.errors.empty());
    int mi = m.find("main");
    REQUIRE(mi >= 0);
    CHECK(m.functions[mi].has_error);
    CHECK_FALSE(m.functions[0].has_error);
    bool calls_a = false;
    for (const auto& r : m.functions[mi].body)
        if (r.kind == RegionKind::Call && r.name == "a") calls_a = true;
    CHECK(calls_a);
}

TEST_CASE("parse errors: calls that error recovery turns into declarations are still calls") {
    // typing `broken(` in front of an existing statement: tree-sitter reads
    // `broken( funcA();` as a declaration of a function named funcA
    CHECK(ir("void funcA() {}\nvoid funcC() {}\nint main() { broken( funcA(); funcC(); return 0; }\n") ==
          R"(fn funcA
fn funcC
fn main [error]
  call funcA
  call funcC
  return
)");
    // an unfinished `if (` swallows the next statement into its condition
    Model m = parse("void funcA() {}\nvoid funcC() {}\nint main() { funcA(); if (x\n funcC(); return 0; }\n");
    int mi = m.find("main");
    REQUIRE(mi >= 0);
    std::string text = dump(m, false);
    CHECK(text.find("call funcA") != std::string::npos);
    CHECK(text.find("call funcC") != std::string::npos);
    // a genuine local prototype in a function that parses cleanly is not a call
    CHECK(ir("void helper() {}\nint main() { void helper(); return 0; }\n") == "fn helper\nfn main\n  return\n");
}

TEST_CASE("last good layout is kept for functions that stop parsing") {
    Model good = parse("void a() {}\nvoid helper() { a(); }\nint main() { helper(); }\n");
    REQUIRE(good.errors.empty());
    // the user is half-way through retyping helper's signature
    Model broken = parse("void a() {}\nvoid helper( { a(); }\nint main() { helper(); }\n");
    REQUIRE_FALSE(broken.errors.empty());
    Model merged = merge_last_good(broken, good);
    int h = merged.find("helper");
    REQUIRE(h >= 0);
    int mi = merged.find("main");
    REQUIRE(mi >= 0);
    bool resolved = false;
    for (const auto& r : merged.functions[mi].body)
        if (r.kind == RegionKind::Call && r.name == "helper") resolved = r.target == h;
    CHECK(resolved);
    CHECK(merged.functions[h].body.size() == 1);

    // a clean parse is never touched
    Model clean = parse("int main() {}\n");
    CHECK(merge_last_good(clean, good).functions.size() == 1);
}

TEST_CASE("cursor lookup finds the innermost function") {
    Model m = parse("void a() {\n}\nstruct S {\n  void m() {\n    a();\n  }\n};\n");
    CHECK(m.function_at(1, 3) == 0);
    CHECK(m.function_at(5, 4) == 1);
    CHECK(m.function_at(3, 0) == -1);
}
