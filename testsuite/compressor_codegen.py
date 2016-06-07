import re
import sys
import os
import inspect
import codecs

BASE_DIR = ''
PROJECT_NAME = ''
DOT_MATRIXS_NAME_LIST = []

DOT_MATRIX_NAME = ''
BITWIDTH = 0
OPERAND_NUM = 0
SIGN_BITWIDTH = 0
SIGN_OPERAND_NUM = 0
COMPRESSOR_LIST = dict()
COMPRESSOR_NO = 0
DEBUG_FILE_NO = 0

def parseProjectInfo():
  Info_group = []
  
  ProjectInfo = open('CompressorInfo.txt', 'r')
  InfoLines = ProjectInfo.readlines()
  ProjectInfo.close()

  Infos = InfoLines[0].split(',')
  Info_group.append(Infos[0])
  Info_group.append(Infos[1])

  return Info_group

def parseDotMatrix(Headline):
  MatrixInfo = []

  Info = Headline.split('-')
  MatrixInfo.append(Info[0])
  MatrixInfo.append(int(Info[1]))
  MatrixInfo.append(int(Info[2]))

  return MatrixInfo
  
def parseDotMatrixs():
  global DOT_MATRIXS_NAME_LIST
  
  DotMatrixsFilePath = os.path.join(BASE_DIR, PROJECT_NAME + '.dotmatrixs')
  DotMatrixsFile = open(DotMatrixsFilePath, 'r')
  DotMatrixs = DotMatrixsFile.readlines()
  DotMatrixsFile.close()

  Startline_num = 0
  Endline_num = 0
  while(Endline_num <= len(DotMatrixs) - 2):
    Headline = DotMatrixs[Startline_num]
    DotMatrixInfo = parseDotMatrix(Headline)
    DotMatrixName = DotMatrixInfo[0]
    DotMatrixRowNum = DotMatrixInfo[1]
    DotMatrixColNum = DotMatrixInfo[2]
    Endline_num = Startline_num + DotMatrixRowNum
    
    DotMatrixFilePath = os.path.join(BASE_DIR, DotMatrixName + '.dotmatrix')
    DotMatrixFile = open(DotMatrixFilePath, 'w')
    
    for i in range(Startline_num, Endline_num + 1):
      DotMatrixFile.write(DotMatrixs[i])
    DotMatrixFile.close()

    Startline_num = Endline_num + 1
    print(DotMatrixName)
    DOT_MATRIXS_NAME_LIST.append(DotMatrixName)

def getMatrixFromDotMatrixFile(FileName, Row_num, Col_num):
  DotMatrixFilePath = os.path.join(BASE_DIR, FileName + '.dotmatrix')
  DotMatrixFile = open(DotMatrixFilePath, 'r')
  DotMatrix = DotMatrixFile.readlines()
  DotMatrixFile.close()
  
  # initialize a empty matrix
  Matrix = [['1\'b0' for col in range(Col_num)] for row in range(Row_num)]
  
  # visit start from line 1 to skip the matrix info line
  for i in range(1, len(DotMatrix)):
    Operand = DotMatrix[i]
    Operand_Bits = Operand.split(',')
    
    for j in range(0, len(Operand_Bits)):
      Operand_Bit = Operand_Bits[j]
      
      if (Operand_Bit == '\n' or Operand_Bit == ''):
        continue
      
      # reverse the column order for reading convenient
      Matrix[i - 1][Col_num - 1 - j] = Operand_Bit
      
  return Matrix

def printDotMatrixForMatrix(Matrix, Dot_Matrix_Name, Operand_num, Operand_BitWdith):
  DotMatrixFile = open(Dot_Matrix_Name + '.dotmatrix', 'w')
  
  # print the dot matrix info
  DotMatrixFile.write(Dot_Matrix_Name + '-' + str(Operand_num) + '-' + str(Operand_BitWdith) + '\n')
  
  for row_no in range(len(Matrix)):
    Row = Matrix[row_no]
    
    for col_no in range(len(Row)):
      Element = Row[len(Row) - 1 - col_no]      
      DotMatrixFile.write(Element + ',')
      
      if (col_no == len(Row) - 1):
        DotMatrixFile.write('\n')
  
  DotMatrixFile.close()

