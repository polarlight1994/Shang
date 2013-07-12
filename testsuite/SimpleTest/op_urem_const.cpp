#include <stdlib.h>
#include <stdio.h>
#include <assert.h>

#ifdef __cplusplus
extern "C" {
#endif
unsigned op_urem_const(unsigned a) __attribute__ ((noinline));
unsigned op_urem_const(unsigned a) { return a % 411774; }
#ifdef __cplusplus
}
#endif

int main(int argc, char **argv) {
  srand (16);

  int i;
  for(i = 0; i < 16; ++i) {
    unsigned a = rand();
    printf("result:%d\n", op_urem_const(a));
  }

  return 0;
}
