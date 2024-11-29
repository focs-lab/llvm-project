#include <stdlib.h>

int x[100];

void foo() {}

void Test1() {
    x[0] = 1;
    x[1] = 2;
    x[2] = 3;
    x[3] = 4;
    foo();
    x[4] = 5;
    x[5] = 6;
    x[6] = 7;
}

const int NUM_PTRS = 10000000;
int* y[NUM_PTRS];

void Test2() {
    for (int i = 0; i < NUM_PTRS; ++i) { 
        y[i] = (int*) malloc(sizeof(int));
    }
    for (int i = 0; i < NUM_PTRS; ++i) { 
        free(y[i]);
    }
}

void Test3() {
    for (int i = 0; i < NUM_PTRS; ++i) { 
        y[i] = (int*) malloc(sizeof(int));
        free(y[i]);
    }
}

int main() {

    // for (int i = 0; i < 100; ++i) {
    //     x[i] = i;
    //     foo();
    // }

    // Test2();
    Test3();

    return 0;
}