def printMatrixForDebug(Matrix):
  global DEBUG_FILE_NO
  
  Matrix_file_name = DOT_MATRIX_NAME + '_matrix_%d.txt' % DEBUG_FILE_NO
  Matrix_file = open(Matrix_file_name, 'w')
  
  for row in Matrix:
    for col in row:
      Matrix_file.write(str(col) + '\t')

    Matrix_file.write('\n')

  DEBUG_FILE_NO += 1
  Matrix_file.close()
  
def parseSignBitPatternInDotMatrix():
  global SIGN_BITWIDTH
  global SIGN_OPERAND_NUM
  
  # get the corresponding matrix from the dot matrix file
  Matrix = getMatrixFromDotMatrixFile(DOT_MATRIX_NAME, OPERAND_NUM, BITWIDTH)
  
  # check if there are sign bit pattern in matrix
  Sign_bit_pattern_row_list = []
  Sign_bit_pattern_width = BITWIDTH
  for i in range(0, len(Matrix)):
    Operand = Matrix[i]    
    MSB = Operand[len(Operand) - 1]

    # ignore the known sign bit
    if (MSB == '1\'b0' or MSB == '1\'b1'):
      continue
    
    # count the sign bit numbers in current operand
    Current_sign_bit_pattern_width = 0
    for j in range(0, len(Operand)):
      if (Operand[len(Operand) - 1 - j] == MSB):
        Current_sign_bit_pattern_width += 1
      else:
        break
    
    # if there are only one sign bit, then it is useless
    if (Current_sign_bit_pattern_width != 1):
      Sign_bit_pattern_row_list.append(i)
      Sign_bit_pattern_width = min(Current_sign_bit_pattern_width, Sign_bit_pattern_width)
  
  # generate sign bit dot matrix and left behind matrix if there are sign bit pattern
  if (Sign_bit_pattern_width >= 2 and Sign_bit_pattern_width != BITWIDTH):
    Sign_bit_matrix = []
    
    for i in Sign_bit_pattern_row_list:
      Sign_bit_pattern_row = Matrix[i]
      
      Sign_bit_matrix_row = []
      for j in range(0, Sign_bit_pattern_width):
        Sign_bit_matrix_row.append(Sign_bit_pattern_row[BITWIDTH - 1 - j])
        Matrix[i][BITWIDTH - 1 - j] = '1\'b0'
        
      Sign_bit_matrix.append(Sign_bit_matrix_row)
    
    SIGN_BITWIDTH = Sign_bit_pattern_width
    SIGN_OPERAND_NUM = len(Sign_bit_pattern_row_list)
    
    printDotMatrixForMatrix(Sign_bit_matrix, DOT_MATRIX_NAME + '_signbit', SIGN_OPERAND_NUM, SIGN_BITWIDTH)
    printDotMatrixForMatrix(Matrix, DOT_MATRIX_NAME + '_leftbehind', OPERAND_NUM, BITWIDTH)
    
    return True
  
  return False

def transportMatrix(Matrix, Row_num, Col_num):
  T_Matrix = [['1\'b0' for col in range(Row_num)] for row in range(Col_num)]
  
  for row_no in range(len(Matrix)):
    Row = Matrix[row_no]
    
    for col_no in range(len(Row)):
      T_Matrix[col_no][row_no] = Row[col_no]
      
  return T_Matrix

