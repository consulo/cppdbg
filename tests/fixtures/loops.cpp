#include <cstdio>

int sum = 0;

int compute(int i) {
    sum += i;
    return sum;
}

int main() {
    for (int i = 0; i < 10; ++i) {
        compute(i);
    }
    std::printf("sum=%d\n", sum);
    return 0;
}
