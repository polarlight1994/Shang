import re
import sys
import os
import inspect
import codecs

DotMatrixFileList = []

VerilogFile = ''
MatrixName = ''
partial_product_num = 0
partial_product_bits_num = 0
append_partial_product_num = 0

file_no = 0
compressor_no = 0

BASE_DIR = ''

CompressorsList = dict()

def codegen():
  global BASE_DIR
  
  Info_group = parseCompressorInfo()
  Name = Info_group[0]
  BASE_DIR = Info_group[1]
  
  generateCompressFile(Name)

def parseCompressorInfo():
  Info_group = []
  
  CompressorInfoFilePath = os.path.join(os.path.dirname(__file__), 'CompressorInfo.txt')
  CompressorInfoFile = open(CompressorInfoFilePath, 'r')
  InfoLines = CompressorInfoFile.readlines()
  CompressorInfoFile.close()

  Infos = InfoLines[0].split(',')
  Info_group.append(Infos[0])
  Info_group.append(Infos[1])

  return Info_group  

def parseMatrixInfo(headline):
  MatrixInfo = []

  Info = headline.split('-')
  MatrixInfo.append(Info[0])
  MatrixInfo.append(int(Info[1]))
  MatrixInfo.append(int(Info[2]))

  return MatrixInfo

def parseDotMatrix(DotMatrixs_FileName):
  global DotMatrixFileList  
  
  DotMatrixsFilePath = os.path.join(BASE_DIR, DotMatrixs_FileName + '.dotmatrix')
  DotMatrixs_file = open(DotMatrixsFilePath, 'r')
  DotMatrixs = DotMatrixs_file.readlines()
  DotMatrixs_file.close()

  startline_num = 0
  endline_num = 0
  while(endline_num <= len(DotMatrixs) - 2):
    headline = DotMatrixs[startline_num]
    MatrixInfo = parseMatrixInfo(headline)
    MatrixName = MatrixInfo[0]
    MatrixRowNum = MatrixInfo[1]
    MatrixColNum = MatrixInfo[2]
    endline_num = startline_num + MatrixRowNum
    
    MatrixFilePath = os.path.join(BASE_DIR, MatrixName + '.dotmatrix')
    Matrix_file = open(MatrixFilePath, 'w')
    for MatrixLine in range(startline_num, endline_num + 1):
      Matrix_file.write(DotMatrixs[MatrixLine])
    Matrix_file.close()

    startline_num = endline_num + 1
    DotMatrixFileList.append(MatrixName)

def generateCompressFile(DotMatrixs_FileName):
  global VerilogFile
  VerilogFile = DotMatrixs_FileName
  
  parseDotMatrix(DotMatrixs_FileName)

  for FileName in DotMatrixFileList:    
    generateCompressorForMatrix(FileName)

  generateCompressor_3_2(VerilogFile)

def generateCompressor_3_2(VerilogFile):
  WTAFilePath = os.path.join(BASE_DIR, VerilogFile + '.sv')
  WTA_file = open(WTAFilePath, 'a')

  WTA_file.write('\nmodule compressor_3_2(\n' + \
                 '\tinput wire a,\n' + \
                 '\tinput wire b,\n' + \
                 '\tinput wire cin,\n' + \
                 '\toutput wire result,' + \
                 '\toutput wire cout\n' + \
                 ');\n\n' + \
                 '\tassign result = (a ^ b) ^ cin;\n' + \
                 '\tassign cout = (a & b) | (a & cin) | (b & cin);\n\n' + \
                 'endmodule\n')

  WTA_file.close()

def initMatrix(FileName):
  global MatrixName
  global partial_product_num
  global partial_product_bits_num
  global append_partial_product_num
  global CompressorsList
  
  PartialProductMatrixPath = os.path.join(BASE_DIR, FileName + '.dotmatrix')
  partial_product_matrix = open(PartialProductMatrixPath, 'r')
  partial_products = partial_product_matrix.readlines()
  partial_product_matrix.close()

  MatrixInfo = parseMatrixInfo(partial_products[0])
  MatrixName = MatrixInfo[0]
  partial_product_num = MatrixInfo[1]
  partial_product_bits_num = MatrixInfo[2]
  append_partial_product_num = 0
  CompressorsList.clear()

  WTAFilePath = os.path.join(BASE_DIR, VerilogFile + '.sv')
  WTA_file = open(WTAFilePath, 'a')

  WTA_file.write('\nmodule ' + MatrixName + '(\n')
  for no in range(0, append_partial_product_num + partial_product_num):
    WTA_file.write('\t(* altera_attribute = "-name VIRTUAL_PIN on" *) input wire[' + str(partial_product_bits_num-1) + ':0] operand_%d,\n' %no)
  WTA_file.write('\t(* altera_attribute = "-name VIRTUAL_PIN on" *) output wire[' + str(partial_product_bits_num-1) + ':0] result\n')
  WTA_file.write(');\n\n')
  WTA_file.close()

  matrix = [['1\'b0' for col in range(append_partial_product_num + partial_product_num)] for row in range(partial_product_bits_num)]

  for num in range(1, append_partial_product_num + partial_product_num + 1):
    partial_product = partial_products[num]
    partial_product_bits = partial_product.split(',')

    rownum = 0

    for partial_product_bit in partial_product_bits:

      partial_product_bit_name = '1\'b0'

      if (partial_product_bit == '\n' or partial_product_bit == ''):
        continue

      matrix[partial_product_bits_num - 1 - rownum][num - 1] = partial_product_bit;
      rownum += 1

  printMatrix(matrix)
  
  return matrix