def eliminateOneInTMatrix(T_Matrix):
  # count the number of 1'b1 in each row
  One_number_in_t_matrix = [0 for col in range(BITWIDTH)]
  
  for row_no in range(0, len(T_Matrix)):
    Row = T_Matrix[row_no]

    for col_no in range(0, len(Row)):
      if (T_Matrix[row_no][col_no] == '1\'b1'):
        One_number_in_t_matrix[row_no] += 1

  # caculate the final number of 1'b1 after optimize
  Final_one_number_in_t_matrix = [0 for col in range(len(One_number_in_t_matrix))]
  for i in range(0, len(One_number_in_t_matrix)):
    One_no = One_number_in_t_matrix[i]

    Remain_one_no = One_no % 2
    Carry_one_no = One_no // 2

    if (Remain_one_no == 1):
      Final_one_number_in_t_matrix[i] = 1
  
    if (i != len(One_number_in_t_matrix) - 1):
      One_number_in_t_matrix[i + 1] += Carry_one_no
  
  # insert the final 1'b1 into T_Matrix and eliminate all origin 1'b1
  New_t_matrix = []
  for row_no in range(0, len(T_Matrix)):
    Row = T_Matrix[row_no]
    New_row = []

    for col_no in range(0, len(Row)):
      New_row.append('1\'b0')

    assert len(Row) == len(New_row)
    New_t_matrix.append(New_row)

  assert len(T_Matrix) == len(New_t_matrix)
  
  for row_no in range(0, len(T_Matrix)):
    Row = T_Matrix[row_no]
    
    New_col_no = 0
    
    # insert the final 1'b1 in the front of row
    Final_one_no = Final_one_number_in_t_matrix[row_no]
    if (Final_one_no != 0):
      New_t_matrix[row_no][0] = '1\'b1'
      New_col_no += 1

    for col_no in range(0, len(Row)):
      if (T_Matrix[row_no][col_no] == '1\'b1'):
        continue
      else:
        if (New_col_no != len(New_t_matrix[row_no]) - 1):
          New_t_matrix[row_no][New_col_no] = T_Matrix[row_no][col_no]
        else:
          New_t_matrix[row_no].append(T_Matrix[row_no][col_no])
        New_col_no += 1

  printMatrixForDebug(New_t_matrix)
  
  return New_t_matrix

def sortTMatrix(T_Matrix):
  Max_col_no = 0
  for row_no in range(0, len(T_Matrix)):
    Row = T_Matrix[row_no]
    Max_col_no = max(Max_col_no, len(Row))

  New_t_matrix = [['1\'b0' for col in range(Max_col_no)] for row in range(len(T_Matrix))]

  for row_no in range(0, len(T_Matrix)):
    Row = T_Matrix[row_no]
    New_col_no = 0
  
    for col_no in range(0, len(Row)):
      if (T_Matrix[row_no][col_no] != '1\'b0'):
        New_t_matrix[row_no][New_col_no] = T_Matrix[row_no][col_no]
        New_col_no += 1

  return New_t_matrix

def getCompressStatus(T_Matrix):
  Compress_status = [0 for row in range(BITWIDTH)]
  
  for row_no in range(0,len(T_Matrix)):
    Row = T_Matrix[row_no]
    Element_no = 0

    for col_no in range(0, len(Row)):
      if (T_Matrix[row_no][col_no] != '1\'b0'):
        Element_no += 1

    Compress_status[row_no] = Element_no

  return Compress_status

def checkIdenticalCompressor(Triple_elements):
  Key_1 = Triple_elements[0] + Triple_elements[1] + Triple_elements[2]
  if (COMPRESSOR_LIST.get(Key_1)):
    return COMPRESSOR_LIST.get(Key_1)

  Key_2 = Triple_elements[0] + Triple_elements[2] + Triple_elements[1]
  if (COMPRESSOR_LIST.get(Key_2)):
    return COMPRESSOR_LIST.get(Key_2)

  Key_3 = Triple_elements[1] + Triple_elements[0] + Triple_elements[2]
  if (COMPRESSOR_LIST.get(Key_3)):
    return COMPRESSOR_LIST.get(Key_3)

  Key_4 = Triple_elements[1] + Triple_elements[2] + Triple_elements[0]
  if (COMPRESSOR_LIST.get(Key_4)):
    return COMPRESSOR_LIST.get(Key_4)

  Key_5 = Triple_elements[2] + Triple_elements[0] + Triple_elements[1]
  if (COMPRESSOR_LIST.get(Key_5)):
    return COMPRESSOR_LIST.get(Key_5)

  Key_6 = Triple_elements[2] + Triple_elements[1] + Triple_elements[0]
  if (COMPRESSOR_LIST.get(Key_6)):
    return COMPRESSOR_LIST.get(Key_6)

  return None
  
