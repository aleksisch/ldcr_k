// Scene of shapes: a hand-written list container, decorators, a factory, observers.
// Exercises: objects stored in and read back from a shared container type, decorator
// chains (virtual call on a field), factory methods, callbacks through observer lists.

struct Point {
    long x, y;
    Point(long a, long b) : x(a), y(b) {}
};

struct Shape {
    virtual ~Shape() {}
    virtual long area() = 0;
    virtual Point* anchor() = 0;
    virtual Shape* moved(Point* by) = 0;
};

struct Circle : Shape {
    Point* center;
    long r;
    Circle(Point* c, long radius) : center(c), r(radius) {}
    long area() override { return 3 * r * r; }
    Point* anchor() override { return center; }
    Shape* moved(Point* by) override { return new Circle(new Point(center->x + by->x, center->y + by->y), r); }
};

struct Rect : Shape {
    Point* corner;
    long w, h;
    Rect(Point* c, long ww, long hh) : corner(c), w(ww), h(hh) {}
    long area() override { return w * h; }
    Point* anchor() override { return corner; }
    Shape* moved(Point* by) override { return new Rect(by, w, h); }
};

struct Square : Rect {
    Square(Point* c, long s) : Rect(c, s, s) {}
    Shape* moved(Point* by) override { return new Square(by, w); }
};

// Decorators hold another shape and forward to it.
struct Scaled : Shape {
    Shape* inner;
    long factor;
    Scaled(Shape* s, long f) : inner(s), factor(f) {}
    long area() override { return factor * factor * inner->area(); }
    Point* anchor() override { return inner->anchor(); }
    Shape* moved(Point* by) override { return new Scaled(inner->moved(by), factor); }
};

struct Labeled : Shape {
    Shape* inner;
    const char* label;
    Labeled(Shape* s, const char* l) : inner(s), label(l) {}
    long area() override { return inner->area(); }
    Point* anchor() override { return inner->anchor(); }
    Shape* moved(Point* by) override { return new Labeled(inner->moved(by), label); }
};

// ---- a generic list (void* payload, like a C container) -------------------------------

struct Cell {
    void* item;
    Cell* next;
    Cell(void* i, Cell* n) : item(i), next(n) {}
};

struct List {
    Cell* head = nullptr;
    int size = 0;
    void push(void* item) { head = new Cell(item, head); ++size; }
    void* get(int i) {
        Cell* c = head;
        while (i-- > 0 && c) c = c->next;
        return c ? c->item : nullptr;
    }
};

// ---- observers ------------------------------------------------------------------------

struct Observer {
    virtual ~Observer() {}
    virtual void added(Shape* s) = 0;
};

struct AreaTotal : Observer {
    long total = 0;
    void added(Shape* s) override { total += s->area(); }
};

struct LastAnchor : Observer {
    Point* last = nullptr;
    void added(Shape* s) override { last = s->anchor(); }
};

struct Scene {
    List shapes;
    List observers;
    void add(Shape* s) {
        shapes.push(s);
        for (int i = 0; i < observers.size; ++i)
            static_cast<Observer*>(observers.get(i))->added(s);
    }
    Shape* at(int i) { return static_cast<Shape*>(shapes.get(i)); }
    void watch(Observer* o) { observers.push(o); }
};

// ---- factory --------------------------------------------------------------------------

Shape* make(int kind, Point* p) {
    switch (kind) {
    case 0: return new Circle(p, 2);
    case 1: return new Rect(p, 3, 4);
    default: return new Square(p, 5);
    }
}

Shape* decorate(Shape* s, int how) {
    if (how == 1) return new Scaled(s, 2);
    if (how == 2) return new Labeled(s, "tag");
    return s;
}

long sink;

int main(int argc, char**) {
    Scene scene;
    AreaTotal total;
    LastAnchor last;
    scene.watch(&total);
    scene.watch(&last);

    Point* origin = new Point(0, 0);
    Point* p1 = new Point(1, 1);
    Point* p2 = new Point(2, 2);

    Shape* c = make(0, origin);
    Shape* r = make(1, p1);
    Shape* q = make(argc, p2);
    scene.add(c);
    scene.add(decorate(r, 1));
    scene.add(decorate(q, 2));
    scene.add(decorate(decorate(c, 1), 2));

    Point* shift = new Point(10, 10);
    Shape* moved = scene.at(1)->moved(shift);
    Point* a1 = c->anchor();
    Point* a2 = r->anchor();
    Point* a3 = moved->anchor();

    sink = total.total + last.last->x + a1->x + a2->y + a3->x + moved->area();
    return 0;
}
