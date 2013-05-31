FUs.LUTDelay = 0.3 / PERIOD
FUs.MaxLutSize = 4
FUs.BRam.Latency = 1.0 / PERIOD -- Block RAM

FUs.AddSub = { Latencies = { 1.000 / PERIOD, 1.747 / PERIOD, 2.357 / PERIOD, 3.361 / PERIOD, 5.140 / PERIOD }, --Add
	             Costs = {2 * 64, 10 * 64, 18 * 64, 34 * 64, 66 * 64}, --Add
               StartInterval=1}
FUs.Shift = { Latencies = { 0.827 / PERIOD, 2.541 / PERIOD, 2.609 / PERIOD, 3.708 / PERIOD, 4.690 / PERIOD }, --Shift
              Costs = {1 * 64, 27 * 64, 70 * 64, 171 * 64, 393 * 64}, --Shift
              StartInterval=1} --Shift
FUs.Mult = { Latencies = { 0.827 / PERIOD, 2.620 / PERIOD, 3.170 / PERIOD, 6.806 / PERIOD, 9.087 / PERIOD }, --Mul
	           Costs = {1 * 64, 103 * 64, 344 * 64, 1211 * 64, 4478 * 64}, --Mul
             StartInterval=1} --Mul 
FUs.ICmp   = { Latencies = { 0.827 / PERIOD, 1.845 / PERIOD, 2.306 / PERIOD, 3.264 / PERIOD, 5.091 / PERIOD }, --Cmp
	              Costs = {1 * 64, 8 * 64, 16 * 64, 32 * 64, 64 * 64}, --Cmp
                StartInterval=1 } --Cmp 

FUs.Mux    = { MaxAllowedMuxSize = 16,
               Latencies = { 1.151 / PERIOD, --2-input 
                      1.935 / PERIOD, --3-input 
                      2.259 / PERIOD, --4-input 
                      2.395 / PERIOD, --5-input 
                      2.357 / PERIOD, --6-input 
                      2.357 / PERIOD, --7-input 
                      3.028 / PERIOD, --8-input 
                      3.028 / PERIOD, --9-input 
                      3.193 / PERIOD, --10-input 
                      3.193 / PERIOD, --11-input 
                      3.883 / PERIOD, --12-input 
                      3.841 / PERIOD, --13-input 
                      3.880 / PERIOD, --14-input 
                      3.900 / PERIOD, --15-input 
                      4.595 / PERIOD, --16-input 
                      4.294 / PERIOD, --17-input 
                      4.291 / PERIOD, --18-input 
                      4.379 / PERIOD, --19-input 
                      5.123 / PERIOD, --20-input 
                      5.333 / PERIOD, --21-input 
                      5.387 / PERIOD, --22-input 
                      5.207 / PERIOD, --23-input 
                      5.715 / PERIOD, --24-input 
                      5.263 / PERIOD, --25-input 
                      5.581 / PERIOD, --26-input 
                      6.133 / PERIOD, --27-input 
                      5.497 / PERIOD, --28-input 
                      5.783 / PERIOD, --29-input 
                      6.314 / PERIOD, --30-input 
                      6.204 / PERIOD, --31-input 
                      6.322 / PERIOD  --32-input
                    },
               Costs = {{64 , 512 , 1024 , 2048 , 4096},--2-input
                {128 , 128 , 128 , 128 , 128},--3-input
                {192 , 1024 , 2048 , 4096 , 8192},--4-input
                {256 , 2176 , 4224 , 8320 , 16512},--5-input
                {320 , 2880 , 5568 , 11072 , 21568},--6-input
                {384 , 3200 , 6272 , 12416 , 24704},--7-input
                {512 , 3456 , 6656 , 12800 , 25088},--8-input
                {512 , 3648 , 7552 , 12864 , 28608},--9-input
                {576 , 4672 , 9152 , 17152 , 34816},--10-input
                {832 , 5312 , 9600 , 19008 , 35136},--11-input
                {768 , 4992 , 9408 , 17216 , 34240},--12-input
                {1088 , 6016 , 11136 , 21440 , 42880},--13-input
                {1216 , 6528 , 12160 , 23552 , 43008},--14-input
                {1216 , 6272 , 12416 , 21696 , 42176},--15-input
                {1472 , 7232 , 14400 , 26048 , 50944},--16-input
                {1280 , 8384 , 15616 , 28096 , 54912},--17-input
                {1664 , 7552 , 15936 , 30528 , 50560},--18-input
                {1536 , 9408 , 16640 , 32448 , 63616},--19-input
                {1600 , 9600 , 17856 , 34624 , 67968},--20-input
                {1984 , 9792 , 16000 , 34880 , 68160},--21-input
                {1664 , 10368 , 19008 , 36864 , 72320},--22-input
                {1856 , 10880 , 20032 , 39104 , 76672},--23-input
                {1856 , 11136 , 20480 , 39296 , 76480},--24-input
                {2112 , 11712 , 21440 , 41152 , 80768},--25-input
                {2048 , 12160 , 22528 , 43008 , 84032},--26-input
                {2176 , 12352 , 22656 , 43328 , 84736},--27-input
                {2368 , 12864 , 23616 , 45312 , 88576},--28-input
                {2560 , 13440 , 24768 , 47424 , 92736},--29-input
                {2432 , 14016 , 24896 , 49408 , 92672},--30-input
                {2688 , 14528 , 25984 , 51712 , 97152},--31-input
                {2688 , 14784 , 28224 , 51712 , 105984},--32-input
               }, StartInterval=1 }
