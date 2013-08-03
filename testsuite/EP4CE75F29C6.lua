FUs.LUTDelay = 0.3 / PERIOD
FUs.MaxLutSize = 4
FUs.MemoryBus.AddrLatency = 1.0 / PERIOD -- Block RAM

FUs.udiv = { Latencies = { ['64'] = 290.639 / PERIOD } }
FUs.sdiv = { Latencies = { ['64'] = 290.639 / PERIOD } }

FUs.AddSub = { Latencies = { 0 , 2.156 / PERIOD , 2.606 / PERIOD , 3.650 / PERIOD , 5.503 / PERIOD }, --Add
	             Costs = {2, 10, 18, 34, 66}, --Add
               StartInterval=1}
FUs.Shift = { Latencies = { 0 , 2.558 / PERIOD , 2.920 / PERIOD , 4.031 / PERIOD , 5.109 / PERIOD }, --Shift
              Costs = {1, 27, 70, 171, 393}, --Shift
              StartInterval=1} --Shift
FUs.Mult = { Latencies = { 0 , 4.142 / PERIOD , 4.627 / PERIOD , 7.846 / PERIOD , 10.383 / PERIOD }, --Mult
	           Costs = {1, 103, 344, 1211, 4478}, --Mul
             StartInterval=1} --Mul 
FUs.ICmp   = { Latencies = { 0 , 1.976 / PERIOD , 2.667 / PERIOD , 3.609 / PERIOD , 5.459 / PERIOD }, --ICmp
               Costs = {1, 8, 16, 32, 64}, --Cmp
               StartInterval=1 } --Cmp 

