#include <stdlib.h>
#include <stdio.h>
#include <assert.h>

#ifdef __cplusplus
extern "C" {
#endif
int op_srem_const(int a) __attribute__ ((noinline));
int op_srem_const(int a) { return a % 411774; }
#ifdef __cplusplus
}
#endif

int main(int argc, char **argv) {
  srand (16);

  int i;
  for(i = 0; i < 16; ++i) {
    int a = rand();
    printf("result:%d\n", op_srem_const(a));
  }

  return 0;
}
