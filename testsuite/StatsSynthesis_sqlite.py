#! /usr/bin/python

import json
import sqlite3
import re
import sys
from datetime import datetime

def geomean(nums):
    if len(nums) != 0 :
        return (reduce(lambda x, y: x*y, nums))**(1.0/len(nums))
    return 0

def getFreq(data):
    return float(re.match(r"(^[1-9]\d*\.\d*|0\.\d*[1-9]\d*$|^\d*)",data).group(0))

def getOthers(data):
    data=data.replace(',','')
    return float(re.match(r"(^\d*)",data).group(1))

def append(list, data):
    if data != 0 :
        list.append(data)
    return

experiment_comment = sys.argv[2]
import_time = datetime.now()
experiment_url = sys.argv[3]

#Open a Json file
json_text = sys.stdin

read_data = '['+json_text.read()[1:]+']'

json_read = json.loads(read_data)

sql_db = sys.argv[1]
conn = sqlite3.connect(sql_db)
cursor = conn.cursor()

cursor.execute("INSERT INTO experiment_comment VALUES ('%s', '%s', '%s')" % (import_time, experiment_comment, experiment_url))
experiment_id = cursor.lastrowid

for data in json_read:	
  cursor.execute("INSERT INTO synthesis_run VALUES ('%s',%s,%s,%s,%s,%s,%s, %s)" % (data["name"], getOthers(data["restricted_fmax"]), getOthers(data["cycles"]), getOthers(data["les"]), getOthers(data["regs"]), getOthers(data["mult9"]), getOthers(data["mem_bit"]), experiment_id))

conn.commit()
conn.close()