def compressFirstTripleColumns(T_Matrix, Stage, CompressPattern):
  Compress_status = getCompressStatus(T_Matrix)

  for row_no in range(0, len(T_Matrix) - 1):
    if (Compress_status[row_no] >= 3):
      Num = Compress_status[row_no] // 3
      
      for no in range(0, Num):
        Triple_elements = [T_Matrix[row_no][3*no], T_Matrix[row_no][3*no+1], T_Matrix[row_no][3*no+2]]

        T_Matrix[row_no][3*no] = '1\'b0'
        T_Matrix[row_no][3*no+1] = '1\'b0'
        T_Matrix[row_no][3*no+2] = '1\'b0'

        Sum_name = 'NULL'
        Carry_name = 'NULL'

        IdenticalCompressor = checkIdenticalCompressor(Triple_elements)
        if (IdenticalCompressor != None):
          Sum_name = IdenticalCompressor[0]
          Carry_name = IdenticalCompressor[1]
        else:
          if (CompressPattern == 2):
            Sum_name = 'sign_bit_sum_' + str(row_no) + '_' + str(no) + '_' + str(Stage)
            Carry_name = 'sign_bit_carry_' + str(row_no) + '_' + str(no) + '_' + str(Stage)
            Compressor_generator(Triple_elements, Sum_name, Carry_name)
          elif (CompressPattern == 3):
            Sum_name = 'left_behind_sum_' + str(row_no) + '_' + str(no) + '_' + str(Stage)
            Carry_name = 'left_behind_carry_' + str(row_no) + '_' + str(no) + '_' + str(Stage)
            Compressor_generator(Triple_elements, Sum_name, Carry_name)
          else:
            Sum_name = 'sum_' + str(row_no) + '_' + str(no) + '_' + str(Stage)
            Carry_name = 'carry_' + str(row_no) + '_' + str(no) + '_' + str(Stage)
            Compressor_generator(Triple_elements, Sum_name, Carry_name)

        T_Matrix[row_no].append(Sum_name)
        if (row_no <= BITWIDTH - 2):
          T_Matrix[row_no+1].append(Carry_name)

  if (Compress_status[BITWIDTH-1] >= 2):
    VerilogFilePath = os.path.join(BASE_DIR, PROJECT_NAME + '.sv')
    VerilogFile = open(VerilogFilePath, 'a')
    if (CompressPattern == 2):
      VerilogFile.write('wire sign_bit_result_MSB_%d' %Stage + ' = ' + T_Matrix[BITWIDTH-1][0])
    elif (CompressPattern == 3):
      VerilogFile.write('wire left_behind_result_MSB_%d' %Stage + ' = ' + T_Matrix[BITWIDTH-1][0])
    else:
      VerilogFile.write('wire result_MSB_%d' %Stage + ' = ' + T_Matrix[BITWIDTH-1][0])    
    T_Matrix[BITWIDTH-1][0] = '1\'b0'
    for no in range(1, Compress_status[BITWIDTH-1]):
      VerilogFile.write(' ^ ' + T_Matrix[BITWIDTH-1][no])
      T_Matrix[BITWIDTH-1][no] = '1\'b0'
    VerilogFile.write(';\n\n')
    if (CompressPattern == 2):
      T_Matrix[BITWIDTH-1].append('sign_bit_result_MSB_%d' %Stage)
    elif (CompressPattern == 3):
      T_Matrix[BITWIDTH-1].append('left_behind_result_MSB_%d' %Stage)
    else:
      T_Matrix[BITWIDTH-1].append('result_MSB_%d' %Stage)    

    VerilogFile.close()

  T_Matrix = sortTMatrix(T_Matrix)
  
  return T_Matrix
  
