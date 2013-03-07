#! /usr/bin/python

import json
import sqlite3
import re
import sys
from datetime import datetime

experiment_comment = sys.argv[3]
import_time = datetime.now()
experiment_url = sys.argv[4]

sql_db_from = sys.argv[1]
conn_from = sqlite3.connect(sql_db_from)
cursor_from = conn_from.cursor()

sql_db_to = sys.argv[2]
conn_to = sqlite3.connect(sql_db_to)
cursor_to = conn_to.cursor()


cursor_to.execute("INSERT INTO experiment_comment(datetime, comments, url) VALUES ('%s', '%s', '%s')" % (import_time, experiment_comment, experiment_url))
experiment_id = cursor_to.lastrowid

for row in cursor_from.execute("select * from synthesis_run"):
    cursor_to.execute("INSERT INTO synthesis_run(name, fmax, cycles, les, regs, mults, membits, experiment_id) VALUES ('%s',%s,%s,%s,%s,%s,%s, %s)" % (row[1], row[2], row[3], row[4], row[5], row[6], row[7] , experiment_id))

conn_from.commit()
conn_from.close()

conn_to.commit()
conn_to.close()
