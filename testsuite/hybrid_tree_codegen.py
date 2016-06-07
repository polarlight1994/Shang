import re
import sys
import os
import inspect
import codecs

from copy import deepcopy
from operator import itemgetter

BASE_DIR = ''
PROJECT_NAME = ''
DOT_MATRIXS_NAME_LIST = []

COMPRESSOR_DELAY = 0.0855
ADD_32_DELAY = 0.3800
ADD_16_DELAY = 0.2501
ADD_DELAY = 0.0

DOT_MATRIX_NAME = ''
DOT_MATRIX_OPERAND_LIST = []
DOT_MATRIX_CONSTANT_LIST = []
DOT_MATRIX_FINAL_GROUP = []
DOT_MATRIX_ADD_CHAIN_GROUP = []
BITWIDTH = 0
OPERAND_NUM = 0
SIGN_BITWIDTH = 0
SIGN_OPERAND_NUM = 0
COMPRESSOR_LIST = dict()
COMPRESSOR_NO = 0
DEBUG_FILE_NO = 0

def isConstantInt(Matrix_row):
  IsConstantInt = True

  for i in range(0, len(Matrix_row)):
    Element = Matrix_row[i]
    
    if (Element != '1\'b0' and Element != '1\'b1'):
      IsConstantInt = False
      break
  
  return IsConstantInt

def parseProjectInfo():
  Info_group = []
  
  ProjectInfo = open('CompressorInfo.txt', 'r')
  InfoLines = ProjectInfo.readlines()
  ProjectInfo.close()

  Infos = InfoLines[0].split(',')
  Info_group.append(Infos[0])
  Info_group.append(Infos[1])

  return Info_group

def parseDelayStage(DelayStage):
  if (DelayStage == 'NULL'):
    return [9999.9999, 9999]

  DSInfo = []
  
  Info = DelayStage.split('-')
  DSInfo.append(float(Info[0]))
  DSInfo.append(int(Info[1]))
  
  return DSInfo

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
    DOT_MATRIXS_NAME_LIST.append(DotMatrixName)
    
  DSDotMatrixsFilePath = os.path.join(BASE_DIR, PROJECT_NAME + '_delay_stage.dotmatrixs')
  DSDotMatrixsFile = open(DSDotMatrixsFilePath, 'r')
  DSDotMatrixs = DSDotMatrixsFile.readlines()
  DSDotMatrixsFile.close()

  DSStartline_num = 0
  DSEndline_num = 0
  while(DSEndline_num <= len(DSDotMatrixs) - 2):
    DSHeadline = DSDotMatrixs[DSStartline_num]
    DSDotMatrixInfo = parseDotMatrix(DSHeadline)
    DSDotMatrixName = DSDotMatrixInfo[0]
    DSDotMatrixRowNum = DSDotMatrixInfo[1]
    DSDotMatrixColNum = DSDotMatrixInfo[2]
    DSEndline_num = DSStartline_num + DSDotMatrixRowNum
    
    DSDotMatrixFilePath = os.path.join(BASE_DIR, DSDotMatrixName + '_delay_stage.dotmatrix')
    DSDotMatrixFile = open(DSDotMatrixFilePath, 'w')
    
    for i in range(DSStartline_num, DSEndline_num + 1):
      DSDotMatrixFile.write(DSDotMatrixs[i])
    DSDotMatrixFile.close()

    DSStartline_num = DSEndline_num + 1

def getMatrixFromDotMatrixFile(FileName, Row_num, Col_num, IsDSMatrix):
  DotMatrixFilePath = os.path.join(BASE_DIR, FileName + '.dotmatrix')
  DotMatrixFile = open(DotMatrixFilePath, 'r')
  DotMatrix = DotMatrixFile.readlines()
  DotMatrixFile.close()
  
  # initialize a empty matrix
  Matrix = []
  if (IsDSMatrix):
    Matrix = [['NULL' for col in range(Col_num)] for row in range(Row_num)]
  else:
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
  
  assert len(Matrix) >= Operand_num
  for row_no in range(0, Operand_num):
    Row = Matrix[row_no]

    assert Operand_BitWdith == len(Row)
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
  
  return

def transportMatrix(Matrix, Row_num, Col_num, IsDSMatrix):
  T_Matrix = []
  if (IsDSMatrix):
    T_Matrix = [['NULL' for col in range(Row_num)] for row in range(Col_num)]
  else:
    T_Matrix = [['1\'b0' for col in range(Row_num)] for row in range(Col_num)]
  
  for row_no in range(len(Matrix)):
    Row = Matrix[row_no]
    
    for col_no in range(len(Row)):
      T_Matrix[col_no][row_no] = Row[col_no]
      
  return T_Matrix

def eliminateOneInTMatrix(T_Matrix, T_DSMatrix):
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
  New_t_dsmatrix = []  
  for row_no in range(0, len(T_Matrix)):
    Row = T_Matrix[row_no]
    DSRow = T_DSMatrix[row_no]
    
    New_row = ['1\'b0']
    New_dsrow = ['NULL']

    for col_no in range(0, len(Row)):
      New_row.append('1\'b0')
      New_dsrow.append('NULL')

    assert (len(Row) + 1) == len(New_row)
    New_t_matrix.append(New_row)
    New_t_dsmatrix.append(New_dsrow)

  assert len(T_Matrix) == len(New_t_matrix)
  
  for row_no in range(0, len(T_Matrix)):
    Row = T_Matrix[row_no]
    DSRow = T_DSMatrix[row_no]
    
    New_col_no = 0
    
    # insert the final 1'b1 in the front of row
    Final_one_no = Final_one_number_in_t_matrix[row_no]
    if (Final_one_no != 0):
      New_t_matrix[row_no][0] = '1\'b1'
      New_t_dsmatrix[row_no][0] = '0.0-0'
    else:
      New_t_matrix[row_no][0] = '1\'b0'
      New_t_dsmatrix[row_no][0] = '0.0-0'
      
    New_col_no += 1

    for col_no in range(0, len(Row)):
      if (T_Matrix[row_no][col_no] == '1\'b1'):
        New_t_matrix[row_no][New_col_no] = '1\'b0'
      else:
        if (New_col_no != len(New_t_matrix[row_no]) - 1):
          New_t_matrix[row_no][New_col_no] = T_Matrix[row_no][col_no]
          New_t_dsmatrix[row_no][New_col_no] = T_DSMatrix[row_no][col_no]
        else:
          New_t_matrix[row_no].append(T_Matrix[row_no][col_no])
          New_t_dsmatrix[row_no].append(T_DSMatrix[row_no][col_no])
      
      New_col_no += 1      
        
  printMatrixForDebug(New_t_matrix)
  printMatrixForDebug(New_t_dsmatrix)
  
  return [New_t_matrix, New_t_dsmatrix]

