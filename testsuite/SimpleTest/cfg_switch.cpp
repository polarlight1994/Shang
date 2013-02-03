#include <stdlib.h>
#include <stdio.h>
#include <assert.h>

#ifdef __cplusplus
extern "C" {
#endif
unsigned cfg_switch(unsigned a, unsigned b, unsigned c) __attribute__ ((noinline));
unsigned cfg_switch(unsigned a, unsigned b, unsigned c) {
  switch (c) {
  default:  return a;
  case 0:   return a + b;
  case 1:   return a - b;
  case 2:   return a * b;
  case 3:   return a & b;
  case 4:   return a | b;
  case 5:   return a ^ b;
  }
}
#ifdef __cplusplus
}
#endif

int main(int argc, char **argv) {
  srand (16);

  long i;
  for(i = 0; i < 16; ++i) {
    unsigned a = (unsigned ) rand();
    unsigned b = (unsigned ) rand();
    unsigned c = (unsigned ) i;
    unsigned res = cfg_switch(a, b, c);
    printf("result:%d\n", res);
  }

  return 0;
}
