Modules.__ip_udiv_i64 = {
InstTmplt = [=[

// 64bit div
reg [5:0] div64_$(num)_counter;
wire [6:0] div64_$(num)_counter_wire = div64_$(num)_counter + 1'b1;

reg div64_$(num)_busy;
assign $(fin) = ~(div64_$(num)_busy | $(en));

assign $(out0) = div64_$(num)opa / div64_$(num)opb;

always @(posedge $(clk), negedge $(rst)) begin
  if (!$(rst)) begin
   div64_$(num)_counter <= 6'b0;
   div64_$(num)_busy <= 1'b0;
  end else begin
    if ($(en)) begin
	  div64_$(num)_busy <= 1'b1;
      div64_$(num)_counter <= 6'b0;
    end

	if (div64_$(num)_busy)
   	  div64_$(num)_counter <= div64_$(num)_counter_wire;

    if (div64_$(num)_counter_wire[6])
	  div64_$(num)_busy <= 1'b0;
  end
end
]=],
TimingInfo = { NumOperands = 2, Latency = 64, OperandInfo = { { Name = [=[div64_$(num)opa]=], SizeInBits = 64 }, { Name = [=[div64_$(num)opb]=], SizeInBits = 64 } } }
}

Modules.__ip_udiv_i32 = {
InstTmplt = [=[
// 32bit div
reg [5:0] div32_$(num)_counter;
wire [6:0] div32_$(num)_counter_wire = div32_$(num)_counter + 1'b1;

reg div32_$(num)_busy;
assign $(fin) = ~(div32_$(num)_busy | $(en));

assign $(out0) = div32_$(num)opa / div32_$(num)opb;

always @(posedge $(clk), negedge $(rst)) begin
  if (!$(rst)) begin
   div32_$(num)_counter <= 6'b0;
   div32_$(num)_busy <= 1'b0;
  end else begin
    if ($(en)) begin
	  div32_$(num)_busy <= 1'b1;
      div32_$(num)_counter <= 6'b0;
    end

	if (div32_$(num)_busy)
	  div32_$(num)_counter <= div32_$(num)_counter_wire;

    if (div32_$(num)_counter_wire[6])
	  div32_$(num)_busy <= 1'b0;
  end
end
]=],
TimingInfo = { NumOperands = 2, Latency = 32, OperandInfo = { { Name = [=[div32_$(num)opa]=], SizeInBits = 32 }, { Name = [=[div32_$(num)opb]=], SizeInBits = 32 } } }
}

Modules.__ip_sdiv_i32 = {
InstTmplt = [=[
// 32bit div
reg [5:0] div32_$(num)_counter;
wire [6:0] div32_$(num)_counter_wire = div32_$(num)_counter + 1'b1;

reg div32_$(num)_busy;
assign $(fin) = ~(div32_$(num)_busy | $(en));

assign $(out0) = $signed (div32_$(num)opa) / $signed (div32_$(num)opb);

always @(posedge $(clk), negedge $(rst)) begin
  if (!$(rst)) begin
   div32_$(num)_counter <= 6'b0;
   div32_$(num)_busy <= 1'b0;
  end else begin
    if ($(en)) begin
	  div32_$(num)_busy <= 1'b1;
      div32_$(num)_counter <= 6'b0;
    end

	if (div32_$(num)_busy)
	  div32_$(num)_counter <= div32_$(num)_counter_wire;

    if (div32_$(num)_counter_wire[6])
	  div32_$(num)_busy <= 1'b0;
  end
end
]=],
TimingInfo = { NumOperands = 2, Latency = 32, OperandInfo = { { Name = [=[div32_$(num)opa]=], SizeInBits = 32 }, { Name = [=[div32_$(num)opb]=], SizeInBits = 32 } } }
}

Modules.__ip_srem_i32 = {
InstTmplt = [=[

// 32bit div
reg [4:0] div32_$(num)_counter;
wire [5:0] div32_$(num)_counter_wire = div32_$(num)_counter + 1'b1;

reg div32_$(num)_busy;
assign $(fin) = ~(div32_$(num)_busy | $(en));

assign $(out0) = $signed (div32_$(num)opa) % $signed (div32_$(num)opb);

always @(posedge $(clk), negedge $(rst)) begin
  if (!$(rst)) begin
   div32_$(num)_counter <= 6'b0;
   div32_$(num)_busy <= 1'b0;
  end else begin
    if ($(en)) begin
	  div32_$(num)_busy <= 1'b1;
      div32_$(num)_counter <= 6'b0;
    end

	if (div32_$(num)_busy)
	  div32_$(num)_counter <= div32_$(num)_counter_wire;

    if (div32_$(num)_counter_wire[5])
	  div32_$(num)_busy <= 1'b0;
  end
end
]=],
TimingInfo = { NumOperands = 2, Latency = 32, OperandInfo = { { Name = [=[div32_$(num)opa]=], SizeInBits = 32 }, { Name = [=[div32_$(num)opb]=], SizeInBits = 32 } } }
}
