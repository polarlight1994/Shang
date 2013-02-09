#include <stdlib.h>
#include <stdio.h>
#include <assert.h>

#ifdef __cplusplus
extern "C" {
#endif
int op_sdiv_const_power_of_2(int a) __attribute__ ((noinline));
int op_sdiv_const_power_of_2(int a) { return a / 16; }
#ifdef __cplusplus
}
#endif

int main(int argc, char **argv) {
  srand (16);

  int i;
  for(i = 0; i < 16; ++i) {
    int a = rand();
    printf("result:%d\n", op_sdiv_const_power_of_2(a));
  }

  return 0;
}
