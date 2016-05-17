/* Program for DCT-II, N=8, based on 
   Arai's fast DCT algorithm
   Pennebraker & Mitchell, "JPEG"
   p. 52.

   Author:   Miodrag Potkonjak
   January 1995.

    DIF = .5 * Arai * sec(pi*u/16)  u=1..7
    DIF = Arai                      u = 0
    simulated and changed (lmg)
*/

#include <stdio.h>
//#define  num8 fix<8,6>
#define  num8 (short)

//#define  num12 fix<12,10>
#define  num12 (short)

#define  m1  num12 (7071)    
#define  m2  num12 (3826)   
#define  m3  num12 (5411)    
#define  m4  num12 (13060)    
#ifdef __cplusplus
extern "C" {
#endif


int arai(short x10, short x11, short x12, short x13,
          short x14, short x15, short x16, short x17)         
{
  short b0, b1, b2, b3, b4, b5, b6;
  short c0, c1, c3;
  short d2, d3, d4, d5, d6, d7, d8;
  short e2, e3, e4, e5, e6, e7, e8;
  short f4, f5, f6, f7;
  int result_out;
 short y10, y11, y12, y13, y14, y15, y16, y17;

  b0 = x10 + x17;
  b1 = x11 + x16;
  b2 = x13 - x14;
  b3 = x11 - x16;
  b4 = x12 + x15;
  b5 = x13 + x14;
  b6 = x12 - x15;
  d8 = x10 - x17;

  c0 = b0 + b5;
  c1 = b1 - b4;
  d2 = -(b2 + b6);
  c3 = b1 + b4;
  d5 = b0 - b5;
  d6 = b3 + d8;
  d7 = b3 + b6;

  d3 = c1 + d5;
  d4 = d2 + d6;

  e2 = num8 (m3 * d2);
  e3 = num8 (m1 * d7);
  e4 = num8 (m4 * d6);
  e5 = d5;
  e6 = num8 (m1 * d3);
  e7 = num8 (m2 * d4);
  e8 = d8;
  
  f4 = e3 + e8;
  f5 = e8 - e3;
  f6 = -(e2 + e7);
  f7 = e4 - e7;
  
  y10 = c0 + c3;
  y14 = c0 - c3;

  y12 = e5 + e6;
  y16 = e5 - e6;

  y11 = f4 + f7;
  y13 = f5 - f6;
  y15 = f5 + f6;
  y17 = f4 - f7;

  result_out = y10+y11+y12+y13 - y14-y15+y16+y17;
  // printf("%d\n",result_out);
  return result_out;
 
}
#ifdef __cplusplus
}
#endif

//int i;//25
//int i, j, k, l;
#define NUM 10
int result_out[NUM];
int inputs[NUM+5]={10,23,34,43,54,98,78,67,75,15,84,71,19,24};
#define exp_res 222
int main()
{
    int main_result;
    int i, j, k, l;
  short x10, x11, x12, x13;
  short x14, x15, x16, x17;
   int return_value=0;
  //  int result_out;  
   int idx;
  for (idx=0;idx<NUM;idx++)
    {
      i = inputs[idx];
      j = inputs[idx+1];
      k = inputs[idx+2];
      l = inputs[idx+3];
      
  x10 = i++; x11 = j++; x12 = k++; x13 = l++;
  x14 = (i+k); x15 = (j+l); x16 = (l+i); x17 = (k+l);

  result_out[idx] = arai(x10, x11, x12, x13,
			 x14, x15, x16,x17);
  return_value = return_value+result_out[idx];
  //     printf("%d\n",result_out[idx]);
    }
  //    printf("benchmark_result = %d\n",result_out);
  // main_result = (result_out != exp_res);
    //    printf("%d\n", main_result);
    //    return main_result;

  printf("return return_value != %d\n", return_value);
  //  return return_value; 
return return_value != 138852;
}
