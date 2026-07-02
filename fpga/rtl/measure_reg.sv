`timescale 1ns / 1ps

module measure_reg (
    input logic clk,
    input logic rstn,

    // access from system bus
    input  logic [ 3:0] addr,   // 4-bit address (8 regs max)
    input  logic [31:0] wdata,
    input  logic        wen,
    input  logic        ren,
    output logic [31:0] rdata,

    // output to measure_ctrl
    output logic        start_measure,  // start measure when set to 1 by PS
    output logic [31:0] sig_size,       // PS sets how many samples tu generate / acquire
    output logic [31:0] delay,          // delay [clk ticks] between gen & acq start
    output logic [10:0] decimation,     // decimation factor (16-1024, power of 2)

    // input from measure_ctrl
    input logic        end_measure,   // set to 1 when measure ended (raises IRQ)
    input logic [31:0] count_measure, // count how many samples have been acquired    
    input logic        busy_i,        // high when measure_ctrl FSM is RUNNING
    input logic        ps_access_denied_i  // high when PS tried to access BRAM during busy
);

  // internal registers
  logic        start_measure_reg;
  logic [31:0] sig_size_reg;
  logic [31:0] delay_reg;  // New: Internal register for delay
  logic [10:0] decimation_reg;
  logic        ps_access_denied_d1;  // Latch PS access denial for STATUS register

  // reading / writing registers
  always_ff @(posedge clk or negedge rstn) begin
    if (!rstn) begin
      start_measure_reg <= 1'b0;  // start is set to 0 when measure finished and read by PS
      sig_size_reg      <= 32'd0;
      delay_reg         <= 32'd0;
      decimation_reg    <= 11'd64;
      ps_access_denied_d1 <= 1'b0;
    end else begin
      // Latch PS access denial events (can be cleared by writing to STATUS register)
      if (ps_access_denied_i) begin
        ps_access_denied_d1 <= 1'b1;
      end else if (wen && addr == 4'd9) begin
        // Clear denial bit when PS writes to STATUS register
        ps_access_denied_d1 <= 1'b0;
      end

      if (wen) begin
        case (addr)
          4'd2:    start_measure_reg <= wdata[0];  // bit[0] = start_measure
          4'd3:    delay_reg <= wdata;
          4'd4:    sig_size_reg <= wdata;  // bit[31:0]
          4'd8:    decimation_reg <= wdata[10:0];  // bit[10:0] = decimation (16-1024)
          default: ;  // do nothing
        endcase
      end
    end
  end  // always_ff

  // reading output (PS reads a register)
  always_comb begin
    case (addr)
      4'd2:    rdata = {31'd0, start_measure_reg};
      4'd3:    rdata = delay_reg;
      4'd4:    rdata = sig_size_reg;
      4'd5:    rdata = {31'd0, end_measure};  // status from measure_ctrl
      4'd6:    rdata = count_measure;  // 32-bit counter from measure_ctrl
      4'd7:    rdata = 32'h15062026;  // BUILD_DATE register (read-only, DDMMYYYY format)
      4'd8:    rdata = {21'd0, decimation_reg};  // decimation factor (read-back)
      4'd9:    rdata = {29'd0, 1'b0, ps_access_denied_d1, busy_i};  // STATUS: [0]=busy, [1]=ps_access_denied, [2]=rw_collision(reserved)
      default: rdata = 32'h0;
    endcase
  end

  // register outputs for the controller
  assign start_measure = start_measure_reg;
  assign sig_size      = sig_size_reg;
  assign delay         = delay_reg;
  assign decimation    = decimation_reg;

endmodule
