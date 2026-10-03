// Fields through a setter/getter, no dispatch. Checks L_F and heap context.
// kCFA, k >= 1:  r1 -> {o1},  r2 -> {o2}.  k = 0: both {o1, o2}.
struct O {};
struct Box { O* f; };

void set(Box* b, O* o) { b->f = o; }
O* get(Box* b) { return b->f; }

int main() {
    Box* b1 = new Box();   // box1
    Box* b2 = new Box();   // box2
    O* o1 = new O();       // o1
    O* o2 = new O();       // o2
    set(b1, o1);           // c1
    set(b2, o2);           // c2
    O* r1 = get(b1);       // c3
    O* r2 = get(b2);       // c4
    return r1 == r2;
}
