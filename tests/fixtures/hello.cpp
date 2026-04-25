#include <cstdio>

int add(int a, int b) { return a + b; }

int main(int argc, char** argv) {
    int x = 2;
    int y = 3;
    int z = add(x, y);
    std::printf("hello %d %d from %s (argc=%d)\n", x, z, argv[0], argc);
    return 0;
}
