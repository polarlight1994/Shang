#include <stdlib.h>
#include <stdio.h>
#include <assert.h>

#ifdef __cplusplus
extern "C" {
#endif
int op_and_by_constant(int a) __attribute__ ((noinline));
int op_and_by_constant(int a) { return a & 0xFFFFF802; }
#ifdef __cplusplus
}
#endif

int main(int argc, char **argv) {
  srand (16);
  
  int i;
  for(i = 0; i < 16; ++i) {
    int a = rand();
    printf("result:%d\n", op_and_by_constant(a));
  }

  return 0;
}
