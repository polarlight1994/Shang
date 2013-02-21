#include <stdlib.h>
#include <stdio.h>
#include <assert.h>

#define N 2053
char a[N];
char b[N];

#ifdef __cplusplus
extern "C" {
#endif
void loop_memop_fusing_mem_move_8bit_epilog() __attribute__ ((noinline));
void loop_memop_fusing_mem_move_8bit_epilog() {
  long i;
  for (i = 0; i < N; ++i)
   b[i] = a[i];
}
#ifdef __cplusplus
}
#endif

int main(int argc, char **argv) {

  long i;
  for(i = 0; i < N; ++i)
    a[i] = (unsigned) rand();

  loop_memop_fusing_mem_move_8bit_epilog();

  for(i = 0; i < N; ++i)
    printf("a[%d]:%d\n", i, b[i]);

  return 0;
}
