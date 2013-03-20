FUs.LUTDelay = 0.2 / PERIOD

FUs.AddSub.Latencies = { 0.653 / PERIOD, 1.242 / PERIOD, 1.342 / PERIOD, 1.727 / PERIOD, 2.289 / PERIOD } --Add
FUs.Shift.Latencies = { 0.528 / PERIOD, 1.262 / PERIOD, 1.554 / PERIOD, 2.108 / PERIOD, 2.471 / PERIOD } --Shift
FUs.Mult.Latencies = { 0.511 / PERIOD, 1.714 / PERIOD, 1.346 / PERIOD, 2.775 / PERIOD, 5.273 / PERIOD } --Mul 
FUs.ICmp.Latencies = { 0.528 / PERIOD, 1.024 / PERIOD, 1.646 / PERIOD, 1.825 / PERIOD, 2.159 / PERIOD } --Cmp 
FUs.Sel.Latencies = { 0.685 / PERIOD, 0.680 / PERIOD, 0.709 / PERIOD, 0.704 / PERIOD, 1.044 / PERIOD } --Sel
FUs.Reduction.Latencies = { 0.689 / PERIOD, 1.380 / PERIOD, 1.073 / PERIOD, 1.209 / PERIOD, 1.357 / PERIOD } --Red

FUs.BRam.Latency = 1.0 / PERIOD -- Block RAM

FUs.Mux.Latencies = { 0.670 / PERIOD, --2-input 
                      0.763 / PERIOD, --3-input 
                      0.803 / PERIOD, --4-input 
                      0.796 / PERIOD, --5-input 
                      0.852 / PERIOD, --6-input 
                      1.010 / PERIOD, --7-input 
                      1.080 / PERIOD, --8-input 
                      1.080 / PERIOD, --9-input 
                      1.080 / PERIOD, --10-input 
                      1.080 / PERIOD, --11-input 
                      1.080 / PERIOD, --12-input 
                      1.080 / PERIOD, --13-input 
                      1.080 / PERIOD, --14-input 
                      1.080 / PERIOD, --15-input 
                      1.080 / PERIOD, --16-input 
                      1.670 / PERIOD, --17-input 
                      1.670 / PERIOD, --18-input 
                      1.670 / PERIOD, --19-input 
                      1.670 / PERIOD, --20-input 
                      1.670 / PERIOD, --21-input 
                      1.670 / PERIOD, --22-input 
                      1.670 / PERIOD, --23-input 
                      1.670 / PERIOD, --24-input 
                      1.670 / PERIOD, --25-input 
                      1.670 / PERIOD, --26-input 
                      1.670 / PERIOD, --27-input 
                      1.670 / PERIOD, --28-input 
                      1.670 / PERIOD, --29-input 
                      1.670 / PERIOD, --30-input 
                      1.670 / PERIOD, --31-input 
                      1.670 / PERIOD  --32-input
                    }