def eliminateOneInMatrix(matrix):
  one_number_in_matrix = [0 for col in range(partial_product_bits_num)]
  
  for row_no in range(0, len(matrix)):
    row = matrix[row_no]

    for col_no in range(0, len(row)):
      if (matrix[row_no][col_no] == '1\'b1'):
        one_number_in_matrix[row_no] += 1

  final_one_number_in_matrix = ['1\'b0' for col in range(len(one_number_in_matrix))]
  for i in range(0, len(one_number_in_matrix)):
    one_no = one_number_in_matrix[i]

    remain_one_no = one_no % 2
    carry_one_no = one_no / 2

    if (remain_one_no == 1):
      final_one_number_in_matrix[i] = '1\'b1'

    if (i != len(one_number_in_matrix) - 1):
      one_number_in_matrix[i + 1] += carry_one_no

  new_matrix = matrix
  for row_no in range(0, len(matrix)):
    row = matrix[row_no]

    for col_no in range(0, len(row)):
      if (matrix[row_no][col_no] == '1\'b1'):
        new_matrix[row_no][col_no] = '1\'b0'
      else:
        new_matrix[row_no][col_no] = matrix[row_no][col_no]

  for row_no in range(0, len(new_matrix)):
    row = new_matrix[row_no]
	
    final_one_no = final_one_number_in_matrix[row_no]
    if (final_one_no != 0):
      row.append('1\'b1')

  return new_matrix

def shrinkMatrix(dot_matrix):
  global append_partial_product_num

  for row_no in range(append_partial_product_num + partial_product_num, append_partial_product_num + partial_product_num):
    row = dot_matrix[row_no]

    for col_no in range(0, len(row)):
      if (re.search(r'booth_encoder_', row[col_no])):
        result = re.findall(r'_\d+', row[col_no])
        origin_row_no = result[0].replace('_', '')

        assert dot_matrix[int(origin_row_no)][col_no] == '1\'b0'
        dot_matrix[int(origin_row_no)][col_no] = row[col_no]

        row[col_no] = '1\'b0'

      if (row[col_no] == '1\'b1'):
        for i in range(0, len(dot_matrix)):
          if (dot_matrix[i][col_no] == '1\'b0'):
            dot_matrix[i][col_no] = row[col_no]
            row[col_no] = '1\'b0'

            break

  eliminated_row_no = []
  for row_no in range(0, append_partial_product_num + partial_product_num):
    row = dot_matrix[row_no]

    eliminated = True
    for col_no in range(0, len(row)):
      if (row[col_no] != '1\'b0'):
        eliminated = False

    if (eliminated):
      eliminated_row_no.append(row_no)

  new_dot_matrix = []
  for row_no in range(0, append_partial_product_num + partial_product_num):    
    if (eliminated_row_no.count(row_no)):
      append_partial_product_num -= 1
      continue
    else:
      new_dot_matrix.append(dot_matrix[row_no])

  return new_dot_matrix

def sortMatrix(matrix):
  new_matrix = [['1\'b0' for col in range(append_partial_product_num + partial_product_num)] for row in range(partial_product_bits_num)]

  for row_no in range(0, len(matrix)):
    row = matrix[row_no]
    new_matrix_col_no = 0
  
    for col_no in range(0, len(row)):
      if (matrix[row_no][col_no] != '1\'b0'):
        new_matrix[row_no][new_matrix_col_no] = matrix[row_no][col_no]
        new_matrix_col_no += 1

  return new_matrix

def getCompressionStatus(matrix):
  compress_status = [0 for row in range(partial_product_bits_num)]
  
  for row_no in range(0,len(matrix)):
    row = matrix[row_no]
    element_no = 0

    for col_no in range(0, len(row)):
      if (matrix[row_no][col_no] != '1\'b0'):
        element_no += 1

    compress_status[row_no] = element_no

  return compress_status

