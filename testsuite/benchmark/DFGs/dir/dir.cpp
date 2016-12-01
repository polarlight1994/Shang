/* C synthesizable-behavioral-style description of PR
   Created by Zhiru Zhang, 01/11/2005
   VLSI CAD Lab, UCLA
*/

/* Program which directly implements 8 point 1D DCT-II
   Direct implementation of below equation:

   S(u) = 0.5*C(u)*SUMMATION (from x=0 to x=7) of {s(x)*cos[(2x+1)*u*pi/16)]}
   Input:  s(x)  x = 0, 1, ...7
   s(x) = 1-D sample value
   Output  S(u)  u = 0, 1, ...7
   S(u) = 1-D DCT coefficient

   C(u) = 1/sqareroot(2)   for u=0
   = 1                for u>0

   number of multiplications:  61
   (note that due to common sub-expression elimination,
   3 multiplications (less than 64) were needed.  
   C00 = C40
   C03 = C43
   C04 = C44 
   C07 = C47 )
   Author: Miodrag Potkonjak
   Date:  January 1994

   simulated and corrected (lmg)
*/

#include <stdio.h>
#define num8 (short)
#define wc (short)

#define C00 wc(3535) 
#define C01 wc(3535) 
#define C02 wc(3535) 
#define C03 wc(3535) 
#define C04 wc(3535) 
#define C05 wc(3535) 
#define C06 wc(3535) 
#define C07 wc(3535) 

#define C10 wc(4903) 
#define C11 wc(4157) 
#define C12 wc(2777) 
#define C13 wc(975) 
#define C14 wc(0-975) 
#define C15 wc(0-2777) 
#define C16 wc(0-4157) 
#define C17 wc(0-4903) 

#define C20 wc(4619) 
#define C21 wc(1913) 
#define C22 wc(0-1913) 
#define C23 wc(0-4619) 
#define C24 wc(0-4619) 
#define C25 wc(0-1913) 
#define C26 wc(1913) 
#define C27 wc(4619) 

#define C30 wc(4157) 
#define C31 wc(0-975) 
#define C32 wc(0-4903) 
#define C33 wc(0-2777) 
#define C34 wc(2777) 
#define C35 wc(4903) 
#define C36 wc(975) 
#define C37 wc(0-4157) 

#define C40 wc(3535) 
#define C41 wc(0-3535) 
#define C42 wc(0-3535) 
#define C43 wc(3535) 
#define C44 wc(3535) 
#define C45 wc(0-3535) 
#define C46 wc(0-3535) 
#define C47 wc(3535) 

#define C50 wc(2777) 
#define C51 wc(0-4903) 
#define C52 wc(975) 
#define C53 wc(4157) 
#define C54 wc(0-4157) 
#define C55 wc(0-975) 
#define C56 wc(4903) 
#define C57 wc(0-2777) 

#define C60 wc(1913) 
#define C61 wc(0-4619) 
#define C62 wc(4619) 
#define C63 wc(0-1913) 
#define C64 wc(0-1913) 
#define C65 wc(4619) 
#define C66 wc(0-4619) 
#define C67 wc(1913) 

#define C70 wc(975) 
#define C71 wc(0-2777) 
#define C72 wc(4157) 
#define C73 wc(0-4903) 
#define C74 wc(4903) 
#define C75 wc(0-4157) 
#define C76 wc(2777) 
#define C77 wc(0-975)
#ifdef __cplusplus
extern "C" {
#endif
int  dir(short x0, short x1, short x2, short x3,
          short x4, short x5, short x6, short x7);

int  dir(short x0, short x1, short x2, short x3,
          short x4, short x5, short x6, short x7)
{
short y10, y11, y12, y13, y14, y15, y16, y17;
 int result_out;

    y10 =  num8 (C00 * x0) + num8 (C01 * x1) + 
        num8 (C02 * x2) + num8 (C03 * x3) + 
        num8 (C04 * x4) + num8 (C05 * x5) + 
        num8 (C06 * x6) + num8 (C07 * x7);

    y11 =  num8 (C10 * x0) + num8 (C11 * x1) + 
        num8 (C12 * x2) + num8 (C13 * x3) + 
        num8 (C14 * x4) + num8 (C15 * x5) + 
        num8 (C16 * x6) + num8 (C17 * x7);

    y12 =  num8 (C20 * x0) + num8 (C21 * x1) + 
        num8 (C22 * x2) + num8 (C23 * x3) + 
        num8 (C24 * x4) + num8 (C25 * x5) + 
        num8 (C26 * x6) + num8 (C27 * x7);

    y13 =  num8 (C30 * x0) + num8 (C31 * x1) + 
        num8 (C32 * x2) + num8 (C33 * x3) + 
        num8 (C34 * x4) + num8 (C35 * x5) + 
        num8 (C36 * x6) + num8 (C37 * x7);

    y14 =  num8 (C40 * x0) + num8 (C41 * x1) + 
        num8 (C42 * x2) + num8 (C43 * x3) + 
        num8 (C44 * x4) + num8 (C45 * x5) + 
        num8 (C46 * x6) + num8 (C47 * x7);

    y15 =  num8 (C50 * x0) + num8 (C51 * x1) + 
        num8 (C52 * x2) + num8 (C53 * x3) + 
        num8 (C54 * x4) + num8 (C55 * x5) + 
        num8 (C56 * x6) + num8 (C57 * x7);

    y16 =  num8 (C60 * x0) + num8 (C61 * x1) + 
        num8 (C62 * x2) + num8 (C63 * x3) + 
        num8 (C64 * x4) + num8 (C65 * x5) + 
        num8 (C66 * x6) + num8 (C67 * x7);

    y17 =  num8 (C70 * x0) + num8 (C71 * x1) + 
        num8 (C72 * x2) + num8 (C73 * x3) + 
        num8 (C74 * x4) + num8 (C75 * x5) + 
        num8 (C76 * x6) + num8 (C77 * x7);
    result_out = y10+y11+y12+y13+y14+y15+y16+y17;
    //  printf("%d\n",result_out);
  return result_out;

}
#ifdef __cplusplus
}
#endif

//int i;//25

#define NUM 10
int i;//25
int result_out[NUM];
int inputs[NUM+15]={10,23,34,43,54,98,78,67,75,15};
#define exp_res 11420
int main()
{
    int main_result;
  int return_value=0;
    short x01, x02, x03, x11, x12, x13, x21, x22;
    int i, j, k,l;
    //  int result_out;  
  for (int idx=0;idx<NUM;idx++)
    {
      i = inputs[idx];
      j = inputs[idx+1];
      k = inputs[idx+2];
      l = inputs[idx+3];
  x01 = i++; x02 = j++; x03 = l++;
  x11 = j++; x12 = k++; x13 = i++;
  x21 = l++; x22 = k++; 
  
  result_out[idx] = dir(x01, x02, x03, x11,
		   x12, x13, x21, x22);
 return_value = return_value+result_out[idx];
 //     printf("%d\n",result_out[idx]);
    }
  //  printf("benchmark_result = %d\n",result_out);
  //    main_result = (result_out != exp_res);
    //    printf("%d\n", main_result);
    //    return main_result;
  printf("return return_value != %d", return_value);
  //  return return_value;
  return return_value != 18928;
    // return 0;
}
