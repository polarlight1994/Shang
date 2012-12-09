#!/bin/python

import re
import json

def geomean(nums):
    return (reduce(lambda x, y: x*y, nums))**(1.0/len(nums))

def getFreq(data):
    return float(re.match(r"(^[1-9]\d*\.\d*|0\.\d*[1-9]\d*$|^\d*)",data).group(0))

def getOthers(data):
    data=data.replace(',','')
    return float(re.match(r"(^\d*)",data).group(1))

def append_nozero(list, data):
    if data != 0 :
        list.append(data)
    return

version = '0.1'