def checkIdenticalCompressor(triple_elements):
  global CompressorsList

  key_1 = triple_elements[0] + triple_elements[1] + triple_elements[2]
  if (CompressorsList.get(key_1)):
    return CompressorsList.get(key_1)

  key_2 = triple_elements[0] + triple_elements[2] + triple_elements[1]
  if (CompressorsList.get(key_2)):
    return CompressorsList.get(key_2)

  key_3 = triple_elements[1] + triple_elements[0] + triple_elements[2]
  if (CompressorsList.get(key_3)):
    return CompressorsList.get(key_3)

  key_4 = triple_elements[1] + triple_elements[2] + triple_elements[0]
  if (CompressorsList.get(key_4)):
    return CompressorsList.get(key_4)

  key_5 = triple_elements[2] + triple_elements[0] + triple_elements[1]
  if (CompressorsList.get(key_5)):
    return CompressorsList.get(key_5)

  key_6 = triple_elements[2] + triple_elements[1] + triple_elements[0]
  if (CompressorsList.get(key_6)):
    return CompressorsList.get(key_6)

  return None

def compressTripleLines(matrix, stage):
  compress_status = getCompressionStatus(matrix)

  for row_no in range(0, len(matrix) - 1):
    if (compress_status[row_no] >= 3):
      num = compress_status[row_no] // 3
      
      for no in range(0, num):
        triple_elements = [matrix[row_no][3*no], matrix[row_no][3*no+1], matrix[row_no][3*no+2]]

        matrix[row_no][3*no] = '1\'b0'
        matrix[row_no][3*no+1] = '1\'b0'
        matrix[row_no][3*no+2] = '1\'b0'

        sum_name = 'NULL'
        carry_name = 'NULL'

        IdenticalCompressor = checkIdenticalCompressor(triple_elements)
        if (IdenticalCompressor != None):
          print(str(IdenticalCompressor))
          sum_name = IdenticalCompressor[0]
          carry_name = IdenticalCompressor[1]
        else:
          sum_name = 'sum_' + str(row_no) + '_' + str(no) + '_' + str(stage)
          carry_name = 'carry_' + str(row_no) + '_' + str(no) + '_' + str(stage)
          compressor_generator(triple_elements, sum_name, carry_name)

        matrix[row_no].append(sum_name)
        if (row_no <= partial_product_bits_num - 2):
          matrix[row_no+1].append(carry_name)

      reminder = compress_status[row_no] % 3
      if (reminder == 2):
        triple_elements = [matrix[row_no][3*num], matrix[row_no][3*num+1], '1\'b0']

        matrix[row_no][3*num] = '1\'b0'
        matrix[row_no][3*num+1] = '1\'b0'

        sum_name = 'NULL'
        carry_name = 'NULL'

        IdenticalCompressor = checkIdenticalCompressor(triple_elements)
        if (IdenticalCompressor != None):
          print(str(IdenticalCompressor))
          sum_name = IdenticalCompressor[0]
          carry_name = IdenticalCompressor[1]
        else:
          sum_name = 'sum_' + str(row_no) + '_' + str(num) + '_' + str(stage)
          carry_name = 'carry_' + str(row_no) + '_' + str(num) + '_' + str(stage)
          compressor_generator(triple_elements, sum_name, carry_name)

        matrix[row_no].append(sum_name)
        if (row_no <= partial_product_bits_num - 2):
          matrix[row_no+1].append(carry_name)

    if (compress_status[row_no] == 2):
      need_to_compress = False
      for no in range(0, row_no):
        if (compress_status[row_no - 1 - no] >= 3):
          need_to_compress = True
        elif (compress_status[row_no - 1 - no] == 2):
          continue
        else:
          break

      if (need_to_compress):
        triple_elements = [matrix[row_no][0], matrix[row_no][1], '1\'b0']

        matrix[row_no][0] = '1\'b0'
        matrix[row_no][1] = '1\'b0'

        sum_name = 'NULL'
        carry_name = 'NULL'

        IdenticalCompressor = checkIdenticalCompressor(triple_elements)
        if (IdenticalCompressor != None):
          print(str(IdenticalCompressor))
          sum_name = IdenticalCompressor[0]
          carry_name = IdenticalCompressor[1]
        else:
          sum_name = 'sum_' + str(row_no) + '_' + str(no) + '_' + str(stage)
          carry_name = 'carry_' + str(row_no) + '_' + str(no) + '_' + str(stage)
          compressor_generator(triple_elements, sum_name, carry_name)

        matrix[row_no].append(sum_name)
        if (row_no <= partial_product_bits_num - 2):
          matrix[row_no+1].append(carry_name)

  if (compress_status[partial_product_bits_num-1] >= 2):
    WTAFilePath = os.path.join(BASE_DIR, VerilogFile + '.sv')
    WTA_file = open(WTAFilePath, 'a')
    WTA_file.write('wire result_MSB_%d' %stage + ' = ' + matrix[partial_product_bits_num-1][0])
    matrix[partial_product_bits_num-1][0] = '1\'b0'
    for no in range(1, compress_status[partial_product_bits_num-1]):
      WTA_file.write(' ^ ' + matrix[partial_product_bits_num-1][no])
      matrix[partial_product_bits_num-1][no] = '1\'b0'
    WTA_file.write(';\n\n')
    matrix[partial_product_bits_num-1].append('result_MSB_%d' %stage)        

  matrix = sortMatrix(matrix)
  
  return matrix

