FUs.LUTDelay = 0.4 / PERIOD
FUs.MaxLutSize = 4

FUs.BRam.Latency = 1.0 / PERIOD -- Block RAM


-- Latency table for EP2C35F672C6
FUs.Shift = { Latencies = { 0.912 / PERIOD, 2.922 / PERIOD, 3.612 / PERIOD, 4.623 / PERIOD, 5.537 / PERIOD }, --Shift
              Costs = {1 * 64, 27 * 64, 70 * 64, 171 * 64, 393 * 64}, --Shift
              StartInterval=1} --Shift
FUs.AddSub = { Latencies = { 1.066 / PERIOD, 2.139 / PERIOD, 2.835 / PERIOD, 4.138 / PERIOD, 6.753 / PERIOD }, --Add
	             Costs = {2 * 64, 10 * 64, 18 * 64, 34 * 64, 66 * 64}, --Add
               StartInterval=1}
FUs.Mult = { Latencies = { 0.908 / PERIOD, 2.181 / PERIOD, 2.504 / PERIOD, 6.580 / PERIOD, 9.377 / PERIOD }, --Mul
	           Costs = {1 * 64, 103 * 64, 344 * 64, 1211 * 64, 4478 * 64}, --Mul
             StartInterval=1} --Mul
FUs.ICmp   = { Latencies = { 0.912 / PERIOD, 1.906 / PERIOD, 2.612 / PERIOD, 4.050 / PERIOD, 6.660 / PERIOD }, --Cmp
	              Costs = {1 * 64, 8 * 64, 16 * 64, 32 * 64, 64 * 64}, --Cmp
                StartInterval=1} --Cmp
FUs.Reduction = { Latencies = { 0.785 / PERIOD, 1.597 / PERIOD, 1.744 / PERIOD, 2.172 / PERIOD, 2.733 / PERIOD }, --Red
	                Costs = {0 * 64, 3 * 64, 5 * 64, 11 * 64, 21 * 64}, --Red
                  StartInterval=1 } --Red
FUs.Mux    = { MaxAllowedMuxSize = 16,
               Latencies = { 1.296 / PERIOD,
                      1.353 / PERIOD,
                      1.943 / PERIOD,
                      1.893 / PERIOD,
                      1.960 / PERIOD,
                      2.006 / PERIOD,
                      2.330 / PERIOD,
                      2.063 / PERIOD,
                      2.302 / PERIOD,
                      2.397 / PERIOD,
                      2.516 / PERIOD,
                      2.607 / PERIOD,
                      2.879 / PERIOD,
                      2.550 / PERIOD,
                      2.716 / PERIOD,
                      2.927 / PERIOD,
                      3.246 / PERIOD,
                      3.171 / PERIOD,
                      3.137 / PERIOD,
                      3.078 / PERIOD,
                      3.423 / PERIOD,
                      4.334 / PERIOD,
                      3.555 / PERIOD,
                      3.756 / PERIOD,
                      4.263 / PERIOD,
                      4.143 / PERIOD,
                      3.961 / PERIOD,
                      4.367 / PERIOD,
                      4.149 / PERIOD,
                      4.707 / PERIOD,
                      4.912 / PERIOD },
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
