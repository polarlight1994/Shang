FUs.LUTDelay = 0.4 / PERIOD
FUs.MaxLutSize = 4
FUs.MemoryBus.AddrLatency = 1.0 / PERIOD -- Block RAM

FUs.udiv = { Latencies = { ['64'] =  345.607 / PERIOD  } }
FUs.sdiv = { Latencies = { ['64'] =  345.607 / PERIOD  } }

-- Latency table for EP2C35F672C6
FUs.Shift = { Latencies = { 0 , 2.910 / PERIOD , 3.619 / PERIOD , 4.397 / PERIOD , 5.868 / PERIOD }, --Shift
              Costs = {1, 27, 70, 171, 393}, --Shift
              StartInterval=1} --Shift
FUs.AddSub = { Latencies = { 0 , 2.573 / PERIOD , 3.111 / PERIOD , 4.597 / PERIOD , 7.192 / PERIOD }, --Add
	             Costs = {2, 10, 18, 34, 66}, --Add
               StartInterval=1}
FUs.Mult = { Latencies = { 0 , 4.082 / PERIOD , 4.630 / PERIOD , 7.952 / PERIOD , 11.140 / PERIOD }, --Mult
	           Costs = {1, 103, 344, 1211, 4478}, --Mul
             StartInterval=1} --Mul
FUs.ICmp   = { Latencies = { 0 , 2.143 / PERIOD , 3.044 / PERIOD , 4.376 / PERIOD , 6.982 / PERIOD }, --ICmp
               Costs = {1, 8, 16, 32, 64}, --Cmp
               StartInterval=1} --Cmp
