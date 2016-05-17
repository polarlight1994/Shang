#define w (short)
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif
int honda(short X_0_1, short X_1_1, 
           short S_1_1, short S_2_1, short S_3_1, short S_4_1,
           short S_5_1, short S_6_1, short S_7_1);

int honda(short X_0_1, short X_1_1, 
           short S_1_1, short S_2_1, short S_3_1, short S_4_1,
           short S_5_1, short S_6_1, short S_7_1)

{
short S_1, S_2, S_3, S_4, S_5, S_6, S_7;
short Y_0_1, Y_1_1;
 int result_out;

      S_1 = w(8263 * S_1_1) + w(1298 * S_2_1) + 
            w(8034 * S_3_1) + w(8411 * S_4_1) +
            w(3718 * S_5_1) + w(2007 * X_0_1) - 
            w(1491 * X_1_1);

      S_2 = w(4418 * S_1_1) + w(3784 * S_2_1) + 
            w(1627 * S_3_1) + w(5249 * S_4_1) + 
            w(2596 * S_5_1) - w(5601 * X_0_1) + 
            w(2981 * X_1_1);

      S_3 = w(6321 * S_1_1) + w(1034 * S_2_1) + 
            w(1153 * S_3_1) - w(9355 * S_4_1) - 
            w(4732 * S_5_1) - w(1130 * X_0_1) + 
            w(1019 * X_1_1);

      S_4 = w(4548 * S_1_1) + w(2553 * S_2_1) + 
            w(5567 * S_3_1) + w(4094 * S_4_1) + 
            w(1360 * S_5_1) + w(2145 * X_0_1) - 
            w(4924 * X_1_1);

      S_5 = w(5087 * S_1_1) + w(1056 * S_2_1) - 
            w(3947 * S_3_1) + w(4757 * S_4_1) + 
            w(9659 * S_5_1) - w(1702 * X_0_1) + 
            w(5519 * X_1_1);

      S_6 = w(9670 * S_1_1) + w(4141 * S_2_1) + 
            w(1503 * S_3_1) + w(7286 * S_4_1) + 
            w(6262 * S_5_1) - w(2114 * X_0_1) + 
            w(2007 * X_1_1);

      S_7 = w(1317 * S_1_1) + w(4936 * S_2_1) + 
            w(2123 * S_3_1) + w(1148 * S_4_1) + 
            w(4344 * S_5_1) + w(1194 * X_0_1) - 
            w(2114 * X_1_1);

      Y_0_1 = w(S_1)+ w(S_2)+ w(S_3)+ w(S_6_1) - w(1491 * X_0_1);
      Y_1_1 = w(S_6)+ w(S_4)+ w(S_5)+ w(S_7)+ w(S_7_1) + w(2007 * X_0_1) - w(1491 * X_1_1);

      result_out = Y_0_1 + Y_1_1;
      //      printf("%d\n", result_out);
      return result_out;
}

/*
Unfolding has been done i =w(   1   times
S_1@1   arrives at   Tj
S_2@1   arrives at   Tj
S_3@1   arrives at   Tj
S_4@1   arrives at   Tj
S_5@1   arrives at   Tj
S_6@1   arrives at   Tj
S_7@1   arrives at   Tj
X_0_1   arrives at (   0   *   TS   )
X_1_1   arrives at (   1   *   TS   )
S_1   needed at (   Tj   )+w(   2   *   TS   )
S_2   needed at (   Tj   )+w(   2   *   TS   )
S_3   needed at (   Tj   )+w(   2   *   TS   )
S_4   needed at (   Tj   )+w(   2   *   TS   )
S_5   needed at (   Tj   )+w(   2   *   TS   )
S_6   needed at (   Tj   )+w(   2   *   TS   )
S_7   needed at (   Tj   )+w(   2   *   TS   )
Y_0_1   needed at (   0   *   TS   )+w(   TL   )
Y_1_1   needed at (   1   *   TS   )+w(   TL   )
*/
#ifdef __cplusplus
}
#endif
//int i;//25

#define NUM 10
int i;//25
int result_out[NUM];
int inputs[NUM+4]={10,23,34,43,54,98,78,67,75,15,37,81,19,91};
#define exp_res 39735
int main()
{
    int main_result;
  short x01, x02, x03, x11, x12, x13, x21, x22, x23;
  int j, k;
  //  int result_out;  
   int return_value=0;
  for (int idx=0;idx<NUM;idx++)
    {
      i = inputs[idx]; 
      j = inputs[idx+2];
      k = inputs[idx+1];
  x01 = i++; x02 = i*2; x03 = j+2;
  x11 = j++; x12 = k++; x13 = (i*3)+1;
  x21 = j+2; x22 = (k*2)+2; x23 = j++;

result_out[idx] =  honda(x01, x02, 
           x03, x11, x12, x13,
		    x21, x22, x23);
 return_value = return_value+result_out[idx];
 //     printf("%d\n",result_out[idx]);
    }

//    printf("benchmark_result = %d\n",result_out);
//    main_result = (result_out != exp_res);
    //    printf("%d\n", main_result);
    //    return main_result;
  printf("return return_value != %d", return_value);
  //return return_value;
    //  return 0;
	  return return_value != -49639 ;
}
