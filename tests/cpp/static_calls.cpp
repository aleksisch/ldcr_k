// No dispatch, no fields. Checks call/return matching (C_k).
// kCFA, k >= 1:  x -> {oa},  y -> {ob}.    k = 0 (Andersen): both {oa, ob}.
struct O {};

O* id(O* p) { return p; }

int main() {
    O* a = new O();   // oa
    O* b = new O();   // ob
    O* x = id(a);     // c1
    O* y = id(b);     // c2
    return x == y;
}