def sortTMatrixInOrderOfDelayStage(T_Matrix, T_DSMatrix):
  # initialize a new matrix to contain both element and its info
  Matrix = []
  for row_no in range(0, len(T_Matrix)):
    Matrix_row = []
    T_Matrix_row = T_Matrix[row_no]
    T_DSMatrix_row = T_DSMatrix[row_no]
    
    for col_no in range(0, len(T_Matrix_row)):
      T_Matrix_element = T_Matrix_row[col_no]
      T_DSMatrix_element_0 = parseDelayStage(T_DSMatrix_row[col_no])[0]
      T_DSMatrix_element_1 = parseDelayStage(T_DSMatrix_row[col_no])[1]
      
      Matrix_element = [T_Matrix_element, T_DSMatrix_element_0, T_DSMatrix_element_1]
      
      Matrix_row.append(Matrix_element)
      
    Matrix.append(Matrix_row)
    
  # sort the new matrix in order of #1:stage #2:delay
  Sorted_matrix = []
  for row_no in range(0, len(Matrix)):
    Sorted_row = sorted(Matrix[row_no], key = itemgetter(2,1))
    Sorted_matrix.append(Sorted_row)
    
  # rebuild the t_matrix and t_dsmatrix according to the sort result
  New_t_matrix = []
  New_t_dsmatrix = []
  for row_no in range(0, len(Sorted_matrix)):
    New_t_matrix_row = []
    New_t_dsmatrix_row = []
    Sorted_matrix_row = Sorted_matrix[row_no]
    
    for col_no in range(0, len(Sorted_matrix_row)):
      Sorted_matrix_element = Sorted_matrix_row[col_no]
      
      New_t_matrix_element = Sorted_matrix_element[0]
      New_t_dsmatrix_element = str(Sorted_matrix_element[1]) + '-' + str(Sorted_matrix_element[2])
      if (Sorted_matrix_element[1] == 9999.9999 and Sorted_matrix_element[2] == 9999):
        New_t_dsmatrix_element = 'NULL'      
      
      New_t_matrix_row.append(New_t_matrix_element)
      New_t_dsmatrix_row.append(New_t_dsmatrix_element)
      
    New_t_matrix.append(New_t_matrix_row)
    New_t_dsmatrix.append(New_t_dsmatrix_row)
    
  return [New_t_matrix, New_t_dsmatrix]
  
def sortTMatrix(T_Matrix, T_DSMatrix):
  New_t_matrix = []
  New_t_dsmatrix = []

  for row_no in range(0, len(T_Matrix)):
    New_row = []
    New_dsrow = []
    Row = T_Matrix[row_no]
    DSRow = T_DSMatrix[row_no]
  
    for col_no in range(0, len(Row)):
      if (Row[col_no] != '1\'b0'):
        New_row.append(T_Matrix[row_no][col_no])
        New_dsrow.append(T_DSMatrix[row_no][col_no])
        
    New_t_matrix.append(New_row)
    New_t_dsmatrix.append(New_dsrow)

  printMatrixForDebug(New_t_matrix)
  printMatrixForDebug(New_t_dsmatrix)
  
  return sortTMatrixInOrderOfDelayStage(New_t_matrix, New_t_dsmatrix)

def getNumOfElementsToBeCompressed(T_Matrix):
  NumOfElements = [0 for row in range(BITWIDTH)]
  
  for row_no in range(0,len(T_Matrix)):
    Row = T_Matrix[row_no]
    Element_no = 0

    for col_no in range(0, len(Row)):
      if (T_Matrix[row_no][col_no] != '1\'b0'):
        Element_no += 1

    NumOfElements[row_no] = Element_no

  return NumOfElements

def getNumofElementsCanBeCompressed(T_DSMatrix):
  NumOfElements = [0 for row in range(BITWIDTH)]
  
  for row_no in range(0, len(T_DSMatrix)):
    Row = T_DSMatrix[row_no]
    
    if (len(Row) == 0):
      NumOfElements[row_no] = 0
      continue

    CurrentStage = parseDelayStage(Row[0])[1]
    Element_no = 0

    for col_no in range(0, len(Row)):
      if (parseDelayStage(Row[col_no])[1] == CurrentStage):
        Element_no += 1

    NumOfElements[row_no] = Element_no

  return NumOfElements
  
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

