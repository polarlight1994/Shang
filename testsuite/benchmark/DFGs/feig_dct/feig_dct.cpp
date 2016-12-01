/* C synthesizable-behavioral-style description of FEIG_DCT
   Created by Zhiru Zhang, 04/02/2005
   VLSI CAD Lab, UCLA
*/

/*  Feig's 8X8 DCT  */

typedef short ssdmbuff;
#include <stdio.h>
short _ssdm_BuffRead(ssdmbuff f);
void _ssdm_BuffWrite(ssdmbuff* f, short data);

/* #define num8 fix<9,8> */

#define num8 (short)

#define  half   (5000)
#define  a0     (7071)
#define  a1     (5411)
#define  a2     (-9238)
#define  a3     (13065)
#define  b1     (-3826)
#define  b2     (-6532)
#define  b3     (9238)
#define  c0     (3535)
#define  gamma2     (9238)
#define  gamma2m    (-9238)
#define  gamma6     (3826)
#define  gamma22    (6532)
#define  gamma22m   (-6532)
#define  gamma62    (2705)

#define R1(x0,x1,x2,x3,x4,x5,x6,x7, y0,y1,y2,y3,y4,y5,y6,y7) \
     aa0 = x0 + x7; \
     aa1 = x1 + x6; \
     aa2 = x2 + x5; \
     aa3 = x3 + x4; \
     aa4 = x0 - x7; \
     aa5 = x1 - x6; \
     aa6 = x2 - x5; \
     aa7 = x3 - x4; \
     bb0 = aa0 + aa3; \
     bb2 = aa0 - aa2; \
     bb3 = aa1 - aa2; \
     y0 = bb0 + aa2; \
     y1 = aa1 - bb0; \
     y2 = bb3; \
     y3 = bb2 - bb3; \
     y4 = aa6; \
     y5 = aa7 - aa4; \
     y6 = aa4 - aa6; \
     y7 = aa5 + aa7; 


#define M1(x0,x1,x2,x3,x4,x5,x6,x7, y0,y1,y2,y3,y4,y5,y6,y7) \
     y0 = x0; \
     y1 = x1; \
     y2 = x2; \
     y3 = num8 (a0 * x3); \
     y4 = x4; \
     y5 = num8 (a0 * x5); \
     aa = num8 (a2 * (x6 + x7)); \
     y6 = num8 (a1 * x6) + aa; \
     y7 = num8 (a3 * x7) + aa; 

#define M2(x0,x1,x2,x3,x4,x5,x6,x7, y0,y1,y2,y3,y4,y5,y6,y7) \
    y0 = num8 (a0 * x0); \
    y1 = num8 (a0 * x1); \
    y2 = num8 (a0 * x2); \
    y3 = num8 (half * x3); \
    y4 = num8 (a0 * x4); \
    y5 = num8 (half * x5); \
    aa = num8 (b2 * (x6 + x7)); \
    y6 = num8 (b1 * x6) + aa; \
    y7 = num8 (b3 * x7) + aa; 

    
#define R2(x0,x1,x2,x3,x4,x5,x6,x7, y0,y1,y2,y3,y4,y5,y6,y7) \
    y0 = x0; y1 = x1; \
    y2 = x2 + x3; \
    y3 = x3 - x2; \
    aa4 = x4 + x5; \
    aa5 = x5 - x4; \
    y4 = aa4 - x6; \
    y5 = aa5 + x7; \
    y6 =  num8 (( -1) * aa4) - x6; \
    y7 = x7 - aa5; 

#define Q3(x0, x1, x2, x3, y0, y1, y2, y3) \
    aa0 = x0 - x3; \
    aa1 = x1 + x2; \
    aa2 = x0 + x3; \
    aa3 = x1 - x2; \
    bb0 = num8 (c0 * aa0); \
    bb1 = num8 (c0 * aa1); \
    bb2 = num8 (half * aa2); \
    bb3 = num8 (half * aa3); \
    cc0 = bb1 - bb0; \
    cc1 = -bb0 - bb1; \
    y0 = cc0 + bb2; \
    y1 = cc1 + bb3; \
    y2 = cc1 - bb3; \
    y3 = cc0 - bb2; 


