#include <sys/types.h>

int t2[(sizeof(off_t) >= 8) ? 1 : -1];

int main() {
  ;
  return 0;
}