def predictCompressStage(Operand_num):
  Stage = 0
  
  while(Operand_num > 2):
    Compressed_Operand_num = (Operand_num // 3) * 3
    LeftBehind_Operand_num = Operand_num % 3
  
    Result_Operand_num = Compressed_Operand_num * 2 // 3 + LeftBehind_Operand_num
    Operand_num = Result_Operand_num
    
    Stage += 1
  
  return Stage
  
def compressFirstTripleColumns(T_Matrix, T_DSMatrix, Stage, CompressPattern):
  NumOfElements = getNumOfElementsToBeCompressed(T_Matrix)
  NumOfReadyElements = getNumofElementsCanBeCompressed(T_DSMatrix)

  for row_no in range(0, len(T_Matrix) - 1):
    if (NumOfElements[row_no] >= 3):
      assert NumOfReadyElements[row_no] != 0    
      Num = NumOfReadyElements[row_no] // 3
      
      for no in range(0, Num):
        Triple_elements = [T_Matrix[row_no][3*no], T_Matrix[row_no][3*no+1], T_Matrix[row_no][3*no+2]]

        T_Matrix[row_no][3*no] = '1\'b0'
        T_Matrix[row_no][3*no+1] = '1\'b0'
        T_Matrix[row_no][3*no+2] = '1\'b0'
        
        Delay_1 = parseDelayStage(T_DSMatrix[row_no][3*no])[0]
        Delay_2 = parseDelayStage(T_DSMatrix[row_no][3*no+1])[0]
        Delay_3 = parseDelayStage(T_DSMatrix[row_no][3*no+2])[0]
        
        Stage_1 = parseDelayStage(T_DSMatrix[row_no][3*no])[1]
        Stage_2 = parseDelayStage(T_DSMatrix[row_no][3*no+1])[1]
        Stage_3 = parseDelayStage(T_DSMatrix[row_no][3*no+2])[1]
        
        assert Stage_1 == Stage_2 and Stage_1 == Stage_3
        
        ElementStage = Stage_1
        
        InputDelay = max(Delay_1, Delay_2, Delay_3)
        OutputDelay = InputDelay + COMPRESSOR_DELAY
        
        T_DSMatrix[row_no][3*no] = 'NULL'
        T_DSMatrix[row_no][3*no+1] = 'NULL'
        T_DSMatrix[row_no][3*no+2] = 'NULL'

        Sum_name = 'NULL'
        Carry_name = 'NULL'

        IdenticalCompressor = checkIdenticalCompressor(Triple_elements)
        if (IdenticalCompressor != None):
          Sum_name = IdenticalCompressor[0]
          Carry_name = IdenticalCompressor[1]
        else:
          if (CompressPattern == 0):
            Sum_name = 'final_group_sum_' + str(row_no) + '_' + str(no) + '_' + str(Stage)
            Carry_name = 'final_group_carry_' + str(row_no) + '_' + str(no) + '_' + str(Stage)
            Compressor_generator(Triple_elements, Sum_name, Carry_name)
          elif (CompressPattern == 1):
            Sum_name = 'pred_group_sum_' + str(row_no) + '_' + str(no) + '_' + str(Stage)
            Carry_name = 'pred_group_carry_' + str(row_no) + '_' + str(no) + '_' + str(Stage)
            Compressor_generator(Triple_elements, Sum_name, Carry_name)

        T_Matrix[row_no].append(Sum_name)
        T_DSMatrix[row_no].append('%f' %OutputDelay + '-%d' %(ElementStage + 1))
        if (row_no <= BITWIDTH - 2):
          T_Matrix[row_no+1].append(Carry_name)
          T_DSMatrix[row_no+1].append('%f' %OutputDelay + '-%d' %(ElementStage + 1))
      
      # increase the stage of all left behind elements so that they can be compressed ASAP in next stage      
      for no in range(Num * 3, NumOfReadyElements[row_no]):
        OriginDelay = parseDelayStage(T_DSMatrix[row_no][no])[0]
        OriginStage = parseDelayStage(T_DSMatrix[row_no][no])[1]
        
        T_DSMatrix[row_no][no] = str(OriginDelay) + '-' + str(OriginStage + 1)

  if (NumOfElements[BITWIDTH-1] >= 2):
    VerilogFilePath = os.path.join(BASE_DIR, PROJECT_NAME + '.sv')
    VerilogFile = open(VerilogFilePath, 'a')
    if (CompressPattern == 0):
      VerilogFile.write('wire final_group_result_MSB_%d' %Stage + ' = ' + T_Matrix[BITWIDTH-1][0])
    elif (CompressPattern == 1):
      VerilogFile.write('wire pred_group_result_MSB_%d' %Stage + ' = ' + T_Matrix[BITWIDTH-1][0])
    
    T_Matrix[BITWIDTH-1][0] = '1\'b0'
    T_DSMatrix[BITWIDTH-1][0] = 'NULL'
    for no in range(1, NumOfElements[BITWIDTH-1]):
      VerilogFile.write(' ^ ' + T_Matrix[BITWIDTH-1][no])
      T_Matrix[BITWIDTH-1][no] = '1\'b0'
      T_DSMatrix[BITWIDTH-1][no] = 'NULL'
    VerilogFile.write(';\n\n')
    if (CompressPattern == 0):
      T_Matrix[BITWIDTH-1].append('final_group_result_MSB_%d' %Stage)
      # actually we do not care the output delay of the MSB since it is not caculate by compressor
      T_DSMatrix[BITWIDTH-1].append('0.0-%d' %(Stage + 1))
    elif (CompressPattern == 1):
      T_Matrix[BITWIDTH-1].append('pred_group_result_MSB_%d' %Stage)
      # actually we do not care the output delay of the MSB since it is not caculate by compressor
      T_DSMatrix[BITWIDTH-1].append('0.0-%d' %(Stage + 1))

    VerilogFile.close()

  SortResult = sortTMatrix(T_Matrix, T_DSMatrix)
  T_Matrix = SortResult[0]
  T_DSMatrix = SortResult[1]
  
  return [T_Matrix, T_DSMatrix]
 
def compressTMatrix(T_Matrix, T_DSMatrix, CompressPattern):
  # sort the T_Matrix
  SortResult = sortTMatrix(T_Matrix, T_DSMatrix)
  T_Matrix = SortResult[0]
  T_DSMatrix = SortResult[1]
  
  printMatrixForDebug(T_Matrix)
  printMatrixForDebug(T_DSMatrix)
  
  # start to compress the T_Matrix
  Need_to_compress = True
  Stage = 0
  while(Need_to_compress):
    # compress the first three columns in T_Matrix
    CompressResult = compressFirstTripleColumns(T_Matrix, T_DSMatrix, Stage, CompressPattern)
    T_Matrix = CompressResult[0]
    T_DSMatrix = CompressResult[1]
    
    printMatrixForDebug(T_Matrix)
    printMatrixForDebug(T_DSMatrix)

    # get the compress status to decide when to stop compressing
    NumOfElements = getNumOfElementsToBeCompressed(T_Matrix)
    Need_to_compress = False
    for row_no in range(0,len(T_Matrix)):
      if (NumOfElements[row_no] >= 3):
        Need_to_compress = True

    Stage += 1
    
    # sort the matrix to prepare for next compressing
    SortResult = sortTMatrix(T_Matrix, T_DSMatrix)
    T_Matrix = SortResult[0]
    T_DSMatrix = SortResult[1]
  
  # finish compressing process by padding needed bits to prepare for final adder
  for row_no in range(0, len(T_Matrix)):
    Row = T_Matrix[row_no]
    Row_length = len(Row)
    
    while(Row_length < 2):
      T_Matrix[row_no].append('1\'b0')
      Row_length += 1
  
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
  
  if (CompressPattern == 0):
    VerilogFile.write('wire[%d-2:0] final_group_dataa = {' %BITWIDTH)
  elif (CompressPattern == 1):
    VerilogFile.write('wire[%d-2:0] pred_group_dataa = {' %BITWIDTH)

  VerilogFile.write(str(Dataa[BITWIDTH - 2]))
  for row_no in range(0, len(Dataa) - 1):
    VerilogFile.write(', ' + str(Dataa[len(Dataa) - 2 - row_no]))
  VerilogFile.write('};\n')

  if (CompressPattern == 0):
    VerilogFile.write('wire[%d-2:0] final_group_datab = {' %BITWIDTH)
  elif (CompressPattern == 1):
    VerilogFile.write('wire[%d-2:0] pred_group_datab = {' %BITWIDTH)

  VerilogFile.write(str(Datab[BITWIDTH - 2]))
  for row_no in range(0, len(Datab) - 1):
    VerilogFile.write(', ' + str(Datab[len(Datab) - 2 - row_no]))
  VerilogFile.write('};\n\n')
  
  if (CompressPattern == 0):
    VerilogFile.write('wire[%d-1:0] ' %BITWIDTH + 'final_group_add_result = final_group_dataa + final_group_datab;\n')
    VerilogFile.write('wire[%d-1:0] final_group_result;\n' %BITWIDTH)
    VerilogFile.write('assign final_group_result[%d-1] = ' %BITWIDTH + T_Matrix[BITWIDTH - 1][0])
  elif (CompressPattern == 1):
    VerilogFile.write('wire[%d-1:0] ' %BITWIDTH + 'pred_group_add_result = pred_group_dataa + pred_group_datab;\n')
    VerilogFile.write('wire[%d-1:0] pred_group_result;\n' %BITWIDTH)
    VerilogFile.write('assign pred_group_result[%d-1] = ' %BITWIDTH + T_Matrix[BITWIDTH - 1][0])
  
  for i in range(1, len(T_Matrix[BITWIDTH - 1])):
    MSB = T_Matrix[BITWIDTH - 1][i]
    if (MSB != '1\'b0'):
      VerilogFile.write(' ^ ' + MSB)
  
  if (CompressPattern == 0):
    VerilogFile.write(' ^ final_group_add_result[%d];\n' %(BITWIDTH - 1))
    VerilogFile.write('assign final_group_result[%d-2:0] = ' %BITWIDTH + 'final_group_add_result[%d:0]' %(BITWIDTH - 2) + ';\n\n')
    VerilogFile.write('assign result[%d-1:0] = ' %BITWIDTH + 'final_group_result[%d-1:0]' %BITWIDTH + ';\n\n')
  elif (CompressPattern == 1):
    VerilogFile.write(' ^ pred_group_add_result[%d];\n' %(BITWIDTH - 1))
    VerilogFile.write('assign pred_group_result[%d-2:0] = ' %BITWIDTH + 'pred_group_add_result[%d:0]' %(BITWIDTH - 2) + ';\n\n')
  
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
  ###### generate add chain for the AddChainGroup
  VerilogFilePath = os.path.join(BASE_DIR, PROJECT_NAME + '.sv')
  VerilogFile = open(VerilogFilePath, 'a')
  
  for i in range(0, len(DOT_MATRIX_ADD_CHAIN_GROUP)):
    Add_chain_operands = DOT_MATRIX_ADD_CHAIN_GROUP[i][0]
    Add_chain_result = DOT_MATRIX_ADD_CHAIN_GROUP[i][1]
    
    Add_chain_matrix = getMatrixFromDotMatrixFile(DOT_MATRIX_NAME + '_for_add_chain_%d' %i, len(Add_chain_operands), BITWIDTH, False)
    for j in range(0, len(Add_chain_matrix)):
      VerilogFile.write('wire[%d-1:0] ' %BITWIDTH + 'add_chain_%d' %i + '_%d = {' %j)
      
      Add_chain_operand_bits = Add_chain_matrix[j]
      for k in range(0, len(Add_chain_operand_bits)):        
        VerilogFile.write(Add_chain_operand_bits[len(Add_chain_operand_bits) - 1 - k])
        
        if (k != len(Add_chain_operand_bits) - 1):
          VerilogFile.write(', ')
        else:
          VerilogFile.write('};\n')
    
    VerilogFile.write('wire[%d-1:0] ' %BITWIDTH + Add_chain_result + ' = ')
    for j in range(0, len(Add_chain_operands)):
      Add_chain_operand = Add_chain_operands[j]
      
      VerilogFile.write('add_chain_%d' %i + '_%d' %j)
      if (j != len(Add_chain_operands) - 1):
        VerilogFile.write(' + ')
      else:
        VerilogFile.write(';\n\n')
  VerilogFile.close()  
  
  ###### generate compressors for the FinalCompressorGroup
  # get the corresponding matrix for FinalCompressor from the dot matrix file
  FinalCompressor_Matrix = getMatrixFromDotMatrixFile(DOT_MATRIX_NAME + '_for_finalcompressor', len(DOT_MATRIX_FINAL_GROUP), BITWIDTH, False)
  # also get the corresponding delay_stage matrix from the dot matrix file
  FinalCompressor_DSMatrix = getMatrixFromDotMatrixFile(DOT_MATRIX_NAME + '_delay_stage_for_finalcompressor', len(DOT_MATRIX_FINAL_GROUP), BITWIDTH, True)
  
  # transport the matrix to prepare for the compressing process
  FinalCompressor_T_Matrix = transportMatrix(FinalCompressor_Matrix, len(DOT_MATRIX_FINAL_GROUP), BITWIDTH, False)
  FinalCompressor_T_DSMatrix = transportMatrix(FinalCompressor_DSMatrix, len(DOT_MATRIX_FINAL_GROUP), BITWIDTH, True)
  
  printMatrixForDebug(FinalCompressor_T_Matrix)
  printMatrixForDebug(FinalCompressor_T_DSMatrix)
  
  # eliminate the 1'b1 in T_Matrix
  FinalCompressor_eliminateOneResult = eliminateOneInTMatrix(FinalCompressor_T_Matrix, FinalCompressor_T_DSMatrix)
  FinalCompressor_T_Matrix = FinalCompressor_eliminateOneResult[0]
  FinalCompressor_T_DSMatrix = FinalCompressor_eliminateOneResult[1]
  
  # compressor the T_Matrix
  compressTMatrix(FinalCompressor_T_Matrix, FinalCompressor_T_DSMatrix, 0)
  
  # finish module
  VerilogFilePath = os.path.join(BASE_DIR, PROJECT_NAME + '.sv')
  VerilogFile = open(VerilogFilePath, 'a')
  VerilogFile.write('endmodule\n')
  VerilogFile.close()

def getAddChainResultTime(List):
  List = sortOperands(List)
  
  if (len(List) == 1):
    return List[0][1]
  
  if (len(List) == 2):
    return max(List[0][1], List[1][1]) + ADD_DELAY
  
  if (len(List) % 2 == 1):    
    return (max(getAddChainResultTime(List[0:-1]), List[len(List) - 1][1]) + ADD_DELAY)
  else:
    Base_delay = getAddChainResultTime(List[0:-2])
    
    # Base + op_0 + op_1
    Max_delay_0 = max(max(Base_delay, List[len(List) - 2][1]) + ADD_DELAY, List[len(List) - 1][1]) + ADD_DELAY
    # Base + (op_0 + op_1)
    Max_delay_1 = max(Base_delay, max(List[len(List) - 2][1], List[len(List) - 1][1]) + ADD_DELAY) + ADD_DELAY
    
    return min(Max_delay_0, Max_delay_1)

def createOperand(Name, BitWidth, Delay, Stage):
  Operand_for_matrix = []
  Operand_for_dsmatrix = []
  for i in range(0, BitWidth):
    Operand_for_matrix.append(Name + '[%d]' %i)
    Operand_for_dsmatrix.append(str(Delay) + '-' + str(Stage))

  return [Operand_for_matrix, Operand_for_dsmatrix]
  
def sortOperands(List):  
  return sorted(List, key = itemgetter(1))
 
def eraseOperands(List, Erase_list):  
  New_list = []
  for i in range(0, len(List)):
    if (not(i in Erase_list)):
      New_list.append(List[i])
      
  return sorted(New_list, key = itemgetter(1))

def insertOperand(List, Operand):
  List.append(Operand)
  return sorted(List, key = itemgetter(1))
  
def loadBalance(FastOne, SlotOne, Final_group_start_time):
  FastOne_time = FastOne[1]
  SlotOne_time = SlotOne[1]
  TimeDifference = abs(float(SlotOne_time) - float(FastOne_time))
  
  FaseOneList = FastOne[0][0]
  SlotOneList = SlotOne[0][0]
  
  # balance tragedy No.1
  PotentialBalancedFastOneList_0 = FaseOneList[0:] + [SlotOneList[len(SlotOneList) - 1]]
  PotentialBalancedSlotOneList_0 = SlotOneList[0:-1]
  Potential_0_correctness = False
  if (getAddChainResultTime(PotentialBalancedFastOneList_0) < Final_group_start_time):
    Potential_0_correctness = True
  
  # balance tragedy No.2
  PotentialBalancedFastOneList_1 = FaseOneList[0:] + [SlotOneList[0]]
  PotentialBalancedSlotOneList_1 = SlotOneList[1:]
  Potential_1_correctness = False
  if (getAddChainResultTime(PotentialBalancedFastOneList_1) < Final_group_start_time):
    Potential_1_correctness = True
  
  if (Potential_0_correctness and Potential_1_correctness):
    PotentialBalancedSlotOneListResultTime_0 = getAddChainResultTime(PotentialBalancedSlotOneList_0)
    PotentialBalancedFastOneListResultTime_0 = getAddChainResultTime(PotentialBalancedFastOneList_0)
    
    PotentialBalancedSlotOneListResultTime_1 = getAddChainResultTime(PotentialBalancedSlotOneList_1)
    PotentialBalancedFastOneListResultTime_1 = getAddChainResultTime(PotentialBalancedFastOneList_1)
    
    PotentialTimeDifference_0 = abs(PotentialBalancedSlotOneListResultTime_0 - PotentialBalancedFastOneListResultTime_0)
    PotentialTimeDifference_1 = abs(PotentialBalancedSlotOneListResultTime_1 - PotentialBalancedFastOneListResultTime_1)
    
    if (PotentialTimeDifference_0 < TimeDifference and PotentialTimeDifference_1 < TimeDifference):
      if (PotentialTimeDifference_0 <= PotentialTimeDifference_1):
        return [[[PotentialBalancedFastOneList_0, FastOne[0][1]], PotentialBalancedFastOneListResultTime_0], [[PotentialBalancedSlotOneList_0, SlotOne[0][1]], PotentialBalancedSlotOneListResultTime_0]]
      else:
        return [[[PotentialBalancedFastOneList_1, FastOne[0][1]], PotentialBalancedFastOneListResultTime_1], [[PotentialBalancedSlotOneList_1, SlotOne[0][1]], PotentialBalancedSlotOneListResultTime_1]]
    elif (PotentialTimeDifference_0 < TimeDifference):
      return [[[PotentialBalancedFastOneList_0, FastOne[0][1]], PotentialBalancedFastOneListResultTime_0], [[PotentialBalancedSlotOneList_0, SlotOne[0][1]], PotentialBalancedSlotOneListResultTime_0]]
    elif (PotentialTimeDifference_1 < TimeDifference):
      return [[[PotentialBalancedFastOneList_1, FastOne[0][1]], PotentialBalancedFastOneListResultTime_1], [[PotentialBalancedSlotOneList_1, SlotOne[0][1]], PotentialBalancedSlotOneListResultTime_1]]
    else:
      return [FastOne, SlotOne]
      
  elif (Potential_0_correctness):
    PotentialTimeDifference_0 = abs(getAddChainResultTime(PotentialBalancedSlotOneList_0) - getAddChainResultTime(PotentialBalancedFastOneList_0))
    
    if (PotentialTimeDifference_0 < TimeDifference):
      return [[[PotentialBalancedFastOneList_0, FastOne[0][1]], PotentialBalancedFastOneListResultTime_0], [[PotentialBalancedSlotOneList_0, SlotOne[0][1]], PotentialBalancedSlotOneListResultTime_0]]
    else:
      return [FastOne, SlotOne]
      
  elif (Potential_1_correctness):
    PotentialTimeDifference_1 = abs(getAddChainResultTime(PotentialBalancedSlotOneList_1) - getAddChainResultTime(PotentialBalancedFastOneList_1))
    
    if (PotentialTimeDifference_1 < TimeDifference):
      return [[[PotentialBalancedFastOneList_1, FastOne[0][1]], PotentialBalancedFastOneListResultTime_1], [[PotentialBalancedSlotOneList_1, SlotOne[0][1]], PotentialBalancedSlotOneListResultTime_1]]
    else:
      return [FastOne, SlotOne]
      
  else:
    return [FastOne, SlotOne]  

def balanceAddChainPair(Add_chain_with_result_fast, Add_chain_with_result_slow, Final_group_start_time):
  LoadResult = []
  
  Continue = True
  while(Continue):
    Continue = False
    LoadResult = loadBalance(Add_chain_with_result_fast, Add_chain_with_result_slow, Final_group_start_time)
    
    #print('Result is ' + str(LoadResult[0]) + '-' + str(LoadResult[1]))
    #print('Origin is ' + str(Add_chain_with_result_fast) + '-' + str(Add_chain_with_result_slow))
    
    if (LoadResult[0] != Add_chain_with_result_fast or LoadResult[1] != Add_chain_with_result_slow):
      Continue = True
      Add_chain_with_result_fast = LoadResult[0]
      Add_chain_with_result_slow = LoadResult[1]

  return LoadResult
    
def balanceAddChain(Final_group_start_time):
  global DOT_MATRIX_ADD_CHAIN_GROUP

  Add_chain_group_with_time = []
  for i in range(0, len(DOT_MATRIX_ADD_CHAIN_GROUP)):
    Add_chain_with_result = DOT_MATRIX_ADD_CHAIN_GROUP[i]    
    Add_chain_result_time = getAddChainResultTime(Add_chain_with_result[0])
    Add_chain_group_with_time.append([Add_chain_with_result, Add_chain_result_time])
  
  Add_chain_group_with_time = sortOperands(Add_chain_group_with_time)
  
  Balanced_add_chain_group_with_time = []
  for i in range(0, len(Add_chain_group_with_time) // 2):
    Add_chain_with_result_fast = Add_chain_group_with_time[i]
    Add_chain_with_result_slow = Add_chain_group_with_time[len(Add_chain_group_with_time) - 1 - i]
    
    Balanced_result = balanceAddChainPair(Add_chain_with_result_fast, Add_chain_with_result_slow, Final_group_start_time)
    Balanced_add_chain_group_with_time.append(Balanced_result[0])
    Balanced_add_chain_group_with_time.append(Balanced_result[1])
  
  #print('After balanced the add chain group is ' + str(Balanced_add_chain_group_with_time))
  
  # re-generate the add chain group
  DOT_MATRIX_ADD_CHAIN_GROUP = []
  
  for i in range(0, len(Balanced_add_chain_group_with_time)):
    DOT_MATRIX_ADD_CHAIN_GROUP.append(Balanced_add_chain_group_with_time[i][0])
    
  if (len(Add_chain_group_with_time) % 2 == 1):
    DOT_MATRIX_ADD_CHAIN_GROUP.append(Add_chain_group_with_time[(len(Add_chain_group_with_time) // 2)][0])

def buildAddTree():
  global DOT_MATRIX_OPERAND_LIST
  global DOT_MATRIX_ADD_CHAIN_GROUP

  # get the start time of last group
  Final_group_start_time = DOT_MATRIX_FINAL_GROUP[len(DOT_MATRIX_FINAL_GROUP) - 1][1]

  # visit the operand_validtime_list to build add chain as many as possible
  # in constraint of finish_time mentioned above
  Add_chain_result_list = []
  Continue = True
  while(Continue):
    Continue = False
  
    Add_chain = []
    
    Erase_list = []
    for i in range(0, len(DOT_MATRIX_OPERAND_LIST)):
      Potential_add_chain = deepcopy(Add_chain)
  
      Operand_Validtime = DOT_MATRIX_OPERAND_LIST[i]
      Potential_add_chain.append(Operand_Validtime)
    
      Result_time = getAddChainResultTime(Potential_add_chain)
      
      # if meet the constraint, push the operand into add chain list officially
      if (Result_time <= Final_group_start_time):
        Add_chain = Potential_add_chain
        Erase_list.append(i)
        continue     
      else:
        break
    
    if (len(Add_chain) > 1):
      DOT_MATRIX_OPERAND_LIST = eraseOperands(DOT_MATRIX_OPERAND_LIST, Erase_list)

      Add_chain_result_name = 'add_chain_result_%d' %(len(DOT_MATRIX_ADD_CHAIN_GROUP))
      Add_chain_result_list.append([Add_chain_result_name, getAddChainResultTime(Add_chain)])
      DOT_MATRIX_ADD_CHAIN_GROUP.append([Add_chain, Add_chain_result_name])
      
      Continue = True
  
  for i in range(0, len(Add_chain_result_list)):
    DOT_MATRIX_OPERAND_LIST = insertOperand(DOT_MATRIX_OPERAND_LIST, Add_chain_result_list[i])
    
  balanceAddChain(Final_group_start_time)

def groupPartition():
  global DOT_MATRIX_OPERAND_LIST
  global DOT_MATRIX_FINAL_GROUP

  # sort the operands
  DOT_MATRIX_OPERAND_LIST = sortOperands(DOT_MATRIX_OPERAND_LIST)
  
  # the last operand is to be compressed definitly
  Erase_list = [len(DOT_MATRIX_OPERAND_LIST) - 1]
  DOT_MATRIX_FINAL_GROUP.append(DOT_MATRIX_OPERAND_LIST[len(DOT_MATRIX_OPERAND_LIST) - 1])
  
  # also the constants will be compressed
  for i in range(0, len(DOT_MATRIX_CONSTANT_LIST)):
    DOT_MATRIX_FINAL_GROUP.append(DOT_MATRIX_CONSTANT_LIST[i])
  
  DOT_MATRIX_OPERAND_LIST = eraseOperands(DOT_MATRIX_OPERAND_LIST, Erase_list)
  
  DOT_MATRIX_FINAL_GROUP = sortOperands(DOT_MATRIX_FINAL_GROUP)
  DOT_MATRIX_OPERAND_LIST = sortOperands(DOT_MATRIX_OPERAND_LIST)

  print('The operand list is ' + str(DOT_MATRIX_OPERAND_LIST))
  
def buildHybridTree():
  global DOT_MATRIX_FINAL_GROUP
  
  # extract the final group to be compressed
  groupPartition()
  
  # extract the operands to be added in add tree
  buildAddTree()
  
  # operands left behind and the result of add tree
  # will be compressed in pred group
  for i in range(0, len(DOT_MATRIX_OPERAND_LIST)):
    DOT_MATRIX_FINAL_GROUP.append(DOT_MATRIX_OPERAND_LIST[i])

  Matrix = getMatrixFromDotMatrixFile(DOT_MATRIX_NAME + '_opt', OPERAND_NUM, BITWIDTH, False)
  DSMatrix = getMatrixFromDotMatrixFile(DOT_MATRIX_NAME + '_opt_delay_stage', OPERAND_NUM, BITWIDTH, True)
  
  FinalCompressor_Matrix = []
  FinalCompressor_DSMatrix = []
  for i in range(0, len(DOT_MATRIX_FINAL_GROUP)):
    Operand_no = DOT_MATRIX_FINAL_GROUP[i][0]
    
    if (isinstance(Operand_no, int)):
      FinalCompressor_Matrix.append(Matrix[Operand_no])
      FinalCompressor_DSMatrix.append(DSMatrix[Operand_no])
    else:
      Operand_created = createOperand(Operand_no, BITWIDTH, DOT_MATRIX_FINAL_GROUP[i][1], 0)
      FinalCompressor_Matrix.append(Operand_created[0])
      FinalCompressor_DSMatrix.append(Operand_created[1])
    
  printDotMatrixForMatrix(FinalCompressor_Matrix, DOT_MATRIX_NAME + '_for_finalcompressor', len(DOT_MATRIX_FINAL_GROUP), BITWIDTH)
  printDotMatrixForMatrix(FinalCompressor_DSMatrix, DOT_MATRIX_NAME + '_delay_stage_for_finalcompressor', len(DOT_MATRIX_FINAL_GROUP), BITWIDTH)
  
  for i in range(0, len(DOT_MATRIX_ADD_CHAIN_GROUP)):
    Add_chain = DOT_MATRIX_ADD_CHAIN_GROUP[i][0]
    AddChain_Matrix = []
    
    for j in range(0, len(Add_chain)):
      Operand_no = Add_chain[j][0]
      AddChain_Matrix.append(Matrix[Operand_no])
      
    printDotMatrixForMatrix(AddChain_Matrix, DOT_MATRIX_NAME + '_for_add_chain_%d' %i, len(Add_chain), BITWIDTH)

  print('The add chain list is ' + str(DOT_MATRIX_ADD_CHAIN_GROUP))
  print('The compressor group is ' + str(DOT_MATRIX_FINAL_GROUP))
  
def operandsPartition():
  global DOT_MATRIX_OPERAND_LIST
  global DOT_MATRIX_CONSTANT_LIST

  # extract the delay info from the matrix and delay stage matrix
  Matrix = getMatrixFromDotMatrixFile(DOT_MATRIX_NAME + '_opt', OPERAND_NUM, BITWIDTH, False)
  DSMatrix = getMatrixFromDotMatrixFile(DOT_MATRIX_NAME + '_opt_delay_stage', OPERAND_NUM, BITWIDTH, True)
  
  for i in range(0, len(DSMatrix)):
    Matrix_row = Matrix[i]
    DSMatrix_row = DSMatrix[i]
    
    Row_delay = 0.0
    for j in range(0, len(DSMatrix_row)):
      Row_delay = max(Row_delay, parseDelayStage(DSMatrix_row[j])[0])
      
      assert Row_delay != 9999.9999
    
    if (isConstantInt(Matrix_row)):
      DOT_MATRIX_CONSTANT_LIST.append([i, Row_delay])
    else:
      DOT_MATRIX_OPERAND_LIST.append([i, Row_delay])
  
def codegenForDotMatrix():
  global OPERAND_NUM
  global BITWIDTH
  global ADD_DELAY
  global COMPRESSOR_LIST
  global DOT_MATRIX_OPERAND_LIST
  global DOT_MATRIX_CONSTANT_LIST
  global DOT_MATRIX_FINAL_GROUP
  global DOT_MATRIX_ADD_CHAIN_GROUP

  DotMatrixFilePath = os.path.join(BASE_DIR, DOT_MATRIX_NAME + '_opt.dotmatrix')
  DotMatrixFile = open(DotMatrixFilePath, 'r')
  DotMatrix = DotMatrixFile.readlines()
  DotMatrixFile.close()
  
  DotMatrixInfo = parseDotMatrix(DotMatrix[0])
  
  assert (DOT_MATRIX_NAME + '_opt') == DotMatrixInfo[0]
  OPERAND_NUM = int(DotMatrixInfo[1])
  BITWIDTH = int(DotMatrixInfo[2])
  
  if (BITWIDTH == 16):
    ADD_DELAY = ADD_16_DELAY
  else:
    ADD_DELAY = ADD_32_DELAY
  
  # clear the compressor list
  COMPRESSOR_LIST.clear()
  DOT_MATRIX_OPERAND_LIST = []
  DOT_MATRIX_CONSTANT_LIST = []
  DOT_MATRIX_FINAL_GROUP = []
  DOT_MATRIX_ADD_CHAIN_GROUP = []
  
  # divide the operands into two parts: one for add tree, one for compressor
  operandsPartition()
  
  # build hybird tree
  buildHybridTree()
  
  generateCompressorTreeInNormalPattern()

def calcAllKnownBitsInDotMatrix():
  global OPERAND_NUM
  global BITWIDTH

  DotMatrixFilePath = os.path.join(BASE_DIR, DOT_MATRIX_NAME + '.dotmatrix')
  DotMatrixFile = open(DotMatrixFilePath, 'r')
  DotMatrix = DotMatrixFile.readlines()
  DotMatrixFile.close()
  
  DotMatrixInfo = parseDotMatrix(DotMatrix[0])
  
  assert DOT_MATRIX_NAME == DotMatrixInfo[0]
  OPERAND_NUM = int(DotMatrixInfo[1])
  BITWIDTH = int(DotMatrixInfo[2])
  
  # generate module declaration
  VerilogFilePath = os.path.join(BASE_DIR, PROJECT_NAME + '.sv')
  VerilogFile = open(VerilogFilePath, 'a')

  VerilogFile.write('\nmodule ' + DOT_MATRIX_NAME + '(\n')
  for i in range(0, OPERAND_NUM):
    VerilogFile.write('\t(* altera_attribute = "-name VIRTUAL_PIN on" *) input wire[' + str(BITWIDTH - 1) + ':0] operand_%d,\n' %i)
  VerilogFile.write('\t(* altera_attribute = "-name VIRTUAL_PIN on" *) output wire[' + str(BITWIDTH - 1) + ':0] result\n')
  VerilogFile.write(');\n\n')
  VerilogFile.close()
  
  Matrix = getMatrixFromDotMatrixFile(DOT_MATRIX_NAME, OPERAND_NUM, BITWIDTH, False)
  DSMatrix = getMatrixFromDotMatrixFile(DOT_MATRIX_NAME + '_delay_stage', OPERAND_NUM, BITWIDTH, True)
  
  T_Matrix = transportMatrix(Matrix, OPERAND_NUM, BITWIDTH, False)
  T_DSMatrix = transportMatrix(DSMatrix, OPERAND_NUM, BITWIDTH, True)
  
  EliminateResult = eliminateOneInTMatrix(T_Matrix, T_DSMatrix)
  
  Matrix = transportMatrix(EliminateResult[0], BITWIDTH, OPERAND_NUM * 2, False)
  DSMatrix = transportMatrix(EliminateResult[1], BITWIDTH, OPERAND_NUM * 2, True)
  
  # set the NULL in DSMatrix to 0.0-0
  for i in range(0, len(DSMatrix)):
    DS_row = DSMatrix[i]
    
    for j in range(0, len(DS_row)):
      DS_element = DS_row[j]
      
      if (DS_element == 'NULL'):
        DSMatrix[i][j] = '0.0-0'

  # eliminate the all zero row in matrix
  Valid_operand_list = []
  for i in range(0, len(Matrix)):
    Row = Matrix[i]
    
    AllZero = True
    for j in range(0, len(Row)):
      Element = Row[j]
      
      if (Element != '1\'b0'):
        AllZero = False
        break

    if (not AllZero):
      Valid_operand_list.append(i)

  Opt_Matrix = []
  Opt_DSMatrix = []
  for i in range(0, len(Valid_operand_list)):
    Opt_Matrix.append(Matrix[Valid_operand_list[i]])
    Opt_DSMatrix.append(DSMatrix[Valid_operand_list[i]])    

  printDotMatrixForMatrix(Opt_Matrix, DOT_MATRIX_NAME + '_opt', len(Valid_operand_list), BITWIDTH)
  printDotMatrixForMatrix(Opt_DSMatrix, DOT_MATRIX_NAME + '_opt_delay_stage', len(Valid_operand_list), BITWIDTH)
  
  # reset the global value
  OPERAND_NUM = 0
  BITWIDTH = 0
  
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
    print('####### processing matrix: ' + DOT_MATRIX_NAME)
    calcAllKnownBitsInDotMatrix()
    codegenForDotMatrix()
  
  generateCompressor_3_2()
  
#if _name_ == "_main_":
codegen()
