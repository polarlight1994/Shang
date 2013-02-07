#include <stdlib.h>
#include <stdio.h>
#include <assert.h>

#ifdef __cplusplus
extern "C" {
#endif
int op_and_with_bit_catt(int a, int b) __attribute__ ((noinline));
int op_and_with_bit_catt(int a, int b) {
  return a & (((b & 0xff) << 8) | 0xff0000ff);
}
#ifdef __cplusplus
}
#endif

int main(int argc, char **argv) {
  srand (16);
  
  int i;
  for(i = 0; i < 16; ++i) {
    int a = rand();
    int b = rand();
    printf("result:%d\n", op_and_with_bit_catt(a, b));
  }

  return 0;
}