def compressTMatrix(T_Matrix, CompressPattern):
  Current_BitWidth = 0
  if (CompressPattern == 2):
    Current_BitWidth = SIGN_BITWIDTH
  else:
    Current_BitWidth = BITWIDTH

  # sort the T_Matrix
  T_Matrix = sortTMatrix(T_Matrix)
  
  printMatrixForDebug(T_Matrix)
  
  # start to compress the T_Matrix
  Need_to_compress = True
  Stage = 0
  while(Need_to_compress):
    # compress the first three columns in T_Matrix
    T_Matrix = compressFirstTripleColumns(T_Matrix, Stage, CompressPattern)
    printMatrixForDebug(T_Matrix)

    # get the compress status to decide when to stop compressing
    Compress_status = getCompressStatus(T_Matrix)
    Need_to_compress = False
    for row_no in range(0,len(T_Matrix)):
      if (Compress_status[row_no] >= 3):
        Need_to_compress = True

    Stage += 1
  
  # when there are only two column left, we need to collect them
  # and use a CPA to add them
  Dataa = []
  Datab = []
  
  # the MSB can be ignored since it can be handled with a Xor gate
  for row_no in range(0, len(T_Matrix) - 1):
    Dataa.append(T_Matrix[row_no][0])
    Datab.append(T_Matrix[row_no][1])

  VerilogFilePath = os.path.join(BASE_DIR, PROJECT_NAME + '.sv')
  VerilogFile = open(VerilogFilePath, 'a')
  
  if (CompressPattern == 2):
    VerilogFile.write('wire[%d-2:0] sign_bit_dataa = {' %Current_BitWidth)
  elif (CompressPattern == 3):
    VerilogFile.write('wire[%d-2:0] left_behind_dataa = {' %Current_BitWidth)
  else:
    VerilogFile.write('wire[%d-2:0] dataa = {' %Current_BitWidth)
  VerilogFile.write(str(Dataa[Current_BitWidth - 2]))
  for row_no in range(0, len(Dataa) - 1):
    VerilogFile.write(', ' + str(Dataa[len(Dataa) - 2 - row_no]))
  VerilogFile.write('};\n')

  if (CompressPattern == 2):
    VerilogFile.write('wire[%d-2:0] sign_bit_datab = {' %Current_BitWidth)
  elif (CompressPattern == 3):
    VerilogFile.write('wire[%d-2:0] left_behind_datab = {' %Current_BitWidth)
  else:
    VerilogFile.write('wire[%d-2:0] datab = {' %Current_BitWidth)
  VerilogFile.write(str(Datab[Current_BitWidth - 2]))
  for row_no in range(0, len(Datab) - 1):
    VerilogFile.write(', ' + str(Datab[len(Datab) - 2 - row_no]))
  VerilogFile.write('};\n\n')
  
  if (CompressPattern == 2):
    VerilogFile.write('wire[%d-1:0] ' %Current_BitWidth + 'sign_bit_add_result = sign_bit_dataa + sign_bit_datab;\n')
    VerilogFile.write('wire[%d-1:0] temp_sign_bit_result;\n' %Current_BitWidth)
    VerilogFile.write('assign temp_sign_bit_result[%d-1] = ' %Current_BitWidth + T_Matrix[Current_BitWidth - 1][0])
  elif (CompressPattern == 3):
    VerilogFile.write('wire[%d-1:0] ' %Current_BitWidth + 'left_behind_add_result = left_behind_dataa + left_behind_datab;\n')
    VerilogFile.write('wire[%d-1:0] left_behind_result;\n' %Current_BitWidth)
    VerilogFile.write('assign left_behind_result[%d-1] = ' %Current_BitWidth + T_Matrix[Current_BitWidth - 1][0])
  else:
    VerilogFile.write('wire[%d-1:0] ' %Current_BitWidth + 'add_result = dataa + datab;\n')
    VerilogFile.write('assign result[%d-1] = ' %Current_BitWidth + T_Matrix[Current_BitWidth - 1][0])
  
  for i in range(1, len(T_Matrix[Current_BitWidth - 1]) - 1):
    MSB = T_Matrix[Current_BitWidth - 1][i]
    if (MSB != '1\'b0'):
      VerilogFile.write(' ^ ' + MSB)
  
  if (CompressPattern == 2):
    VerilogFile.write(' ^ sign_bit_add_result[%d];\n' %(Current_BitWidth - 1))
    VerilogFile.write('assign temp_sign_bit_result[%d-2:0] = ' %Current_BitWidth + 'sign_bit_add_result[%d:0]' %(Current_BitWidth - 2) + ';\n\n')
  elif (CompressPattern == 3):
    VerilogFile.write(' ^ left_behind_add_result[%d];\n' %(Current_BitWidth - 1))
    VerilogFile.write('assign left_behind_result[%d-2:0] = ' %Current_BitWidth + 'left_behind_add_result[%d:0]' %(Current_BitWidth - 2) + ';\n\n')
  else:
    VerilogFile.write(' ^ add_result[%d];\n' %(Current_BitWidth - 1))
    VerilogFile.write('assign result[%d-2:0] = ' %Current_BitWidth + 'add_result[%d:0]' %(Current_BitWidth - 2) + ';\n\n')
  
  VerilogFile.close()

