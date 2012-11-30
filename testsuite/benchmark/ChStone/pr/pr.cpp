/* C synthesizable-behavioral-style description of PR
   Created by Yiping Fan, 11/12/2004
   VLSI CAD Lab, UCLA
*/


#ifdef __cplusplus
extern "C" {
#endif
int pr(short x10, short x11, short x12, short x13,
        short x14, short x15, short x16, short x17)  __attribute__ ((noinline));

int pr(short x10, short x11, short x12, short x13,
        short x14, short x15, short x16, short x17)
{
    short x20, x21, x22, x23, x24, x25, x26, x27,
          x30, x31, x32, x33, x34, x35, x36, x37,
          x40, x41, x42, x43, x44, x45, x46, x47,
          x50, x51, x52, x53, x54, x55, x56, x57;

short y10, y11, y12, y13, y14, y15,y16, y17;
 int result_out;
    
    x20 = x10 + x17;
    x21 = x11 + x12;
    x22 = x11 - x12;
    x23 = x13 + x14;
    x24 = x13 - x14;
    x25 = x15 + x16;
    x26 = x15 - x16;
    x27 = x10 - x17;
    
    x30 = x20 + x23;
    x31 = x21 + x25;
    x32 = x22 - x26;
    x33 = x20 - x23;
    x34 = x24;
    x35 = x21 - x25;
    x36 = x22 + x26;
    x37 = x27;
    
    x40 = x30 + x31;
    x41 = x30 - x31;
    x42 = x32;
    x43 = x33;
    x44 = x34;
    x45 = 7071 * x35;
    x46 = 7071 * x36;
    x47 = x37;
    
    x50 = x40;
    x51 = x41;
    x52 = x42;
    x53 = x43;

    x54 = x44 + x46;
    x55 = x45 + x47;
    x56 = x44 - x46;
    x57 = x47 - x45;


    y10 = 7071 * x50;
    y14 = 7071 * x51;
    y12 = 3827 * x52 + 9239 * x53;
    y16 = 3827 * x53 - 9239 * x52;

    y11 = 9807 * x55 + 1951 * x54;
    y17 = 1951 * x55 - 9807 * x54;

    y13 = 8315 * x57 - 5556 * x56;
    y15 = 5556 * x57 + 8315 * x56;

    result_out = y10+y11+y12+y13+y14+y15+y16+y17;
    //    printf("%d\n",result_out);
    return result_out;
}
#ifdef __cplusplus
}
#endif

#define NUM 10
int i;//25
int result_out[NUM];
int inputs[NUM]={10,23,34,43,54,98,78,67,75,15};

#define exp_res 26500
int main()
{
    int main_result;
    short x01, x02, x03, x11, x12, x13, x21, x22;
    //    int result_out;  
    

  for (int idx=0;idx<NUM;idx++)
    {
      i = inputs[idx];
      x01 = i++; x02 = i++; x03 = i++;
      x11 = i++; x12 = i++; x13 = i++;
      x21 = i++; x22 = i++;
      result_out[idx] = pr(x01,x02,x03,x11,x12,x13,x21, x22);
     printf("%d\n",result_out[idx]);
    }
  //    printf("benchmark_result = %d\n",result_out);
  //    main_result = (result_out != exp_res);
    //        printf("%d\n", main_result);
    //    return main_result;
      return 0;
}
