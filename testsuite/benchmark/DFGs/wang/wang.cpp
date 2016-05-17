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

int wang (short x11, short x12, short x13, short x14, short x15,
           short x16, short x17, short x18);


int wang (short x11, short x12, short x13, short x14, short x15,
           short x16, short x17, short x18)

{
  int result_out;
short x21, x22, x23, x24, x25, x26, x27, x28,
  x31, x32, x33, x34,
  x81, x82, x83, x84,
  x91, x92, x93, x94;

 short y1, y2, y3, y4, y5, y6, y7, y8;

 x21 = x11 + x18;
 x22 = x12 + x17;
 x23 = x13 + x16;
 x24 = x14 + x15;
 x25 = x14 - x15;
 x26 = x13 - x16;
 x27 = x12 - x17;
 x28 = x11 - x18;

 x31 = x21 + x24;
 x32 = x22 + x23;
 x33 = x22 - x23;
 x34 = x21 - x24;

  y1 = num8 ((x31 + x32) * C1I4);
  y5 = num8 ((x31 - x32) * C1I4);
  y7 = num8 ( num8 (x34 * S1I8) - num8 (x33 * C1I8));
  y3 = num8 ( num8 (x34 * C1I8) + num8 (x33 * S1I8));

  x81 = num8 (x28 * V1);
  x82 = num8 (x25 * V1);
  x83 = num8 ((x27 + x26) * C1I4B);
  x84 = num8 ((x27 - x26) * C1I4B);

  x91 = num8 ((x81 + x83) * V1);
  x92 = num8 ((x82 + x84) * V1);
  x93 = num8 ((x81 - x83) * V1);
  x94 = num8 ((x82 - x84) * V1);

  y2 = num8 (x91 * C1I16) + num8 (x92 * S1I16);
  y8 = num8 (x91 * S1I16) - num8 (x92 * C1I16);
  y6 = num8 (x93 * C5I16) + num8 (x94 * S5I16);
  y4 = num8 (x93 * S5I16) - num8 (x94 * C5I16);

  result_out = y1+y2+y3+y4 +y5+y6+y7+y8;
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
    int main_result;
  int x11, x12, x13, x21, x22, x23;
  int x31, x32;
  int j, k, l;
  //  int result_out;  
  //  int i =25;
   int return_value=0;


  for (int idx = 0;idx<NUM;idx++)
    {
      i = inputs[idx];
      j = inputs[idx+1];
      k = inputs[idx+2];
      l = inputs[idx+4];
      x11 = i++; x12 = l++; x13 = k++;
      x21 = k++; x22 = j++; x23 = i++;
      x31 = j++; x32 = k++; 
      result_out[idx] = wang (x11, x12, x13, x21, x22,
		     x23, x31, x32);
 return_value = return_value+result_out[idx];
 //      printf("%d\n",result_out[idx]);
    }

  //    printf("benchmark_result = %d\n",result_out);
  //main_result = (result_out != exp_res);
             printf("%d\n", return_value);
    //    return main_result;
  //  return 0;
  return return_value != 282164;
}