def Compressor_generator(Triple_elements, Sum_name, Carry_name):
  global COMPRESSOR_NO
  global COMPRESSOR_LIST
  
  VerilogFilePath = os.path.join(BASE_DIR, PROJECT_NAME + '.sv')
  VerilogFile = open(VerilogFilePath, 'a')
  VerilogFile.write('wire ' + Sum_name + ';\n' + \
                    'wire ' + Carry_name + ';\n' + \
                    'compressor_3_2 compressor_3_2_%d' %COMPRESSOR_NO + '(\n' + \
                    '\t.a(' + Triple_elements[0] + '),\n' + \
                    '\t.b(' + Triple_elements[1] + '),\n' + \
                    '\t.cin(' + Triple_elements[2] + '),\n' + \
                    '\t.result(' + Sum_name + '),\n' + \
                    '\t.cout(' + Carry_name + ')\n' + \
                    ');\n\n')
  VerilogFile.close()

  Key = Triple_elements[0] + Triple_elements[1] + Triple_elements[2]
  Value = [Sum_name, Carry_name]
  COMPRESSOR_LIST.setdefault(Key, Value)
  
  COMPRESSOR_NO += 1
  
def generateCompressorTreeInNormalPattern():
  # generate module declaration
  VerilogFilePath = os.path.join(BASE_DIR, PROJECT_NAME + '.sv')
  VerilogFile = open(VerilogFilePath, 'a')

  VerilogFile.write('\nmodule ' + DOT_MATRIX_NAME + '(\n')
  for i in range(0, OPERAND_NUM):
    VerilogFile.write('\t(* altera_attribute = "-name VIRTUAL_PIN on" *) input wire[' + str(BITWIDTH - 1) + ':0] operand_%d,\n' %i)
  VerilogFile.write('\t(* altera_attribute = "-name VIRTUAL_PIN on" *) output wire[' + str(BITWIDTH - 1) + ':0] result\n')
  VerilogFile.write(');\n\n')
  VerilogFile.close()

  # get the corresponding matrix from the dot matrix file
  Matrix = getMatrixFromDotMatrixFile(DOT_MATRIX_NAME, OPERAND_NUM, BITWIDTH)
  
  # transport the matrix to prepare for the compressing process
  T_Matrix = transportMatrix(Matrix, OPERAND_NUM, BITWIDTH)
  
  printMatrixForDebug(T_Matrix)
  
  # eliminate the 1'b1 in T_Matrix
  T_Matrix = eliminateOneInTMatrix(T_Matrix)
  
  # compressor the T_Matrix
  compressTMatrix(T_Matrix, 1)
  
  # finish module
  VerilogFilePath = os.path.join(BASE_DIR, PROJECT_NAME + '.sv')
  VerilogFile = open(VerilogFilePath, 'a')
  VerilogFile.write('endmodule\n')
  VerilogFile.close()

def generateSignBitCompressorTree():
  # get the corresponding matrix from the dot matrix file
  Sign_bit_dot_matrix_name = DOT_MATRIX_NAME + '_signbit'
  Sign_matrix = getMatrixFromDotMatrixFile(Sign_bit_dot_matrix_name, SIGN_OPERAND_NUM, SIGN_BITWIDTH)
  
  # consider complement form of the sign matrix
  Complement_sign_matrix = Sign_matrix
  for row_no in range(0, len(Sign_matrix)):
    Row = Sign_matrix[row_no]
    
    for col_no in range(0, len(Row)):
      if (col_no != 0):
        Complement_sign_matrix[row_no][col_no] = '1\'b0'
      else:
        Complement_sign_matrix[row_no][col_no] = Sign_matrix[row_no][col_no]  
  
  # transport the matrix to prepare for the compressing process
  Complement_sign_t_matrix = transportMatrix(Complement_sign_matrix, SIGN_OPERAND_NUM, SIGN_BITWIDTH)
  
  printMatrixForDebug(Complement_sign_t_matrix)
  
  # compress the complement sign T_Matrix
  compressTMatrix(Complement_sign_t_matrix, 2)

def generateLeftBehindCompressorTree():
  # get the corresponding matrix from the dot matrix file
  Left_behind_dot_matrix_name = DOT_MATRIX_NAME + '_leftbehind'
  Left_behind_matrix = getMatrixFromDotMatrixFile(Left_behind_dot_matrix_name, OPERAND_NUM, BITWIDTH)
  
  # transport the matrix to prepare for the compressing process
  Left_behind_t_matrix = transportMatrix(Left_behind_matrix, OPERAND_NUM, BITWIDTH)
  
  printMatrixForDebug(Left_behind_t_matrix)
  
  # compress the complement sign T_Matrix
  compressTMatrix(Left_behind_t_matrix, 3)
  