#define Q1(x0, x1, y0, y1) \
      y0 = num8 (gamma6 * x0) + num8 (gamma2 * x1); \
      y1 = num8 (gamma2m * x0) + num8 (gamma6 * x1); 

#define Q2(x0, x1, y0, y1) \
      y0 = num8 (gamma62 * x0) + num8 (gamma22 * x1); \
      y1 = num8 (gamma22m * x0) + num8 (gamma62 * x1); 

#define M3(x00, x01, x02, x03, x04, x05, x06, x07, x10, x11, x12, x13, x14, x15, x16, x17, y00, y01, y02, y03, y04, y05, y06, y07, y10, y11, y12, y13, y14, y15, y16, y17) \
        Q1(x00,x10, y00, y10) Q1(x01,x11, y01, y11) \
Q1(x02,x12, y02, y12) \
Q2(x03,x13, y03, y13) \
Q1(x04,x14, y04, y14) \
Q2(x05,x15, y05, y15) \
Q3(x06, x07, x16, x17, y06, y07, y16, y17)

#ifdef __cplusplus
extern "C" {
#endif

int feig_dct(short in00, short in01, short in02, short in03,
              short in04, short in05, short in06, short in07,
              short in10, short in11, short in12, short in13,
              short in14, short in15, short in16, short in17,
              short in20, short in21, short in22, short in23,
              short in24, short in25, short in26, short in27, 
              short in30, short in31, short in32, short in33,
              short in34, short in35, short in36, short in37,
              short in40, short in41, short in42, short in43,
              short in44, short in45, short in46, short in47,
              short in50, short in51, short in52, short in53,
              short in54, short in55, short in56, short in57,
              short in60, short in61, short in62, short in63,
              short in64, short in65, short in66, short in67,
              short in70, short in71, short in72, short in73,
              short in74, short in75, short in76, short in77);
   
int feig_dct(short in00, short in01, short in02, short in03,
              short in04, short in05, short in06, short in07,
              short in10, short in11, short in12, short in13,
              short in14, short in15, short in16, short in17,
              short in20, short in21, short in22, short in23,
              short in24, short in25, short in26, short in27, 
              short in30, short in31, short in32, short in33,
              short in34, short in35, short in36, short in37,
              short in40, short in41, short in42, short in43,
              short in44, short in45, short in46, short in47,
              short in50, short in51, short in52, short in53,
              short in54, short in55, short in56, short in57,
              short in60, short in61, short in62, short in63,
              short in64, short in65, short in66, short in67,
              short in70, short in71, short in72, short in73,
              short in74, short in75, short in76, short in77)
{
short
    out00,out01,out02,out03,
    out04,out05,out06,out07,
    out10,out11,out12,out13,
    out14,out15,out16,out17,
    out20,out21,out22,out23,
    out24,out25,out26,out27, 
    out30,out31,out32,out33,
    out34,out35,out36,out37,
    out40,out41,out42,out43,
    out44,out45,out46,out47,
    out50,out51,out52,out53,
    out54,out55,out56,out57,
    out60,out61,out62,out63,
    out64,out65,out66,out67,
    out70,out71,out72,out73,
    out74,out75,out76,out77; 

    short 
    fi00,fi01,fi02,fi03,
    fi04,fi05,fi06,fi07,
    fi10,fi11,fi12,fi13,
    fi14,fi15,fi16,fi17,
    fi20,fi21,fi22,fi23,
    fi24,fi25,fi26,fi27, 
    fi30,fi31,fi32,fi33,
    fi34,fi35,fi36,fi37,
    fi40,fi41,fi42,fi43,
    fi44,fi45,fi46,fi47,
    fi50,fi51,fi52,fi53,
    fi54,fi55,fi56,fi57,
    fi60,fi61,fi62,fi63,
    fi64,fi65,fi66,fi67,
    fi70,fi71,fi72,fi73,
    fi74,fi75,fi76,fi77, 

    se00,se01,se02,se03,
    se04,se05,se06,se07,
    se10,se11,se12,se13,
    se14,se15,se16,se17,
    se20,se21,se22,se23,
    se24,se25,se26,se27, 
    se30,se31,se32,se33,
    se34,se35,se36,se37,
    se40,se41,se42,se43,
    se44,se45,se46,se47,
    se50,se51,se52,se53,
    se54,se55,se56,se57,
    se60,se61,se62,se63,
    se64,se65,se66,se67,
    se70,se71,se72,se73,
    se74,se75,se76,se77, 
    
    th00,th01,th02,th03,
    th04,th05,th06,th07,
    th10,th11,th12,th13,
    th14,th15,th16,th17,
    th20,th21,th22,th23,
    th24,th25,th26,th27, 
    th30,th31,th32,th33,
    th34,th35,th36,th37,
    th40,th41,th42,th43,
    th44,th45,th46,th47,
    th50,th51,th52,th53,
    th54,th55,th56,th57,
    th60,th61,th62,th63,
    th64,th65,th66,th67,
    th70,th71,th72,th73,
    th74,th75,th76,th77, 

    fv00,fv01,fv02,fv03,
    fv04,fv05,fv06,fv07,
    fv10,fv11,fv12,fv13,
    fv14,fv15,fv16,fv17,
    fv20,fv21,fv22,fv23,
    fv24,fv25,fv26,fv27, 
    fv30,fv31,fv32,fv33,
    fv34,fv35,fv36,fv37,
    fv40,fv41,fv42,fv43,
    fv44,fv45,fv46,fv47,
    fv50,fv51,fv52,fv53,
    fv54,fv55,fv56,fv57,
    fv60,fv61,fv62,fv63,
    fv64,fv65,fv66,fv67,
    fv70,fv71,fv72,fv73,
    fv74,fv75,fv76,fv77;

    short aa, aa0, aa1, aa2, aa3, aa4, aa5, aa6, aa7,
          bb0, bb1, bb2, bb3, cc0, cc1;
    int result_out;
/* the first stage R  */
 
  R1(in00, in01, in02, in03, in04, in05, in06, in07, fi00, fi01, fi02, fi03, fi04, fi05, fi06, fi07)
    
  R1(in10, in11, in12, in13, in14, in15, in16, in17, fi10, fi11, fi12, fi13, fi14, fi15, fi16, fi17)

  R1(in20, in21, in22, in23, in24, in25, in26, in27, fi20, fi21, fi22, fi23, fi24, fi25, fi26, fi27)
    
  R1(in30, in31, in32, in33, in34, in35, in36, in37, fi30, fi31, fi32, fi33, fi34, fi35, fi36, fi37)
          
  R1(in40, in41, in42, in43, in44, in45, in46, in47, fi40, fi41, fi42, fi43, fi44, fi45, fi46, fi47)
  
  R1(in50, in51, in52, in53, in54, in55, in56, in57, fi50, fi51, fi52, fi53, fi54, fi55, fi56, fi57)
  
  R1(in60, in61, in62, in63, in64, in65, in66, in67, fi60, fi61, fi62, fi63, fi64, fi65, fi66, fi67) 
  
  R1(in70, in71, in72, in73, in74, in75, in76, in77, fi70, fi71, fi72, fi73, fi74, fi75, fi76, fi77)
  
/*  the second stage, after permutaion of rows and columns R */
  
  R1(fi00, fi10, fi20, fi30, fi40, fi50, fi60, fi70, se00, se10, se20, se30, se40, se50, se60, se70)
  
  R1(fi01, fi11, fi21, fi31, fi41, fi51, fi61, fi71, se01, se11, se21, se31, se41, se51, se61, se71)
  
  R1(fi02, fi12, fi22, fi32, fi42, fi52, fi62, fi72, se02, se12, se22, se32, se42, se52, se62, se72)

  R1(fi03, fi13, fi23, fi33, fi43, fi53, fi63, fi73, se03, se13, se23, se33, se43, se53, se63, se73)
  
  R1(fi04, fi14, fi24, fi34, fi44, fi54, fi64, fi74, se04, se14, se24, se34, se44, se54, se64, se74)

  R1(fi05, fi15, fi25, fi35, fi45, fi55, fi65, fi75, se05, se15, se25, se35, se45, se55, se65, se75)

  R1(fi06, fi16, fi26, fi36, fi46, fi56, fi66, fi76, se06, se16, se26, se36, se46, se56, se66, se76)
  
  R1(fi07, fi17, fi27, fi37, fi47, fi57, fi67, fi77, se07, se17, se27, se37, se47, se57, se67, se77)

/*  the third stage:  multiplications by Mi */
  
  M1(se00, se10, se20, se30, se40, se50, se60, se70,th00, th10, th20, th30, th40, th50, th60, th70)

  M1(se01, se11, se21, se31, se41, se51, se61, se71, th01, th11, th21, th31, th41, th51, th61, th71)
  
  M1(se02, se12, se22, se32, se42, se52, se62, se72, th02, th12, th22, th32, th42, th52, th62, th72)
  
  M2(se03, se13, se23, se33, se43, se53, se63, se73, th03, th13, th23, th33, th43, th53, th63, th73)
        
  M1(se04, se14, se24, se34, se44, se54, se64, se74, th04, th14, th24, th34, th44, th54, th64, th74)
  
  M1(se05, se15, se25, se35, se45, se55, se65, se75, th05, th15, th25, th35, th45, th55, th65, th75)
  
  M3(se06, se16, se26, se36, se46, se56, se66, se76, se07, se17, se27, se37, se47, se57, se67, se77, th06, th16, th26, th36, th46, th56, th66, th76, th07, th17, th27, th37, th47, th57, th67, th77)
  
/*  the fourth stage  */
        
  R2(th00, th10, th20, th30, th40, th50, th60, th70, fv00, fv10, fv20, fv30, fv40, fv50, fv60, fv70)
        
  R2(th01, th11, th21, th31, th41, th51, th61, th71, fv01, fv11, fv21, fv31, fv41, fv51, fv61, fv71)
  
  R2(th02, th12, th22, th32, th42, th52, th62, th72, fv02, fv12, fv22, fv32, fv42, fv52, fv62, fv72)

  R2(th03, th13, th23, th33, th43, th53, th63, th73, fv03, fv13, fv23, fv33, fv43, fv53, fv63, fv73)
        
  R2(th04, th14, th24, th34, th44, th54, th64, th74, fv04, fv14, fv24, fv34, fv44, fv54, fv64, fv74)
  
  R2(th05, th15, th25, th35, th45, th55, th65, th75, fv05, fv15, fv25, fv35, fv45, fv55, fv65, fv75)
        
  R2(th06, th16, th26, th36, th46, th56, th66, th76, fv06, fv16, fv26, fv36, fv46, fv56, fv66, fv76)
        
  R2(th07, th17, th27, th37, th47, th57, th67, th77, fv07, fv17, fv27, fv37, fv47, fv57, fv67, fv77)


/* the fifth stage R  */
        
  R2(fv00, fv01, fv02, fv03, fv04, fv05, fv06, fv07, out00, out01, out02, out03, out04, out05, out06, out07)
  
  R2(fv10, fv11, fv12, fv13, fv14, fv15, fv16, fv17, out10, out11, out12, out13, out14, out15, out16, out17)
  
  R2(fv20, fv21, fv22, fv23, fv24, fv25, fv26, fv27, out20, out21, out22, out23, out24, out25, out26, out27)
        
  R2(fv30, fv31, fv32, fv33, fv34, fv35, fv36, fv37, out30, out31, out32, out33, out34, out35, out36, out37)

  R2(fv40, fv41, fv42, fv43, fv44, fv45, fv46, fv47, out40, out41, out42, out43, out44, out45, out46, out47)
  
  R2(fv50, fv51, fv52, fv53, fv54, fv55, fv56, fv57, out50, out51, out52, out53, out54, out55, out56, out57)
        
  R2(fv60, fv61, fv62, fv63, fv64, fv65, fv66, fv67, out60, out61, out62, out63, out64, out65, out66, out67)
        
  R2(fv70, fv71, fv72, fv73, fv74, fv75, fv76, fv77, out70, out71, out72, out73, out74, out75, out76, out77)
  
    result_out = out00+out01+out02+out03+out04+out05+out06+out07;
    result_out = result_out + out10+out11+out12+out13+out14+out15+out16+out17;
    result_out = result_out + out20+out21+out22+out23+out24+out25+out26+out27;
    result_out = result_out + out30+out31+out32+out33+out34+out35+out36+out37;
    result_out = result_out + out40+out41+out42+out43+out44+out45+out46+out47;
    result_out = result_out + out50+out51+out52+out53+out54+out55+out56+out57;
    result_out = result_out + out60+out61+out62+out63+out64+out65+out66+out67;
    result_out = result_out + out70+out71+out72+out73+out74+out75+out76+out77;
    //    printf("%d\n",result_out);
    return result_out;
}
#ifdef __cplusplus
}
#endif

