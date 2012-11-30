#!/bin/bash

for i in 0.5 1.5 2.5 3.5
do
  sed  -e "s/<lut-latency>/$i/" < patch.patch | \
  ssh 192.168.1.253 "buildbot try --connect=pb --master=192.168.1.253:5557 --username=try --passwd=asic2012 --diff=-\
                     --properties=CUSTOM_TARGET=benchmark_synthesis_main,CUSTOM_MAKE_OPTION=-i \
                     --branch=master --baserev=`git rev-parse adsc-local/master`  --comment=$i";
done
