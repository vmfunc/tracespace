// The smallest useful diagram: a row of calls, a counted loop, and children.
//
//   tracespace examples/basics.cpp
//
// Expected (rounded theme):
//
//              ╭─── ×2 ───╮
//              ▼          │
// ┌──────┐    ┌────────────┐    ┌────────────┐
// │ main │───▶│   funcA    │───▶│   funcC    │
// └──────┘    └────────────┘    └────────────┘
//             ┌─────┐┌─────┐
//             │ a1  ││ a2  │
//             └─────┘└─────┘

void a1() {}
void a2() {}

void funcA() {
    for (int i = 0; i < 2; ++i) {
        a1();
        a2();
    }
}

void funcC() {}

int main() {
    funcA();
    funcC();
    return 0;
}
