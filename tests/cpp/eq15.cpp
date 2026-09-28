// Paper Eq. 15 / Fig. 9 shape. The dispatch excursion at c8 climbs out of wid
// through one call site (c6) and may come back through another (c7).
// kCFA, k >= 1:  v1 -> {k1},  v2 -> {k2}.
// L_DC (no L_R): also k1 -> v2 (excursion leaves via c6, returns via c7).
struct K {};
struct J { virtual K* id(K* p) { return p; } };

K* wid(J* j, K* k) { return j->id(k); }   // c8 (virtual; receiver j is a parameter)

int main() {
    J* j1 = new J();       // j1
    K* k1 = new K();       // k1
    K* k2 = new K();       // k2
    K* v1 = wid(j1, k1);   // c6
    K* v2 = wid(j1, k2);   // c7
    return v1 == v2;
}
