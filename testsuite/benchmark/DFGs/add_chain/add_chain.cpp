/* Program for DCT-II, N=8, based on 
   Z. Wang: "Fast Algorithms for discrete W transform and for the
   discrete Fourier Transform", IEEE Transactions on Acoustic, Speech,
   and Signal porcessing, Vol. 32, No. 8, pp.803-816, 1994.

   Also in Rao & Yip  pp. 370-380 

   Author:   Miodrag Potkonjak
   January 1994.

   simulated and corrected (lmg)
*/

#include <stdio.h>
/*#define  num8 fix<8,8> */
#define  num8 (short)
/*#define  num12 fix<12,11>*/
#define  num12 (short)

#define  C1I4  num12(3535)
#define  S1I4  num12(3535)
#define  C1I8  num12(4619)
#define  S1I8  num12(1913)
#define  C1I4B num12(4999)
#define  S1I4B num12(4999)
#define  V1  num12(7071)
#define  C1I16  num12(9807)
#define  S1I16  num12(1950)
#define  C5I16  num12(5555)
#define  S5I16  num12(8314)
#ifdef __cplusplus
extern "C" {
#endif

int add_chain (short x11, short x12, short x13);


int add_chain (short x11, short x12, short x13)

{
  int result_out;

  result_out = x11+x12+x13;
  //  printf("%d\n", result_out);
  return result_out;
}
#ifdef __cplusplus
}
#endif

#define NUM 10
int i;//25
int result_out[NUM];
int inputs[NUM+5]={10,23,34,43,54,98,78,67,75,15,81, 19, 74, 24, 45};

#define exp_res 27136
//int i;//25
int main()
{
   
  return 1;
}
