FUs.MaxLutSize = 4
-- Latency table for EP4CE75F29C6
FUs.AddSub = { LogicLevels = { 1, 8, 16, 32, 64 }, --Add
	             Costs = {2 * 64, 10 * 64, 18 * 64, 34 * 64, 66 * 64}, --Add
               StartInterval=1,
			         ChainingThreshold = ADDSUB_ChainingThreshold}
FUs.Shift = { LogicLevels = { 1, 4, 3, 5, 6 }, --Shift
              Costs = {1 * 64, 27 * 64, 70 * 64, 171 * 64, 393 * 64}, --Shift
              StartInterval=1,
			        ChainingThreshold = SHIFT_ChainingThreshold}
FUs.Mult = { LogicLevels = { 1, 11, 21, 48, 88 }, --Mul
	           Costs = {1 * 64, 103 * 64, 344 * 64, 1211 * 64, 4478 * 64}, --Mul
             StartInterval=1,
			       ChainingThreshold = MULT_ChainingThreshold}
FUs.ICmp   = { 	LogicLevels = { 1, 8, 16, 32, 64 }, --Cmp
	              Costs = {1 * 64, 8 * 64, 16 * 64, 32 * 64, 64 * 64}, --Cmp
               StartInterval=1,
			         ChainingThreshold = ICMP_ChainingThreshold}
FUs.Sel = { LogicLevels = { 1, 1, 1, 1, 1 }, --Sel
	          Costs = {1 * 64, 8 * 64, 16 * 64, 32 * 64, 64 * 64}, --Sel
            StartInterval=1,
            ChainingThreshold = SEL_ChainingThreshold}
FUs.Reduction = { LogicLevels = { 1, 2, 2, 3, 3 }, --Red
	                Costs = {0 * 64, 3 * 64, 5 * 64, 11 * 64, 21 * 64}, --Red
                  StartInterval=1,
                  ChainingThreshold = REDUCTION_ChainingThreshold}
FUs.Mux    = { MaxAllowedMuxSize = 32,
               LogicLevels = {
                { 1, 1, 1, 1, 1 }, --2-input
                { 1, 1, 1, 1, 1 }, --3-input
                { 3, 3, 3, 3, 3 }, --4-input
                { 3, 3, 3, 3, 3 }, --5-input
                { 3, 3, 3, 3, 3 }, --6-input
                { 3, 3, 3, 3, 3 }, --7-input
                { 4, 4, 4, 4, 4 }, --8-input
                { 4, 4, 5, 4, 4 }, --9-input
                { 4, 4, 4, 4, 4 }, --10-input
                { 4, 4, 5, 4, 4 }, --11-input
                { 5, 5, 5, 5, 5 }, --12-input
                { 5, 5, 5, 5, 5 }, --13-input
                { 5, 5, 6, 6 }, --14-input
                { 5, 6, 7, 6, 5 }, --15-input
                { 7, 5, 6, 6 }, --16-input
                { 7, 6, 6, 7, 7 }, --17-input
                { 6, 6, 7, 7, 6 }, --18-input
                { 7, 7, 7, 7, 6 }, --19-input
                { 7, 7, 8, 7, 7 }, --20-input
                { 9, 7, 8, 8 }, --21-input
                { 9, 9, 8, 7, 8 }, --22-input
                { 8, 8, 8, 8, 8 }, --23-input
                { 9, 9, 9, 9, 9 }, --24-input
                { 9, 9, 9, 9, 9 }, --25-input
                { 9, 9, 8, 9, 9 }, --26-input
                { 10, 10, 9, 10 }, --27-input
                { 9, 10, 10, 10 }, --28-input
                { 10, 10, 10, 10 }, --29-input
                { 10, 11, 11, 11 }, --30-input
                { 11, 11, 11, 11 }, --31-input
                { 11, 11, 11, 11 }, --32-input
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
               }, StartInterval=1,
			         ChainingThreshold = MUX_ChainingThreshold}
