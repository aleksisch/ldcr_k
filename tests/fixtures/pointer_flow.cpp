struct Inner { int* head; int* tail; };
struct Box { int* lead; Inner nested; };
int globalValue;

int* identity(int* value) { return value; }

int* invoke(int* (*function)(int*), int* value) { return function(value); }

int* dereference(int** address) { return *address; }

int* throughField(Box* box, int* (*reader)(int**)) { return reader(&box->nested.tail); }

void noArguments() {}
void unresolved(void (*function)()) { function(); }

int* inspect(Box* box, int* value, bool choose) {
    box->nested.tail = value;
    int* loaded = box->nested.tail;
    int** escaped = &box->nested.tail;
    *escaped = identity(loaded);
    return choose ? loaded : box->nested.head;
}

int main(int argc, char**) {
    Box box{&globalValue, {&globalValue, &globalValue}};
    int* result = invoke(identity, inspect(&box, &globalValue, argc > 1));
    noArguments();
    return result == throughField(&box, dereference);
}
