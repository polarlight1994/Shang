FUs.LUTDelay = 0.4 / PERIOD

FUs.BRam.Latency = 1.0 / PERIOD -- Block RAM

-- Latency table for EP2C35F672C6
FUs.Shift.Latencies = { 0.912 / PERIOD, 2.922 / PERIOD, 3.612 / PERIOD, 4.623 / PERIOD, 5.537 / PERIOD } --Shift
FUs.AddSub.Latencies = { 1.066 / PERIOD, 2.139 / PERIOD, 2.835 / PERIOD, 4.138 / PERIOD, 6.753 / PERIOD } --Add
FUs.Mult.Latencies = { 0.908 / PERIOD, 2.181 / PERIOD, 2.504 / PERIOD, 6.580 / PERIOD, 9.377 / PERIOD } --Mul
FUs.Sel.Latencies = { 1.053 / PERIOD, 1.193 / PERIOD, 1.233 / PERIOD, 1.500 / PERIOD, 2.758 / PERIOD } --Sel
FUs.Reduction.Latencies = { 0.785 / PERIOD, 1.597 / PERIOD, 1.744 / PERIOD, 2.172 / PERIOD, 2.733 / PERIOD } --Red
FUs.ICmp.Latencies = { 0.912 / PERIOD, 1.906 / PERIOD, 2.612 / PERIOD, 4.050 / PERIOD, 6.660 / PERIOD } --Cmp
FUs.Mux.Latencies = { 1.296 / PERIOD,
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
                      4.912 / PERIOD }
