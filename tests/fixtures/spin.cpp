#include <chrono>
#include <cstdio>
#include <thread>

// Sentinel global so the test can break on a known function.
volatile int spin_iter = 0;

void slow() {
    for (int i = 0; i < 600; ++i) {
        spin_iter = i;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

int main() {
    std::printf("spin: pid ready\n");
    std::fflush(stdout);
    slow();
    return 0;
}
