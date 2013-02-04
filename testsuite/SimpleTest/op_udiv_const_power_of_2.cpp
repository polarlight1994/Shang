#include <stdlib.h>
#include <stdio.h>
#include <assert.h>

#ifdef __cplusplus
extern "C" {
#endif
int op_udiv_const_power_of_2(unsigned a) __attribute__ ((noinline));
int op_udiv_const_power_of_2(unsigned a) { return a / 64; }
#ifdef __cplusplus
}
#endif

int main(int argc, char **argv) {
  srand (16);

  int i;
  for(i = 0; i < 16; ++i) {
    int a = rand();
    printf("result:%d\n", op_udiv_const_power_of_2(a));
  }

  return 0;
}
