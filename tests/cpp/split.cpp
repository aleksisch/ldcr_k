// Endpoint intersection: L_D and L_C each accept a path from a1 to q, but not the same one.
// Exact L_DC: p -> {a1}, q -> {a2}.   Ours with --match-calls: p, q -> {a1, a2}.
// - The only L_D path a1 ~> q goes into id at c1 and out at c2: L_C rejects it.
// - L_C (which ignores fields) finds another one: a1 -store[f]-> b, then back along b->g = q's
//   store (reversed): no calls, realizable, but its fields don't match: L_D rejects it.
// p -> a2 the same way backwards: a2 -> q, b->g = q, b->f = a1 reversed, then a1 -> p via c1.
// Delete the line `b->g = q;` and both spurious facts disappear.
struct A {};
struct Box {
    A* f;
    A* g;
};

A* id(A* x) { return x; }

int main() {
    A* a1 = new A();     // a1
    A* a2 = new A();     // a2
    Box* b = new Box();  // b
    A* p = id(a1);       // c1
    A* q = id(a2);       // c2
    b->f = a1;
    b->g = q;
    (void)p;
    return 0;
}
