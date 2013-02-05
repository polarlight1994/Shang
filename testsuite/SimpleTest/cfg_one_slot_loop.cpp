#include <stdlib.h>
#include <stdio.h>
#include <assert.h>

#define N_FFT (16)

#ifdef __cplusplus
extern "C" {
#endif
int cfg_one_slot_loop(int j) __attribute__ ((noinline));
int cfg_one_slot_loop(int j) {
 	int m = N_FFT;
	while (m >= 2 && j >= m) 
	  {
	    j -= m ;
	    m >>= 1;
	  }
	j += m ;
  return j;
}
#ifdef __cplusplus
}
#endif

int main(int argc, char **argv) {
  srand (16);

  int i, j = 0  ; 
  int tmpr, max = 2, m, n = N_FFT << 1 ; 
    
    /* do the bit reversal scramble of the input data */
    
  for (i = 0; i < (n-1) ; i += 2) {	
	 j = cfg_one_slot_loop(j);
   printf("[%d] = %x\n", i, j);
  }

  return 0;
}
