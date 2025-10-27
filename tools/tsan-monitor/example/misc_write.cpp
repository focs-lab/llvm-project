// Test: Single-threaded write function
// Category: Misc
// Expectation: NO_RACE
// Notes: No concurrency; sanity baseline
int x;
void f() { x = 1; }

int main() {
  f();
  return 0;
}