def generateCompressorTreeInSignBitPattern():
  # generate module declaration
  VerilogFilePath = os.path.join(BASE_DIR, PROJECT_NAME + '.sv')
  VerilogFile = open(VerilogFilePath, 'a')

  VerilogFile.write('\nmodule ' + DOT_MATRIX_NAME + '(\n')
  for i in range(0, OPERAND_NUM):
    VerilogFile.write('\t(* altera_attribute = "-name VIRTUAL_PIN on" *) input wire[' + str(BITWIDTH - 1) + ':0] operand_%d,\n' %i)
  VerilogFile.write('\t(* altera_attribute = "-name VIRTUAL_PIN on" *) output wire[' + str(BITWIDTH - 1) + ':0] result\n')
  VerilogFile.write(');\n\n')
  VerilogFile.close()
  
  # generate compressor tree for sign bit dot matrix
  generateSignBitCompressorTree()
  
  # remember to change back to normal from complement form
  VerilogFilePath = os.path.join(BASE_DIR, PROJECT_NAME + '.sv')
  VerilogFile = open(VerilogFilePath, 'a')
  VerilogFile.write('wire[%d-1:0] sign_bit_result_before_padding = ~temp_sign_bit_result + 1\'b1;\n' %SIGN_BITWIDTH)
  VerilogFile.write('wire[%d-1:0] sign_bit_result' %BITWIDTH + ' = {sign_bit_result_before_padding, %d\'b0};\n\n' %(BITWIDTH - SIGN_BITWIDTH))
  
  VerilogFile.close()
  
  # generate compressor tree for left behind dot matrix
  generateLeftBehindCompressorTree()
  
  # add two compressor tree result
  VerilogFilePath = os.path.join(BASE_DIR, PROJECT_NAME + '.sv')
  VerilogFile = open(VerilogFilePath, 'a')
  VerilogFile.write('assign result = sign_bit_result + left_behind_result;\n\n')
  
  VerilogFile.write('endmodule\n')
  VerilogFile.close()  
  
def codegenForDotMatrix():
  global OPERAND_NUM
  global BITWIDTH
  global COMPRESSOR_LIST

  DotMatrixFilePath = os.path.join(BASE_DIR, DOT_MATRIX_NAME + '.dotmatrix')
  DotMatrixFile = open(DotMatrixFilePath, 'r')
  DotMatrix = DotMatrixFile.readlines()
  DotMatrixFile.close()
  
  DotMatrixInfo = parseDotMatrix(DotMatrix[0])
  
  assert DOT_MATRIX_NAME == DotMatrixInfo[0]
  OPERAND_NUM = int(DotMatrixInfo[1])
  BITWIDTH = int(DotMatrixInfo[2])
  
  # clear the compressor list
  COMPRESSOR_LIST.clear()
  
  # check if there are sign bit pattern
  SignBitPattern = parseSignBitPatternInDotMatrix()
  
  if (not SignBitPattern):
    generateCompressorTreeInNormalPattern()
  else:
    generateCompressorTreeInSignBitPattern()

def generateCompressor_3_2():
  VerilogFilePath = os.path.join(BASE_DIR, PROJECT_NAME + '.sv')
  VerilogFile = open(VerilogFilePath, 'a')

  VerilogFile.write('\nmodule compressor_3_2(\n' + \
                    '\tinput wire a,\n' + \
                    '\tinput wire b,\n' + \
                    '\tinput wire cin,\n' + \
                    '\toutput wire result,' + \
                    '\toutput wire cout\n' + \
                    ');\n\n' + \
                    '\tassign result = (a ^ b) ^ cin;\n' + \
                    '\tassign cout = (a & b) | (a & cin) | (b & cin);\n\n' + \
                    'endmodule\n')

  VerilogFile.close()
    
def codegen():
  global BASE_DIR
  global PROJECT_NAME
  global DOT_MATRIX_NAME
  
  Info_group = parseProjectInfo()
  PROJECT_NAME = Info_group[0]
  BASE_DIR = Info_group[1]
  
  parseDotMatrixs()
  
  for dot_matrix_name in DOT_MATRIXS_NAME_LIST:
    DOT_MATRIX_NAME = dot_matrix_name
    codegenForDotMatrix()
  
  generateCompressor_3_2()
  
#if __name__ == "__main__":
codegen()