//int i;//25

#define NUM 10
int result_out[NUM];
int inputs[NUM+5]={10,23,34,43,54,98,78,67,75,15,62,74,82,19,22};

#define exp_res 61640
int main()
{
    int main_result;
   int return_value=0;
   int i, j, k, l;
short in00,  in01,  in02,  in03,
               in04,  in05,  in06,  in07,
               in10,  in11,  in12,  in13,
               in14,  in15,  in16,  in17,
               in20,  in21,  in22,  in23,
               in24,  in25,  in26,  in27, 
               in30,  in31,  in32,  in33,
               in34,  in35,  in36,  in37,
               in40,  in41,  in42,  in43,
               in44,  in45,  in46,  in47,
               in50,  in51,  in52,  in53,
               in54,  in55,  in56,  in57,
               in60,  in61,  in62,  in63,
               in64,  in65,  in66,  in67,
               in70,  in71,  in72,  in73,
               in74,  in75,  in76,  in77;

//  int result_out;  
  for (int idx=0;idx<NUM;idx++)
    {
      i = inputs[idx];
      j = inputs[idx+1];
      k = inputs[idx+2];
      l = inputs[idx+3];
  in00=i++;in01=j++;in02=k++;in03=i++; in04=l++;in05=j++;in06=k++;in07=l++;
  in10=i++;in11=j++;in12=k++;in13=l++; in14=i++;in15=i++;in16=i++;in17=i++;
  in20=i++;in21=j++;in22=k++;in23=l++; in24=j++;in25=k++;in26=l++;in27=i++;
  in30=k++;in31=j++;in32=k++;in33=l++; in34=j++;in35=k++;in36=l++;in37=j++;
  in40=i++;in41=j++;in42=k++;in43=l++; in44=j++;in45=k++;in46=l++;in47=j++;
  in50=i++;in51=i++;in52=k++;in53=l++; in54=j++;in55=k++;in56=l++;in57=j++;
  in60=i++;in61=k++;in62=k++;in63=l++; in64=i++;in65=i++;in66=i++;in67=i++;
  in70=l++;in71=k++;in72=l++;in73=k++; in74=i++;in75=l++;in76=j++;in77=k++;

result_out[idx] =  feig_dct( in00,  in01,  in02,  in03,
               in04,  in05,  in06,  in07,
               in10,  in11,  in12,  in13,
               in14,  in15,  in16,  in17,
               in20,  in21,  in22,  in23,
               in24,  in25,  in26,  in27, 
               in30,  in31,  in32,  in33,
               in34,  in35,  in36,  in37,
               in40,  in41,  in42,  in43,
               in44,  in45,  in46,  in47,
               in50,  in51,  in52,  in53,
               in54,  in55,  in56,  in57,
               in60,  in61,  in62,  in63,
               in64,  in65,  in66,  in67,
               in70,  in71,  in72,  in73,
		        in74,  in75,  in76,  in77);
  return_value = return_value+result_out[idx];
  //     printf("%d\n",result_out[idx]);
    }
//    printf("benchmark_result = %d\n",result_out);
//    main_result = (result_out != exp_res);
    //        printf("%d\n", main_result);
//  return main_result;
	printf("return return_value != %d", return_value);
//    return return_value;
    // return 0;
  return return_value != -673245;
}
