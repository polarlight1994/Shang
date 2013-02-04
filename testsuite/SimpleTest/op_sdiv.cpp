#include <stdlib.h>
#include <stdio.h>
#include <assert.h>

#ifdef __cplusplus
extern "C" {
#endif
int op_div(int a, int b) __attribute__ ((noinline));
int op_div(int a, int b) { return a / b; }
#ifdef __cplusplus
}
#endif

int main(int argc, char **argv) {
  srand (16);

  int i;
  for(i = 0; i < 16; ++i) {
    int a = rand();
    int b = rand();
	  if (b == 0) b = 1;
    printf("result:%d\n", op_div(a, b));
  }

  return 0;
}
