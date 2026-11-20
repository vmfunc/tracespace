// Calls whose target is not a plain function name: function pointers, virtual
// methods, recursion, lambdas, methods and library calls.
//
//   tracespace examples/dispatch.cpp
//
// Things to look for:
//   dashed box ?(expr)   a call through a pointer or a virtual method; candidates
//                        found by name in this file are listed inside the box
//   recursion marker     a function calling itself, directly or through a cycle;
//                        it is not expanded again
//   dim boxes            library calls (std::sort, printf), shown after pressing L
#include <algorithm>
#include <cstdio>
#include <functional>
#include <vector>

// ---- virtual dispatch: every override in the file is a candidate

struct Shape {
    virtual ~Shape() = default;
    virtual double area() const { return 0.0; }
    void describe() const { std::printf("area %.2f\n", area()); }
};

struct Circle : Shape {
    double r = 1.0;
    double area() const override { return 3.14159 * r * r; }
};

struct Square : Shape {
    double side = 1.0;
    double area() const override;
};

double Square::area() const { return side * side; }

double total_area(const std::vector<Shape*>& shapes) {
    double sum = 0.0;
    for (const Shape* s : shapes) {
        sum += s->area();  // ?(s->area) with Shape::area, Circle::area, Square::area
        s->describe();     // an ordinary method call
    }
    return sum;
}

// ---- function pointers: candidates are the functions assigned to the variable

void on_start() {}
void on_stop() {}

void (*hook)() = on_start;
void (*handlers[2])() = {on_start, on_stop};

void run_hooks(void (*extra)()) {
    hook = on_stop;
    hook();         // ?(hook) with on_start, on_stop
    handlers[0]();  // ?(handlers[0])
    extra();        // ?(extra): a parameter, no candidates known
}

// ---- recursion, direct and through a cycle

int factorial(int n) {
    if (n <= 1) return 1;
    return n * factorial(n - 1);
}

bool is_odd(int n);
bool is_even(int n) { return n == 0 ? true : is_odd(n - 1); }
bool is_odd(int n) { return n == 0 ? false : is_even(n - 1); }

// ---- lambdas are drawn inline where they are written; std::function is indirect

void sort_and_report(std::vector<int>& values, const std::function<void(int)>& report) {
    std::sort(values.begin(), values.end(), [](int a, int b) { return factorial(a) < factorial(b); });
    for (int v : values) {
        report(v);  // ?(report)
    }
}

// ---- methods, qualified and unqualified

class Counter {
public:
    void bump() { ++count_; log(); }
    static Counter make() { return Counter(); }

private:
    void log() const { std::printf("%d\n", count_); }
    int count_ = 0;
};

int main() {
    Circle c;
    Square s;
    std::vector<Shape*> shapes = {&c, &s};
    total_area(shapes);

    run_hooks(on_start);

    factorial(5);
    is_even(10);

    std::vector<int> values = {3, 1, 2};
    sort_and_report(values, [](int v) { std::printf("%d\n", v); });

    Counter counter = Counter::make();
    for (int i = 0; i < 3; ++i) {
        counter.bump();
    }
    return 0;
}
