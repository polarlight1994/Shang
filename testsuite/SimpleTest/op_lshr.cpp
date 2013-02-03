#include <stdlib.h>
#include <stdio.h>
#include <assert.h>

#ifdef __cplusplus
extern "C" {
#endif
int op_lshr(unsigned long long int a, unsigned long long int b) __attribute__ ((noinline));
int op_lshr(unsigned long long int a, unsigned long long int b) { return a >> b; }
#ifdef __cplusplus
}
#endif

int main(int argc, char **argv) {
  srand (16);
  
  int i;
  for(i = 0; i < 16; ++i) {
    long long int a = rand();
    long long int b = rand();
    printf("%x >>> %x -> result:%d\n", a, b, op_lshr(a, b));
  }
  return 0;
}
