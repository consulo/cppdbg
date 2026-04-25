#include <cstdio>
#include <stdexcept>

void thrower(int n) {
    if (n > 0) throw std::runtime_error("test exception");
}

int main() {
    try {
        thrower(1);
    } catch (const std::exception& e) {
        std::printf("caught: %s\n", e.what());
    }
    std::printf("done\n");
    return 0;
}
