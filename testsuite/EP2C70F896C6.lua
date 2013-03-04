FUs.LUTDelay = 0.4 / PERIOD

FUs.BRam.Latency = 1.0 / PERIOD -- Block RAM

-- Latency table for EP2C35F672C6
FUs.AddSub   .Latencies = { 2.113 / PERIOD, 2.700 / PERIOD, 4.052 / PERIOD, 6.608 / PERIOD }
FUs.Shift    .Latencies = { 3.073 / PERIOD, 3.311 / PERIOD, 4.792 / PERIOD, 5.829 / PERIOD }
FUs.Mult     .Latencies = { 2.181 / PERIOD, 2.504 / PERIOD, 6.503 / PERIOD, 9.221 / PERIOD }
FUs.ICmp     .Latencies = { 1.909 / PERIOD, 2.752 / PERIOD, 4.669 / PERIOD, 7.342 / PERIOD }
FUs.Sel      .Latencies = { 0.835 / PERIOD, 1.026 / PERIOD, 1.209 / PERIOD, 2.769 / PERIOD }
FUs.Reduction.Latencies = { 1.587 / PERIOD, 1.749 / PERIOD, 2.318 / PERIOD, 2.655 / PERIOD }
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
