#define w (short)
#include <stdio.h>
#define NUM 64
#ifdef __cplusplus
extern "C" {
#endif
int single_add(int x, int y) __attribute__ ((noinline));

int single_add(int x, int y) {
  return x+y;
}
#ifdef __cplusplus
}
#endif

#define exp_res 39735
int main()
{
  static int array0[NUM] = {61251, 39938, 557, 44087, 47457, 34892, 10951, 27320, 52441, 19223, 37907, 17733, 7955, 63768, 28004, 49123, 8207, 12611, 65416, 5149, 57091, 60947, 38247, 547, 49697, 41377, 18871, 18062, 28699, 44195, 62595, 22690, 9267, 127, 35116, 22432, 22036, 49858, 38900, 17577, 23766, 17334, 36673, 63610, 26343, 63308, 43736, 31387, 38191, 22004, 26629, 15195, 39604, 13415, 41537, 11555, 9773, 22456, 33278, 23191, 41678, 27438, 6844, 36947};
  static int array1[NUM] = {11607, 534, 19214, 64367, 27918, 19012, 37072, 979, 56019, 23092, 54035, 64188, 27923, 26057, 52053, 56972, 7918, 59126, 41985, 42020, 16517, 26104, 52278, 10423, 49869, 5984, 42782, 33530, 49211, 46241, 15456, 60220, 32410, 56650, 60279, 54196, 53804, 28761, 38523, 29573, 54733, 15360, 39285, 43816, 31873, 26499, 22319, 38246, 62792, 38988, 48511, 23212, 12372, 18176, 30231, 36577, 7205, 42957, 47135, 9895, 11447, 60186, 44430, 35845};
  static int array2[NUM] = {11932, 42413, 31723, 3896, 844, 23911, 65491, 24313, 52609, 31417, 20721, 60632, 49059, 5862, 39053, 10167, 36890, 38733, 8956, 28278, 48035, 4305, 37304, 21307, 42690, 19462, 4362, 46911, 36549, 1674, 35349, 45646, 12323, 9478, 64330, 31680, 4579, 45000, 20212, 43913, 53085, 18535, 61445, 11974, 30590, 25140, 44513, 48028, 19706, 8074, 42629, 23593, 34769, 55387, 43049, 28729, 59843, 13400, 16779, 412, 38532, 17896, 62463, 34061};

  long result = 0;
  for (unsigned i =0; i < NUM; ++i) {
    result += single_add(array0[i], array1[i]);
  }

  //printf("%ld\n", result);
  return result != 4245205;
}
