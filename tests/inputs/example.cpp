void a1() {}
void a2() {}
void funcA() {
    for (int i = 0; i < 2; ++i) { a1(); a2(); }
}
void funcC() {}
int main() { funcA(); funcC(); return 0; }
