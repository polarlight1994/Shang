#include <stdio.h>
/*#define num16 fix<8,8> */
#define num16 (short)
/*#define num12 fix <12,11> */
#define num12 (short)
/*#define num1 fix<1,0>*/

#define Const_X0_1 num12(3535)
#define Const_X1_1 num12(17)
#define Const_X1_2 num12(39)
#define Const_X1_3 num12(156)
#define Const_X1_4 num12(937)
#define Const_X1_5 num12(2778)
#define Const_X1_6 num12(5078)
#define Const_X2_1 num12(1914)
#define Const_X2_2 num12(4619)
#define Const_X3_1 num12(17)
#define Const_X3_2 num12(39)
#define Const_X3_3 num12(156)
#define Const_X3_4 num12(937)
#define Const_X3_5 num12(2778)
#define Const_X3_6 num12(5078)
#define Const_X4_1 num12(3535)
#define Const_X5_1 num12(17)
#define Const_X5_2 num12(39)
#define Const_X5_3 num12(156)
#define Const_X5_4 num12(937)
#define Const_X5_5 num12(2778)
#define Const_X5_6 num12(5078)
#define Const_X6_1 num12(1914)
#define Const_X6_2 num12(4619)
#define Const_X7_1 num12(17)
#define Const_X7_2 num12(39)
#define Const_X7_3 num12(156)
#define Const_X7_4 num12(937)
#define Const_X7_5 num12(2778)
#define Const_X7_6 num12(5078)
#ifdef __cplusplus
extern "C" {
#endif
int  mcm(short X0, short X1, short X2, short X3,
         short X4, short X5, short X6, short X7);

int  mcm(short X0, short X1, short X2, short X3,
         short X4, short X5, short X6, short X7)

{
    short
        MX0_1,
        MX1_1, MX1_2, MX1_3, MX1_4, MX1_5, MX1_6, ImX1_7, ImX1_8, ImX1_9,
        MX2_1, MX2_2,
        MX3_1, MX3_2, MX3_3, MX3_4, MX3_5, MX3_6, ImX3_7, ImX3_8, ImX3_9,
        MX4_1, 
        MX5_1, MX5_2, MX5_3, MX5_4, MX5_5, MX5_6, ImX5_7, ImX5_8, ImX5_9,
        MX6_1, MX6_2,
        MX7_1, MX7_2, MX7_3, MX7_4, MX7_5, MX7_6, ImX7_7, ImX7_8, ImX7_9,
        Sum43, Sum44, Sum45, Sum46;
        short Y0, Y1, Y2, Y3;
	short Y4, Y5, Y6, Y7;   
	int result_out;
 
    MX0_1 = num16(Const_X0_1 * X0);


    MX1_1 = num16(Const_X1_1 * X1);
    MX1_2 = num16(Const_X1_2 * X1);
    MX1_3 = num16(Const_X1_3 * X1);
    MX1_4 = num16(Const_X1_4 * X1);
    MX1_5 = num16(Const_X1_5 * X1);
    MX1_6 = num16(Const_X1_6 * X1);
    ImX1_7 = MX1_2 + MX1_4 ;
    ImX1_8 =  MX1_6 + MX1_1 - MX1_4 ;
    ImX1_9 =  MX1_6 - MX1_1 - MX1_3;


    MX2_1 = num16(Const_X2_1 * X2);
    MX2_2 = num16(Const_X2_2 * X2);


    MX3_1 = num16(Const_X3_1 * X3);
    MX3_2 = num16(Const_X3_2 * X3);
    MX3_3 = num16(Const_X3_3 * X3);
    MX3_4 = num16(Const_X3_4 * X3);
    MX3_5 = num16(Const_X3_5 * X3);
    MX3_6 = num16(Const_X3_6 * X3);
    ImX3_7 = MX3_2 + MX3_4 ;
    ImX3_8 =  MX3_6 + MX3_1 - MX3_4;
    ImX3_9 =  MX3_6 - MX3_1 - MX3_3;


    MX4_1 = num16(Const_X4_1 * X4);


    MX5_1 = num16(Const_X5_1 * X5);
    MX5_2 = num16(Const_X5_2 * X5);
    MX5_3 = num16(Const_X5_3 * X5);
    MX5_4 = num16(Const_X5_4 * X5);
    MX5_5 = num16(Const_X5_5 * X5);
    MX5_6 = num16(Const_X5_6 * X5);
    ImX5_7 = MX5_2 + MX5_4 ;
    ImX5_8 = MX5_6 -MX5_4 + MX5_1 ;
    ImX5_9 = MX5_6 - MX5_3 - MX5_1 ;


    MX6_1 = num16(Const_X6_1 * X6);
    MX6_2 = num16(Const_X6_2 * X6);


    MX7_1 = num16(Const_X7_1 * X7);
    MX7_2 = num16(Const_X7_2 * X7);
    MX7_3 = num16(Const_X7_3 * X7);
    MX7_4 = num16(Const_X7_4 * X7);
    MX7_5 = num16(Const_X7_5 * X7);
    MX7_6 = num16(Const_X7_6 * X7);
    ImX7_7 = MX7_2 + MX7_4 ;
    ImX7_8 = MX7_6 -MX7_4 + MX7_1 ;
    ImX7_9 = MX7_6 -MX7_3 - MX7_1 ;


    Sum46 = MX0_1 - MX2_2 - MX6_1 + MX4_1 ;
    Sum45 = MX6_2 - MX2_1 - MX4_1 + MX0_1 ;
    Sum44 = MX2_1 - MX4_1 - MX6_2 + MX0_1 ;
    Sum43 = MX0_1 + MX2_2 + MX4_1 + MX6_1 ;


    Y0 = ImX1_9 + ImX3_8 + MX5_5 + ImX7_7 + Sum43 ;
    Y1 = ImX1_8 - ImX3_7 - ImX5_9 - MX7_5 + Sum44 ;
    Y2 = MX1_5 - ImX3_9 + ImX5_7 + ImX7_8 + Sum45 ;
    Y3 = ImX1_7 - MX3_5 + ImX5_8 - ImX7_9 + Sum46 ;
    Y4 = Sum46 - ImX1_7 + MX3_5 - ImX5_8 + ImX7_9 ;
    Y5 = Sum45 - MX1_5 + ImX3_9 - ImX5_7 - ImX7_8 ;
    Y6 = Sum44 - ImX1_8 + ImX3_7 + ImX5_9 + MX7_5 ;
    Y7 = Sum43 - ImX1_9 - ImX3_8 - MX5_5 - ImX7_7 ;

    result_out = (Y0 + Y1 + Y2 + Y3 + Y4 + Y5 + Y6 +Y7);
    //    printf("%d\n", result_out);
    return result_out;
}
#ifdef __cplusplus
}
#endif

#define NUM 10
int result_out[NUM];
int inputs[NUM+5]={10,23,34,43,54,98,78,67,75,15, 74, 81, 19, 45, 63};

//int i;//25
#define exp_res 13896
int main()
{
    int main_result;
    int x01, x02, x03, x11, x12, x13, x21, x22;
   int return_value=0;
   int i, j, k, l;
  for (int idx =0;idx<NUM;idx++)
    {
    i=inputs[idx];
    j = inputs[idx+1];
    k = inputs[idx+2];
    l = inputs[idx+3];
  x01 = i++; x02 = i++; x03 = l++;
  x11 = j++; x12 = k++; x13 = i++;
  x21 = i++; x22 = j++; 
      result_out[idx] =  mcm(x01     , x02     , x03     , x11     ,
		  x12     , x13     , x21     , x22     );
  return_value = return_value+result_out[idx];
  //     printf("%d\n",result_out[idx]);
    }
//      printf("benchmark_result = %d\n",result_out);
//    main_result = (result_out != exp_res);
              printf("%d\n", return_value);
    //    return main_result;
    //    return 0;
  return return_value != 30456;
}