FUs.Mux    = { MaxAllowedMuxSize = 16,
               Latencies = { 1.248 / PERIOD, 1.649 / PERIOD, 1.739 / PERIOD, 1.744 / PERIOD, 1.918 / PERIOD, 1.955 / PERIOD, 2.119 / PERIOD, 2.351 / PERIOD, 2.216 / PERIOD, 2.346 / PERIOD, 2.348 / PERIOD, 2.346 / PERIOD, 2.489 / PERIOD, 2.538 / PERIOD, 2.496 / PERIOD, 2.613 / PERIOD, 2.634 / PERIOD, 2.607 / PERIOD, 2.608 / PERIOD, 2.793 / PERIOD, 2.824 / PERIOD, 2.732 / PERIOD, 2.882 / PERIOD, 2.795 / PERIOD, 2.824 / PERIOD, 2.844 / PERIOD, 3.042 / PERIOD, 3.011 / PERIOD, 2.841 / PERIOD, 2.895 / PERIOD, 3.031 / PERIOD, 3.448 / PERIOD, 3.028 / PERIOD, 3.452 / PERIOD, 3.413 / PERIOD, 3.268 / PERIOD, 3.261 / PERIOD, 3.417 / PERIOD, 3.052 / PERIOD, 3.468 / PERIOD, 3.271 / PERIOD, 3.183 / PERIOD, 3.446 / PERIOD, 3.269 / PERIOD, 3.155 / PERIOD, 3.158 / PERIOD, 3.439 / PERIOD, 3.267 / PERIOD, 3.273 / PERIOD, 3.470 / PERIOD, 3.512 / PERIOD, 3.463 / PERIOD, 3.298 / PERIOD, 3.453 / PERIOD, 3.427 / PERIOD, 3.535 / PERIOD, 3.308 / PERIOD, 3.487 / PERIOD, 3.437 / PERIOD, 3.410 / PERIOD, 3.411 / PERIOD, 3.502 / PERIOD, 3.452 / PERIOD, 3.585 / PERIOD, 3.581 / PERIOD, 3.753 / PERIOD, 3.594 / PERIOD, 3.547 / PERIOD, 3.548 / PERIOD, 3.566 / PERIOD, 3.563 / PERIOD, 3.599 / PERIOD, 3.542 / PERIOD, 3.564 / PERIOD, 3.612 / PERIOD, 3.530 / PERIOD, 3.569 / PERIOD, 3.586 / PERIOD, 3.543 / PERIOD, 3.757 / PERIOD, 3.684 / PERIOD, 3.730 / PERIOD, 3.732 / PERIOD, 3.603 / PERIOD, 3.632 / PERIOD, 3.679 / PERIOD, 3.723 / PERIOD, 3.771 / PERIOD, 3.774 / PERIOD, 3.634 / PERIOD, 3.831 / PERIOD, 3.758 / PERIOD, 3.797 / PERIOD, 3.813 / PERIOD, 3.743 / PERIOD, 3.783 / PERIOD, 3.813 / PERIOD, 3.776 / PERIOD, 3.743 / PERIOD, 3.780 / PERIOD, 3.816 / PERIOD, 3.809 / PERIOD, 3.780 / PERIOD, 3.805 / PERIOD, 3.766 / PERIOD, 3.723 / PERIOD, 3.831 / PERIOD, 3.954 / PERIOD, 3.797 / PERIOD, 3.787 / PERIOD, 3.779 / PERIOD, 3.806 / PERIOD, 3.945 / PERIOD, 3.791 / PERIOD, 3.861 / PERIOD, 3.802 / PERIOD, 3.805 / PERIOD, 3.857 / PERIOD, 3.827 / PERIOD, 3.853 / PERIOD, 3.818 / PERIOD, 3.805 / PERIOD, 3.851 / PERIOD, 3.892 / PERIOD, 3.856 / PERIOD, 3.816 / PERIOD, 3.847 / PERIOD, 4.261 / PERIOD, 4.308 / PERIOD, 4.288 / PERIOD, 4.174 / PERIOD, 4.168 / PERIOD, 4.379 / PERIOD, 4.226 / PERIOD, 4.097 / PERIOD, 4.390 / PERIOD, 4.208 / PERIOD, 4.209 / PERIOD, 4.209 / PERIOD, 4.329 / PERIOD, 4.125 / PERIOD, 4.253 / PERIOD, 4.217 / PERIOD, 4.437 / PERIOD, 4.203 / PERIOD, 4.229 / PERIOD, 4.243 / PERIOD, 4.281 / PERIOD, 4.232 / PERIOD, 4.236 / PERIOD, 4.239 / PERIOD, 4.195 / PERIOD, 4.272 / PERIOD, 4.313 / PERIOD, 4.227 / PERIOD, 4.280 / PERIOD, 4.234 / PERIOD, 4.168 / PERIOD, 4.311 / PERIOD, 4.398 / PERIOD, 4.494 / PERIOD, 4.274 / PERIOD, 4.338 / PERIOD, 4.277 / PERIOD, 4.318 / PERIOD, 4.209 / PERIOD, 4.289 / PERIOD, 4.197 / PERIOD, 4.372 / PERIOD, 4.310 / PERIOD, 4.300 / PERIOD, 4.338 / PERIOD, 4.355 / PERIOD, 4.368 / PERIOD, 4.442 / PERIOD, 4.327 / PERIOD, 4.286 / PERIOD, 4.382 / PERIOD, 4.425 / PERIOD, 4.283 / PERIOD, 4.289 / PERIOD, 4.371 / PERIOD, 4.231 / PERIOD, 4.213 / PERIOD, 4.295 / PERIOD, 4.398 / PERIOD, 4.214 / PERIOD, 4.422 / PERIOD, 4.371 / PERIOD, 4.273 / PERIOD, 4.492 / PERIOD, 4.251 / PERIOD, 4.270 / PERIOD, 4.353 / PERIOD, 4.366 / PERIOD, 4.342 / PERIOD, 4.496 / PERIOD, 4.240 / PERIOD, 4.440 / PERIOD, 4.346 / PERIOD, 4.332 / PERIOD, 4.344 / PERIOD, 4.396 / PERIOD, 4.504 / PERIOD, 4.329 / PERIOD, 4.537 / PERIOD, 4.251 / PERIOD, 4.406 / PERIOD, 4.390 / PERIOD, 4.386 / PERIOD, 4.415 / PERIOD, 4.462 / PERIOD, 4.408 / PERIOD, 4.413 / PERIOD, 4.390 / PERIOD, 4.480 / PERIOD, 4.311 / PERIOD, 4.538 / PERIOD, 4.375 / PERIOD, 4.646 / PERIOD, 4.381 / PERIOD, 4.402 / PERIOD, 4.286 / PERIOD, 4.410 / PERIOD, 4.425 / PERIOD, 4.389 / PERIOD, 4.531 / PERIOD, 4.483 / PERIOD, 4.647 / PERIOD, 4.526 / PERIOD, 4.408 / PERIOD, 4.503 / PERIOD, 4.351 / PERIOD, 4.489 / PERIOD, 4.560 / PERIOD, 4.545 / PERIOD, 4.400 / PERIOD, 4.544 / PERIOD, 4.404 / PERIOD, 4.469 / PERIOD, 4.498 / PERIOD, 4.450 / PERIOD, 4.426 / PERIOD, 4.466 / PERIOD, 4.518 / PERIOD, 4.441 / PERIOD, 4.497 / PERIOD, 4.477 / PERIOD, 4.473 / PERIOD, 4.687 / PERIOD, 4.415 / PERIOD, 4.451 / PERIOD, 4.483 / PERIOD, 4.452 / PERIOD, 4.483 / PERIOD, 4.598 / PERIOD, 4.601 / PERIOD, 4.573 / PERIOD, 4.595 / PERIOD, 4.586 / PERIOD, 4.485 / PERIOD, 4.575 / PERIOD, 4.691 / PERIOD, 4.780 / PERIOD, 4.498 / PERIOD, 4.617 / PERIOD, 4.658 / PERIOD, 4.621 / PERIOD, 4.762 / PERIOD, 4.526 / PERIOD, 4.660 / PERIOD, 4.613 / PERIOD, 4.532 / PERIOD, 4.472 / PERIOD, 4.551 / PERIOD, 4.723 / PERIOD, 4.539 / PERIOD, 4.471 / PERIOD, 4.533 / PERIOD, 4.658 / PERIOD, 4.656 / PERIOD, 4.644 / PERIOD, 4.738 / PERIOD, 4.681 / PERIOD, 4.773 / PERIOD, 4.622 / PERIOD, 4.737 / PERIOD, 4.777 / PERIOD, 4.794 / PERIOD, 4.800 / PERIOD, 4.629 / PERIOD, 4.538 / PERIOD, 4.699 / PERIOD, 4.862 / PERIOD, 4.723 / PERIOD, 4.611 / PERIOD, 4.702 / PERIOD, 4.618 / PERIOD, 4.862 / PERIOD, 4.758 / PERIOD, 4.810 / PERIOD, 4.489 / PERIOD, 4.840 / PERIOD, 4.826 / PERIOD, 4.804 / PERIOD, 4.665 / PERIOD, 4.728 / PERIOD, 4.750 / PERIOD, 4.668 / PERIOD, 4.492 / PERIOD, 4.770 / PERIOD, 4.761 / PERIOD, 4.677 / PERIOD, 4.700 / PERIOD, 4.814 / PERIOD, 4.598 / PERIOD, 4.714 / PERIOD, 4.494 / PERIOD, 4.619 / PERIOD, 4.776 / PERIOD, 4.716 / PERIOD, 4.739 / PERIOD, 4.775 / PERIOD, 4.746 / PERIOD, 4.695 / PERIOD, 4.804 / PERIOD, 4.619 / PERIOD, 4.684 / PERIOD, 4.838 / PERIOD, 4.745 / PERIOD, 4.773 / PERIOD, 4.851 / PERIOD, 4.831 / PERIOD, 4.771 / PERIOD, 4.758 / PERIOD, 4.816 / PERIOD, 4.837 / PERIOD, 4.725 / PERIOD, 4.693 / PERIOD, 4.850 / PERIOD, 4.695 / PERIOD, 4.760 / PERIOD, 4.853 / PERIOD, 4.664 / PERIOD, 4.932 / PERIOD, 4.765 / PERIOD, 4.694 / PERIOD, 4.836 / PERIOD, 4.674 / PERIOD, 4.898 / PERIOD, 4.959 / PERIOD, 4.771 / PERIOD, 4.883 / PERIOD, 4.937 / PERIOD, 4.949 / PERIOD, 4.988 / PERIOD, 4.959 / PERIOD, 4.908 / PERIOD, 4.869 / PERIOD, 4.884 / PERIOD, 4.893 / PERIOD, 4.847 / PERIOD, 4.801 / PERIOD, 4.994 / PERIOD, 4.872 / PERIOD, 4.790 / PERIOD, 4.868 / PERIOD, 4.970 / PERIOD, 4.933 / PERIOD, 4.985 / PERIOD, 4.908 / PERIOD, 4.784 / PERIOD, 4.833 / PERIOD, 4.885 / PERIOD, 4.893 / PERIOD, 4.854 / PERIOD, 4.823 / PERIOD, 4.767 / PERIOD, 4.881 / PERIOD, 4.968 / PERIOD, 4.976 / PERIOD, 4.857 / PERIOD, 4.876 / PERIOD, 4.957 / PERIOD, 4.995 / PERIOD, 4.948 / PERIOD, 5.063 / PERIOD, 4.916 / PERIOD, 4.942 / PERIOD, 4.851 / PERIOD, 4.961 / PERIOD, 5.012 / PERIOD, 4.791 / PERIOD, 4.890 / PERIOD, 4.816 / PERIOD, 4.816 / PERIOD, 4.912 / PERIOD, 4.883 / PERIOD, 4.885 / PERIOD, 4.932 / PERIOD, 4.953 / PERIOD, 4.939 / PERIOD, 4.901 / PERIOD, 5.096 / PERIOD, 4.930 / PERIOD, 4.849 / PERIOD, 4.899 / PERIOD, 5.110 / PERIOD, 4.947 / PERIOD, 4.809 / PERIOD, 4.889 / PERIOD, 4.905 / PERIOD, 4.954 / PERIOD, 4.920 / PERIOD, 5.128 / PERIOD, 4.968 / PERIOD, 5.048 / PERIOD, 4.987 / PERIOD, 4.993 / PERIOD, 4.973 / PERIOD, 5.000 / PERIOD, 5.046 / PERIOD, 5.061 / PERIOD, 4.935 / PERIOD, 4.942 / PERIOD, 4.903 / PERIOD, 5.092 / PERIOD, 5.119 / PERIOD, 4.969 / PERIOD, 4.925 / PERIOD, 4.930 / PERIOD, 5.028 / PERIOD, 5.009 / PERIOD, 4.974 / PERIOD, 4.916 / PERIOD, 5.196 / PERIOD, 5.144 / PERIOD, 4.904 / PERIOD, 5.000 / PERIOD, 5.091 / PERIOD, 5.025 / PERIOD, 5.064 / PERIOD, 4.874 / PERIOD, 4.909 / PERIOD, 5.043 / PERIOD, 4.979 / PERIOD, 4.895 / PERIOD, 5.076 / PERIOD, 4.992 / PERIOD, 5.203 / PERIOD, 5.069 / PERIOD, 5.152 / PERIOD, 4.973 / PERIOD, 5.079 / PERIOD, 5.082 / PERIOD, 5.182 / PERIOD, 5.097 / PERIOD, 4.989 / PERIOD, 4.950 / PERIOD, 4.977 / PERIOD, 5.235 / PERIOD, 5.121 / PERIOD, 5.027 / PERIOD, 5.195 / PERIOD, 5.182 / PERIOD, 5.047 / PERIOD, 5.024 / PERIOD, 5.007 / PERIOD, 5.182 / PERIOD, 5.114 / PERIOD, 5.039 / PERIOD, 5.158 / PERIOD, 5.038 / PERIOD, 5.015 / PERIOD, 5.034 / PERIOD, 4.976 / PERIOD, 5.100 / PERIOD, 5.113 / PERIOD, 5.105 / PERIOD, 5.081 / PERIOD, 5.143 / PERIOD, 5.145 / PERIOD, 4.916 / PERIOD, 5.186 / PERIOD, 5.120 / PERIOD, 5.232 / PERIOD, 4.910 / PERIOD, 5.125 / PERIOD, 5.095 / PERIOD, 5.158 / PERIOD, 5.125 / PERIOD, 5.142 / PERIOD, 5.152 / PERIOD, 5.160 / PERIOD, 5.136 / PERIOD, 5.077 / PERIOD, 5.209 / PERIOD, 5.182 / PERIOD, 4.995 / PERIOD, 5.068 / PERIOD, 5.092 / PERIOD, 5.139 / PERIOD, 5.118 / PERIOD, 5.136 / PERIOD, 5.085 / PERIOD, 5.155 / PERIOD, 5.270 / PERIOD, 5.647 / PERIOD, 5.301 / PERIOD, 5.149 / PERIOD, 5.435 / PERIOD },
              Costs = { 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13, 14, 14, 15, 15, 16, 16, 17, 17, 18, 18, 19, 19, 20, 20, 21, 21, 22, 22, 23, 23, 24, 24, 25, 25, 26, 26, 27, 27, 28, 28, 29, 29, 30, 30, 31, 31, 32, 32, 33, 33, 34, 34, 35, 35, 36, 36, 37, 37, 38, 38, 39, 39, 40, 40, 41, 41, 42, 42, 43, 43, 44, 44, 45, 45, 46, 46, 47, 47, 48, 48, 49, 49, 50, 50, 51, 51, 52, 52, 53, 53, 54, 54, 55, 55, 56, 56, 57, 57, 58, 58, 59, 59, 60, 60, 61, 61, 62, 62, 63, 63, 64, 64, 65, 65, 66, 66, 67, 67, 68, 68, 69, 69, 70, 70, 71, 71, 72, 72, 73, 73, 74, 74, 75, 75, 76, 76, 77, 77, 78, 78, 79, 79, 80, 80, 81, 81, 82, 82, 83, 83, 84, 84, 85, 85, 86, 86, 87, 87, 88, 88, 89, 89, 90, 90, 91, 91, 92, 92, 93, 93, 94, 94, 95, 95, 96, 96, 97, 97, 98, 98, 99, 99, 100, 100, 101, 101, 102, 102, 103, 103, 104, 104, 105, 105, 106, 106, 107, 107, 108, 108, 109, 109, 110, 110, 111, 111, 112, 112, 113, 113, 114, 114, 115, 115, 116, 116, 117, 117, 118, 118, 119, 119, 120, 120, 121, 121, 122, 122, 123, 123, 124, 124, 125, 125, 126, 126, 127, 127, 128, 128, 129, 129, 130, 130, 131, 131, 132, 132, 133, 133, 134, 134, 135, 135, 136, 136, 137, 137, 138, 138, 139, 139, 140, 140, 141, 141, 142, 142, 143, 143, 144, 144, 145, 145, 146, 146, 147, 147, 148, 148, 149, 149, 150, 150, 151, 151, 152, 152, 153, 153, 154, 154, 155, 155, 156, 156, 157, 157, 158, 158, 159, 159, 160, 160, 161, 161, 162, 162, 163, 163, 164, 164, 165, 165, 166, 166, 167, 167, 168, 168, 169, 169, 170, 170, 171, 171, 172, 172, 173, 173, 174, 174, 175, 175, 176, 176, 177, 177, 178, 178, 179, 179, 180, 180, 181, 181, 182, 182, 183, 183, 184, 184, 185, 185, 186, 186, 187, 187, 188, 188, 189, 189, 190, 190, 191, 191, 192, 192, 193, 193, 194, 194, 195, 195, 196, 196, 197, 197, 198, 198, 199, 199, 200, 200, 201, 201, 202, 202, 203, 203, 204, 204, 205, 205, 206, 206, 207, 207, 208, 208, 209, 209, 210, 210, 211, 211, 212, 212, 213, 213, 214, 214, 215, 215, 216, 216, 217, 217, 218, 218, 219, 219, 220, 220, 221, 221, 222, 222, 223, 223, 224, 224, 225, 225, 226, 226, 227, 227, 228, 228, 229, 229, 230, 230, 231, 231, 232, 232, 233, 233, 234, 234, 235, 235, 236, 236, 237, 237, 238, 238, 239, 239, 240, 240, 241, 241, 242, 242, 243, 243, 244, 244, 245, 245, 246, 246, 247, 247, 248, 248, 249, 249, 250, 250, 251, 251, 252, 252, 253, 253, 254, 254, 255, 255, 256 }, 
              StartInterval=1 }
