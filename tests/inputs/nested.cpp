void a() {} void b() {} void c() {}
void deep() { for (int i = 0; i < 2; ++i) { a(); for (int j = 0; j < 3; ++j) { b(); for (int k = 0; k < 4; ++k) { c(); } } } }
int total() { int s = 0; for (int i = 0; i < 10; ++i) s += i; return s; }
int main() {
    for (int i = 0; i < 5; ++i) {
        a();
        for (int j = 0; j < 4; ++j) { b(); while (true) { c(); } }
        deep();
    }
    for (int k = 0; k < 100; ++k) {}
    total();
}
