// Use C linkage to keep graph assertions independent of C++ name mangling.
extern "C" {
int direct(int value) { return value + 1; }
int indirect(int value) { return value + 2; }
int invoke(int (*function)(int), int value) { return function(value); }
}

int main() {
    return invoke(indirect, direct(0));
}