FUs.Mux    = { MaxAllowedMuxSize = 16,
               Latencies = { 1.080 / PERIOD, 1.382 / PERIOD, 1.515 / PERIOD, 1.518 / PERIOD, 1.678 / PERIOD, 1.667 / PERIOD, 1.676 / PERIOD, 1.997 / PERIOD, 1.849 / PERIOD, 2.066 / PERIOD, 2.020 / PERIOD, 2.094 / PERIOD, 2.132 / PERIOD, 2.006 / PERIOD, 2.051 / PERIOD, 2.262 / PERIOD, 2.156 / PERIOD, 2.278 / PERIOD, 2.278 / PERIOD, 2.248 / PERIOD, 2.417 / PERIOD, 2.275 / PERIOD, 2.446 / PERIOD, 2.385 / PERIOD, 2.276 / PERIOD, 2.417 / PERIOD, 2.427 / PERIOD, 2.403 / PERIOD, 2.615 / PERIOD, 2.541 / PERIOD, 2.441 / PERIOD, 2.506 / PERIOD, 2.597 / PERIOD, 2.556 / PERIOD, 2.595 / PERIOD, 2.899 / PERIOD, 2.884 / PERIOD, 2.869 / PERIOD, 2.608 / PERIOD, 2.706 / PERIOD, 2.611 / PERIOD, 2.595 / PERIOD, 2.612 / PERIOD, 2.761 / PERIOD, 2.741 / PERIOD, 2.730 / PERIOD, 2.875 / PERIOD, 2.838 / PERIOD, 2.937 / PERIOD, 2.747 / PERIOD, 2.726 / PERIOD, 2.963 / PERIOD, 2.892 / PERIOD, 2.908 / PERIOD, 2.914 / PERIOD, 2.928 / PERIOD, 2.925 / PERIOD, 2.889 / PERIOD, 2.810 / PERIOD, 2.893 / PERIOD, 2.998 / PERIOD, 2.979 / PERIOD, 3.056 / PERIOD, 3.027 / PERIOD, 3.004 / PERIOD, 2.898 / PERIOD, 3.023 / PERIOD, 3.021 / PERIOD, 3.023 / PERIOD, 3.124 / PERIOD, 3.109 / PERIOD, 3.013 / PERIOD, 3.007 / PERIOD, 3.126 / PERIOD, 3.218 / PERIOD, 3.114 / PERIOD, 3.126 / PERIOD, 3.090 / PERIOD, 3.027 / PERIOD, 3.126 / PERIOD, 3.069 / PERIOD, 3.040 / PERIOD, 3.212 / PERIOD, 3.282 / PERIOD, 3.172 / PERIOD, 3.065 / PERIOD, 3.227 / PERIOD, 3.127 / PERIOD, 3.119 / PERIOD, 3.184 / PERIOD, 3.279 / PERIOD, 3.229 / PERIOD, 3.269 / PERIOD, 3.091 / PERIOD, 3.270 / PERIOD, 3.285 / PERIOD, 3.229 / PERIOD, 3.307 / PERIOD, 3.255 / PERIOD, 3.198 / PERIOD, 3.196 / PERIOD, 3.269 / PERIOD, 3.257 / PERIOD, 3.228 / PERIOD, 3.272 / PERIOD, 3.345 / PERIOD, 3.311 / PERIOD, 3.303 / PERIOD, 3.275 / PERIOD, 3.244 / PERIOD, 3.297 / PERIOD, 3.289 / PERIOD, 3.252 / PERIOD, 3.272 / PERIOD, 3.295 / PERIOD, 3.317 / PERIOD, 3.281 / PERIOD, 3.286 / PERIOD, 3.253 / PERIOD, 3.307 / PERIOD, 3.358 / PERIOD, 3.289 / PERIOD, 3.382 / PERIOD, 3.453 / PERIOD, 3.255 / PERIOD, 3.415 / PERIOD, 3.316 / PERIOD, 3.332 / PERIOD, 3.282 / PERIOD, 3.435 / PERIOD, 3.337 / PERIOD, 3.484 / PERIOD, 3.343 / PERIOD, 3.396 / PERIOD, 3.570 / PERIOD, 3.444 / PERIOD, 3.431 / PERIOD, 3.369 / PERIOD, 3.453 / PERIOD, 3.603 / PERIOD, 3.524 / PERIOD, 3.587 / PERIOD, 3.430 / PERIOD, 3.442 / PERIOD, 3.483 / PERIOD, 3.607 / PERIOD, 3.488 / PERIOD, 3.494 / PERIOD, 3.629 / PERIOD, 3.626 / PERIOD, 3.485 / PERIOD, 3.413 / PERIOD, 3.553 / PERIOD, 3.658 / PERIOD, 3.690 / PERIOD, 3.626 / PERIOD, 3.709 / PERIOD, 3.713 / PERIOD, 3.685 / PERIOD, 3.688 / PERIOD, 3.587 / PERIOD, 3.598 / PERIOD, 3.673 / PERIOD, 3.721 / PERIOD, 3.892 / PERIOD, 3.598 / PERIOD, 3.622 / PERIOD, 3.719 / PERIOD, 3.593 / PERIOD, 3.584 / PERIOD, 3.707 / PERIOD, 3.638 / PERIOD, 3.645 / PERIOD, 3.639 / PERIOD, 3.657 / PERIOD, 3.659 / PERIOD, 3.624 / PERIOD, 3.591 / PERIOD, 3.823 / PERIOD, 3.643 / PERIOD, 3.709 / PERIOD, 3.772 / PERIOD, 3.805 / PERIOD, 3.694 / PERIOD, 3.720 / PERIOD, 3.684 / PERIOD, 3.746 / PERIOD, 3.680 / PERIOD, 3.599 / PERIOD, 3.921 / PERIOD, 3.736 / PERIOD, 3.698 / PERIOD, 3.718 / PERIOD, 3.610 / PERIOD, 3.656 / PERIOD, 3.644 / PERIOD, 3.803 / PERIOD, 3.715 / PERIOD, 3.695 / PERIOD, 3.668 / PERIOD, 3.613 / PERIOD, 3.776 / PERIOD, 3.628 / PERIOD, 3.799 / PERIOD, 3.655 / PERIOD, 3.625 / PERIOD, 3.575 / PERIOD, 3.592 / PERIOD, 3.794 / PERIOD, 3.739 / PERIOD, 3.655 / PERIOD, 3.830 / PERIOD, 3.630 / PERIOD, 3.812 / PERIOD, 3.820 / PERIOD, 3.709 / PERIOD, 3.729 / PERIOD, 3.746 / PERIOD, 3.654 / PERIOD, 3.764 / PERIOD, 3.745 / PERIOD, 3.856 / PERIOD, 3.857 / PERIOD, 3.777 / PERIOD, 3.705 / PERIOD, 3.996 / PERIOD, 3.770 / PERIOD, 3.677 / PERIOD, 4.003 / PERIOD, 3.773 / PERIOD, 3.806 / PERIOD, 3.909 / PERIOD, 3.762 / PERIOD, 3.899 / PERIOD, 3.670 / PERIOD, 3.774 / PERIOD, 3.941 / PERIOD, 4.026 / PERIOD, 3.740 / PERIOD, 3.935 / PERIOD, 3.838 / PERIOD, 3.745 / PERIOD, 3.824 / PERIOD, 3.843 / PERIOD, 3.749 / PERIOD, 3.912 / PERIOD, 3.788 / PERIOD, 3.863 / PERIOD, 3.696 / PERIOD, 3.834 / PERIOD, 3.841 / PERIOD, 3.784 / PERIOD, 3.921 / PERIOD, 3.883 / PERIOD, 3.845 / PERIOD, 4.054 / PERIOD, 3.854 / PERIOD, 3.781 / PERIOD, 3.671 / PERIOD, 3.929 / PERIOD, 3.733 / PERIOD, 3.919 / PERIOD, 3.949 / PERIOD, 3.955 / PERIOD, 3.806 / PERIOD, 3.921 / PERIOD, 4.013 / PERIOD, 4.080 / PERIOD, 3.924 / PERIOD, 3.909 / PERIOD, 3.918 / PERIOD, 3.954 / PERIOD, 3.957 / PERIOD, 3.912 / PERIOD, 3.965 / PERIOD, 3.863 / PERIOD, 3.898 / PERIOD, 3.921 / PERIOD, 3.796 / PERIOD, 3.836 / PERIOD, 3.898 / PERIOD, 3.921 / PERIOD, 3.867 / PERIOD, 3.894 / PERIOD, 4.085 / PERIOD, 3.792 / PERIOD, 3.899 / PERIOD, 4.042 / PERIOD, 3.887 / PERIOD, 4.051 / PERIOD, 3.932 / PERIOD, 3.926 / PERIOD, 3.910 / PERIOD, 3.941 / PERIOD, 3.919 / PERIOD, 3.879 / PERIOD, 3.902 / PERIOD, 4.025 / PERIOD, 4.095 / PERIOD, 3.943 / PERIOD, 4.134 / PERIOD, 4.042 / PERIOD, 3.985 / PERIOD, 4.032 / PERIOD, 3.874 / PERIOD, 4.058 / PERIOD, 3.898 / PERIOD, 3.969 / PERIOD, 4.057 / PERIOD, 4.039 / PERIOD, 3.938 / PERIOD, 4.062 / PERIOD, 4.011 / PERIOD, 4.102 / PERIOD, 4.206 / PERIOD, 3.867 / PERIOD, 4.005 / PERIOD, 4.126 / PERIOD, 3.966 / PERIOD, 4.173 / PERIOD, 3.947 / PERIOD, 4.131 / PERIOD, 4.080 / PERIOD, 4.031 / PERIOD, 4.061 / PERIOD, 3.958 / PERIOD, 3.987 / PERIOD, 4.257 / PERIOD, 4.251 / PERIOD, 4.044 / PERIOD, 4.025 / PERIOD, 4.122 / PERIOD, 4.296 / PERIOD, 4.193 / PERIOD, 4.151 / PERIOD, 4.077 / PERIOD, 3.984 / PERIOD, 3.967 / PERIOD, 4.061 / PERIOD, 3.941 / PERIOD, 4.217 / PERIOD, 4.332 / PERIOD, 4.122 / PERIOD, 4.038 / PERIOD, 4.093 / PERIOD, 4.101 / PERIOD, 3.953 / PERIOD, 4.073 / PERIOD, 4.014 / PERIOD, 4.161 / PERIOD, 4.126 / PERIOD, 4.150 / PERIOD, 4.401 / PERIOD, 4.133 / PERIOD, 3.941 / PERIOD, 4.062 / PERIOD, 4.087 / PERIOD, 4.287 / PERIOD, 4.124 / PERIOD, 4.278 / PERIOD, 4.230 / PERIOD, 4.072 / PERIOD, 4.249 / PERIOD, 4.130 / PERIOD, 4.146 / PERIOD, 4.185 / PERIOD, 4.299 / PERIOD, 4.137 / PERIOD, 4.192 / PERIOD, 4.091 / PERIOD, 4.264 / PERIOD, 4.203 / PERIOD, 4.064 / PERIOD, 4.221 / PERIOD, 4.185 / PERIOD, 4.196 / PERIOD, 4.164 / PERIOD, 4.164 / PERIOD, 4.018 / PERIOD, 4.200 / PERIOD, 4.075 / PERIOD, 4.287 / PERIOD, 4.117 / PERIOD, 4.314 / PERIOD, 4.224 / PERIOD, 4.109 / PERIOD, 4.184 / PERIOD, 4.187 / PERIOD, 4.193 / PERIOD, 4.155 / PERIOD, 4.322 / PERIOD, 4.210 / PERIOD, 4.225 / PERIOD, 4.228 / PERIOD, 4.224 / PERIOD, 4.190 / PERIOD, 4.207 / PERIOD, 4.263 / PERIOD, 4.251 / PERIOD, 4.149 / PERIOD, 4.290 / PERIOD, 4.121 / PERIOD, 4.242 / PERIOD, 4.376 / PERIOD, 4.260 / PERIOD, 4.216 / PERIOD, 4.222 / PERIOD, 4.324 / PERIOD, 4.370 / PERIOD, 4.168 / PERIOD, 4.208 / PERIOD, 4.161 / PERIOD, 4.188 / PERIOD, 4.219 / PERIOD, 4.151 / PERIOD, 4.198 / PERIOD, 4.058 / PERIOD, 4.390 / PERIOD, 4.208 / PERIOD, 4.208 / PERIOD, 4.291 / PERIOD, 4.191 / PERIOD, 4.182 / PERIOD, 4.180 / PERIOD, 4.135 / PERIOD, 4.157 / PERIOD, 4.288 / PERIOD, 4.229 / PERIOD, 4.299 / PERIOD, 4.061 / PERIOD, 4.253 / PERIOD, 4.299 / PERIOD, 4.330 / PERIOD, 4.341 / PERIOD, 4.258 / PERIOD, 4.109 / PERIOD, 4.251 / PERIOD, 4.212 / PERIOD, 4.314 / PERIOD, 4.338 / PERIOD, 4.235 / PERIOD, 4.384 / PERIOD, 4.203 / PERIOD, 4.168 / PERIOD, 4.276 / PERIOD, 4.278 / PERIOD, 4.322 / PERIOD, 4.275 / PERIOD, 4.354 / PERIOD, 4.225 / PERIOD, 4.373 / PERIOD, 4.243 / PERIOD, 4.209 / PERIOD, 4.239 / PERIOD, 4.221 / PERIOD, 4.440 / PERIOD, 4.279 / PERIOD, 4.295 / PERIOD, 4.219 / PERIOD, 4.165 / PERIOD, 4.307 / PERIOD, 4.247 / PERIOD, 4.338 / PERIOD, 4.212 / PERIOD, 4.393 / PERIOD, 4.227 / PERIOD, 4.233 / PERIOD, 4.391 / PERIOD, 4.373 / PERIOD, 4.320 / PERIOD, 4.251 / PERIOD, 4.374 / PERIOD, 4.344 / PERIOD, 4.371 / PERIOD, 4.417 / PERIOD, 4.303 / PERIOD, 4.357 / PERIOD, 4.261 / PERIOD, 4.385 / PERIOD, 4.408 / PERIOD, 4.260 / PERIOD, 4.397 / PERIOD, 4.467 / PERIOD, 4.253 / PERIOD, 4.281 / PERIOD, 4.228 / PERIOD, 4.309 / PERIOD, 4.357 / PERIOD, 4.343 / PERIOD, 4.360 / PERIOD, 4.233 / PERIOD, 4.334 / PERIOD, 4.449 / PERIOD, 4.506 / PERIOD, 4.352 / PERIOD, 4.419 / PERIOD, 4.303 / PERIOD, 4.273 / PERIOD, 4.359 / PERIOD, 4.434 / PERIOD, 4.329 / PERIOD, 4.300 / PERIOD, 4.396 / PERIOD, 4.411 / PERIOD, 4.301 / PERIOD, 4.396 / PERIOD, 4.580 / PERIOD, 4.498 / PERIOD, 4.500 / PERIOD, 4.514 / PERIOD, 4.399 / PERIOD },
              Costs = { 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13, 14, 14, 15, 15, 16, 16, 17, 17, 18, 18, 19, 19, 20, 20, 21, 21, 22, 22, 23, 23, 24, 24, 25, 25, 26, 26, 27, 27, 28, 28, 29, 29, 30, 30, 31, 31, 32, 32, 33, 33, 34, 34, 35, 35, 36, 36, 37, 37, 38, 38, 39, 39, 40, 40, 41, 41, 42, 42, 43, 43, 44, 44, 45, 45, 46, 46, 47, 47, 48, 48, 49, 49, 50, 50, 51, 51, 52, 52, 53, 53, 54, 54, 55, 55, 56, 56, 57, 57, 58, 58, 59, 59, 60, 60, 61, 61, 62, 62, 63, 63, 64, 64, 65, 65, 66, 66, 67, 67, 68, 68, 69, 69, 70, 70, 71, 71, 72, 72, 73, 73, 74, 74, 75, 75, 76, 76, 77, 77, 78, 78, 79, 79, 80, 80, 81, 81, 82, 82, 83, 83, 84, 84, 85, 85, 86, 86, 87, 87, 88, 88, 89, 89, 90, 90, 91, 91, 92, 92, 93, 93, 94, 94, 95, 95, 96, 96, 97, 97, 98, 98, 99, 99, 100, 100, 101, 101, 102, 102, 103, 103, 104, 104, 105, 105, 106, 106, 107, 107, 108, 108, 109, 109, 110, 110, 111, 111, 112, 112, 113, 113, 114, 114, 115, 115, 116, 116, 117, 117, 118, 118, 119, 119, 120, 120, 121, 121, 122, 122, 123, 123, 124, 124, 125, 125, 126, 126, 127, 127, 128, 128, 129, 129, 130, 130, 131, 131, 132, 132, 133, 133, 134, 134, 135, 135, 136, 136, 137, 137, 138, 138, 139, 139, 140, 140, 141, 141, 142, 142, 143, 143, 144, 144, 145, 145, 146, 146, 147, 147, 148, 148, 149, 149, 150, 150, 151, 151, 152, 152, 153, 153, 154, 154, 155, 155, 156, 156, 157, 157, 158, 158, 159, 159, 160, 160, 161, 161, 162, 162, 163, 163, 164, 164, 165, 165, 166, 166, 167, 167, 168, 168, 169, 169, 170, 170, 171, 171, 172, 172, 173, 173, 174, 174, 175, 175, 176, 176, 177, 177, 178, 178, 179, 179, 180, 180, 181, 181, 182, 182, 183, 183, 184, 184, 185, 185, 186, 186, 187, 187, 188, 188, 189, 189, 190, 190, 191, 191, 192, 192, 193, 193, 194, 194, 195, 195, 196, 196, 197, 197, 198, 198, 199, 199, 200, 200, 201, 201, 202, 202, 203, 203, 204, 204, 205, 205, 206, 206, 207, 207, 208, 208, 209, 209, 210, 210, 211, 211, 212, 212, 213, 213, 214, 214, 215, 215, 216, 216, 217, 217, 218, 218, 219, 219, 220, 220, 221, 221, 222, 222, 223, 223, 224, 224, 225, 225, 226, 226, 227, 227, 228, 228, 229, 229, 230, 230, 231, 231, 232, 232, 233, 233, 234, 234, 235, 235, 236, 236, 237, 237, 238, 238, 239, 239, 240, 240, 241, 241, 242, 242, 243, 243, 244, 244, 245, 245, 246, 246, 247, 247, 248, 248, 249, 249, 250, 250, 251, 251, 252, 252, 253, 253, 254, 254, 255, 255, 256 },
              StartInterval=1 }
