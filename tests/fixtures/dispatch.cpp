struct Shape {
    virtual int* area(int* p) = 0;
    virtual void reset() {}
};

struct Square : Shape {
    int* area(int* p) override { return p; }
};

struct Circle : Shape {
    int* area(int* p) override { return p; }
    void reset() override {}
};

struct Unrelated {
    virtual int* area(int* p) { return p; }
};

int* callArea(Shape* s, int* p) { return s->area(p); }
int* callAreaAgain(Shape* s, int* p) { return s->area(p); }
void callReset(Shape* s) { s->reset(); }

int main(int argc, char**) {
    Square square;
    Circle circle;
    Unrelated unrelated;
    Shape* heap = new Circle;
    Shape* s = argc > 1 ? static_cast<Shape*>(&square) : argc > 2 ? heap : &circle;
    int value = argc;
    callReset(s);
    return *callAreaAgain(s, callArea(s, unrelated.area(&value)));
}
