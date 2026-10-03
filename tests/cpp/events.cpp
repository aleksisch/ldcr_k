// Event handling: an event bus with handler chains, commands with undo, a document model.
// Exercises: handlers stored in arrays, chain-of-responsibility (virtual call on `next`),
// commands that capture objects and are replayed from an undo stack, getters/setters.

struct Doc;

struct Event {
    virtual ~Event() {}
    virtual int kind() = 0;
};

struct KeyEvent : Event {
    char key;
    explicit KeyEvent(char k) : key(k) {}
    int kind() override { return 1; }
};

struct ClickEvent : Event {
    long x, y;
    ClickEvent(long a, long b) : x(a), y(b) {}
    int kind() override { return 2; }
};

struct Text {
    char buf[64];
    int len = 0;
};

struct Doc {
    Text* text;
    Text* clipboard;
    Doc(Text* t, Text* c) : text(t), clipboard(c) {}
    Text* getText() { return text; }
    Text* getClipboard() { return clipboard; }
    void setClipboard(Text* t) { clipboard = t; }
};

// ---- commands -------------------------------------------------------------------------

struct Command {
    virtual ~Command() {}
    virtual void run(Doc* d) = 0;
    virtual void undo(Doc* d) = 0;
    virtual Text* touched() = 0;
};

struct Insert : Command {
    char ch;
    Text* target = nullptr;
    explicit Insert(char c) : ch(c) {}
    void run(Doc* d) override { target = d->getText(); target->buf[target->len++] = ch; }
    void undo(Doc*) override { if (target) --target->len; }
    Text* touched() override { return target; }
};

struct Copy : Command {
    Text* saved = nullptr;
    Text* fresh;
    explicit Copy(Text* f) : fresh(f) {}
    void run(Doc* d) override { saved = d->getClipboard(); d->setClipboard(fresh); }
    void undo(Doc* d) override { d->setClipboard(saved); }
    Text* touched() override { return fresh; }
};

struct Macro : Command {
    Command* first;
    Command* second;
    Macro(Command* a, Command* b) : first(a), second(b) {}
    void run(Doc* d) override { first->run(d); second->run(d); }
    void undo(Doc* d) override { second->undo(d); first->undo(d); }
    Text* touched() override { return second->touched(); }
};

struct UndoStack {
    Command* items[16];
    int top = 0;
    void push(Command* c) { items[top++] = c; }
    Command* pop() { return top > 0 ? items[--top] : nullptr; }
};

// ---- handlers: chain of responsibility ------------------------------------------------

struct Handler {
    Handler* next = nullptr;
    virtual ~Handler() {}
    virtual Command* handle(Event* e) { return next ? next->handle(e) : nullptr; }
};

struct KeyHandler : Handler {
    Command* handle(Event* e) override {
        if (e->kind() == 1)
            return new Insert(static_cast<KeyEvent*>(e)->key);
        return Handler::handle(e);
    }
};

struct ClickHandler : Handler {
    Text* scratch;
    explicit ClickHandler(Text* t) : scratch(t) {}
    Command* handle(Event* e) override {
        if (e->kind() == 2)
            return new Copy(scratch);
        return Handler::handle(e);
    }
};

struct LoggingHandler : Handler {
    int seen = 0;
    Command* handle(Event* e) override { ++seen; return Handler::handle(e); }
};

Handler* chain(Handler* a, Handler* b) { a->next = b; return a; }

struct Bus {
    Handler* root;
    Doc* doc;
    UndoStack history;
    Bus(Handler* h, Doc* d) : root(h), doc(d) {}
    void dispatch(Event* e) {
        Command* c = root->handle(e);
        if (!c) return;
        c->run(doc);
        history.push(c);
    }
    Text* undoLast() {
        Command* c = history.pop();
        if (!c) return nullptr;
        c->undo(doc);
        return c->touched();
    }
};

long sink;

int main() {
    Text* body = new Text();
    Text* clip = new Text();
    Text* scratch = new Text();
    Doc* doc = new Doc(body, clip);

    Handler* handlers = chain(new LoggingHandler(), chain(new KeyHandler(), new ClickHandler(scratch)));
    Bus bus(handlers, doc);

    bus.dispatch(new KeyEvent('a'));
    bus.dispatch(new ClickEvent(1, 2));
    bus.dispatch(new KeyEvent('b'));

    Command* macro = new Macro(new Insert('c'), new Copy(new Text()));
    macro->run(doc);
    bus.history.push(macro);

    Text* t1 = bus.undoLast();
    Text* t2 = bus.undoLast();
    Text* c1 = doc->getClipboard();
    Text* b1 = doc->getText();

    sink = (t1 ? t1->len : 0) + (t2 ? t2->len : 0) + c1->len + b1->len;
    return 0;
}