def compress(matrix):
  need_to_compress = True
  stage = 0
  partial_product_matrix = sortMatrix(matrix)
  printMatrix(partial_product_matrix)
  
  while(need_to_compress):
    partial_product_matrix = compressTripleLines(partial_product_matrix, stage)
    printMatrix(partial_product_matrix)

    compress_status = getCompressionStatus(partial_product_matrix)
    need_to_compress = False
    for row_no in range(0,len(partial_product_matrix)):
      if (compress_status[row_no] >= 3):
        need_to_compress = True

    stage += 1

  dataa = []
  datab = []
  for row_no in range(0, len(matrix) - 1):
    dataa.append(partial_product_matrix[row_no][0])
    datab.append(partial_product_matrix[row_no][1])

  WTAFilePath = os.path.join(BASE_DIR, VerilogFile + '.sv')
  WTA_file = open(WTAFilePath, 'a')

  WTA_file.write('wire[%d-2:0] dataa = {' %partial_product_bits_num)
  WTA_file.write(str(dataa[partial_product_bits_num - 2]))
  for row_no in range(0, len(dataa) - 1):
    WTA_file.write(', ' + str(dataa[len(dataa) - 2 - row_no]))
  WTA_file.write('};\n')

  WTA_file.write('wire[%d-2:0] datab = {' %partial_product_bits_num)
  WTA_file.write(str(datab[partial_product_bits_num - 2]))
  for row_no in range(0, len(datab) - 1):
    WTA_file.write(', ' + str(datab[len(datab) - 2 - row_no]))
  WTA_file.write('};\n\n')

  WTA_file.write('wire[%d-1:0] ' %partial_product_bits_num + 'add_result = dataa + datab;\n')
  WTA_file.write('assign result[%d-1] = ' %partial_product_bits_num + partial_product_matrix[partial_product_bits_num - 1][0])
  
  for no in range(1, len(partial_product_matrix[partial_product_bits_num - 1]) - 1):
    elementOfMSB = partial_product_matrix[partial_product_bits_num - 1][no]
    if (elementOfMSB != '1\'b0'):
      WTA_file.write(' ^ ' + elementOfMSB)
  
  WTA_file.write(' ^ add_result[%d];\n' %(partial_product_bits_num - 1))
  WTA_file.write('assign result[%d-2:0] = ' %partial_product_bits_num + 'add_result[%d:0]' %(partial_product_bits_num - 2) + ';\n\n')
  
def printMatrix(matrix):
  global file_no
  
  matrix_file_name = 'matrix_%d.txt' % file_no
  MatrixFilePath = os.path.join(BASE_DIR, matrix_file_name)
  matrix_file = open(MatrixFilePath, 'w')
  
  for row in matrix:
    for col in row:
      matrix_file.write(str(col) + '\t')

    matrix_file.write('\n')

  file_no += 1
  matrix_file.close()

def compressor_generator(triple_elements, sum_name, carry_name):
  global compressor_no
  global CompressorsList
  
  WTAFilePath = os.path.join(BASE_DIR, VerilogFile + '.sv')
  WTA_file = open(WTAFilePath, 'a')
  WTA_file.write('wire ' + sum_name + ';\n' + \
                 'wire ' + carry_name + ';\n' + \
                 'compressor_3_2 compressor_3_2_%d' %compressor_no + '(\n' + \
                 '\t.a(' + triple_elements[0] + '),\n' + \
                 '\t.b(' + triple_elements[1] + '),\n' + \
                 '\t.cin(' + triple_elements[2] + '),\n' + \
                 '\t.result(' + sum_name + '),\n' + \
                 '\t.cout(' + carry_name + ')\n' + \
                 ');\n\n')
  WTA_file.close()

  key = triple_elements[0] + triple_elements[1] + triple_elements[2]
  value = [sum_name, carry_name]
  CompressorsList.setdefault(key, value)
  
  compressor_no += 1

def generateCompressorForMatrix(FileName):  
  matrix = initMatrix(FileName)

  matrix = eliminateOneInMatrix(matrix)

  compress(matrix)

  WTAFilePath = os.path.join(BASE_DIR, VerilogFile + '.sv')
  WTA_file = open(WTAFilePath, 'a')
  WTA_file.write('endmodule\n')
  WTA_file.close()

#if __name__ == "__main__":
codegen()
