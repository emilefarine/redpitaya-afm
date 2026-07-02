`timescale 1ns / 1ps

module averager #(
    parameter int DATA_WIDTH = 14,
    parameter int ADDR_WIDTH = 10,  // size of BRAM = 2^ADDR_WIDTH
    parameter int PREC_BIT   = 3    // Precision bits for averaged data
) (
    input logic clk,
    input logic reset_n,
    input logic enabled,
    input logic [10:0] decimation_value,
    input logic signed [DATA_WIDTH-1:0] adc_data,
    output logic [ADDR_WIDTH-1:0] bram_addr,
    output logic signed [DATA_WIDTH + PREC_BIT - 1:0] bram_in,  // Output 17-bit averaged data
    output logic bram_we,  // write enable BRAM
    output logic [ADDR_WIDTH-1:0] averaged_sample_count  // NEW: Count of averaged samples
);

  // Count accumulated values
  int unsigned sample_count;

  // Accumulator for averaging - Must be wide enough for maximum decimation (1024)
  // Width = DATA_WIDTH + log2(MAX_DECIMATION) = 14 + 10 = 24 bits
  localparam ACCUMULATOR_WIDTH = DATA_WIDTH + 10;  // Support decimation up to 1024
  logic signed [ACCUMULATOR_WIDTH-1:0] current_accumulator;

  // count adresses for BRAM storage
  logic [ADDR_WIDTH-1:0] addr_counter;

  // floor_log2: priority encoder returning the position of the highest set bit.
  // Equivalent to floor(log2(x)) for power-of-2 values.
  function automatic logic [3:0] floor_log2(input logic [10:0] val);
    for (int i = 10; i >= 0; i--) begin
      if (val[i]) return i[3:0];
    end
    return 4'd0;
  endfunction

  logic [3:0] log2_dec;
  logic [3:0] prec_gained;
  logic [3:0] shift_amount;

  assign log2_dec    = floor_log2(decimation_value);
  // Oversampling theorem: extra bits = floor(log2(D)/2), capped at PREC_BIT
  assign prec_gained = (log2_dec >> 1) > PREC_BIT ? 4'(PREC_BIT) : (log2_dec >> 1);
  assign shift_amount = log2_dec - prec_gained;

  always_ff @(posedge clk or negedge reset_n) begin
    if (!reset_n) begin
      sample_count          <= 0;
      current_accumulator   <= 0;
      addr_counter          <= 0;
      bram_we               <= 0;
      bram_in               <= 0;
      bram_addr             <= 0;
      averaged_sample_count <= 0;
    end else begin

      bram_we <= 0;  // default : no writing

      // if acquisition enabled (disabled by measure_ctrl when last value acquired)
      if (enabled) begin

        // Add current data
        current_accumulator <= current_accumulator + adc_data;
        sample_count <= sample_count + 1;

        if (sample_count == decimation_value - 1) begin
          sample_count <= 0;

          // Compute average and write to BRAM
          // Statistical noise reduction method: Precision = log2(sqrt(decimation))
          // Formula: shift = log2(decimation) - log2(sqrt(decimation)) 
          //                = log2(decimation) - log2(decimation)/2
          //                = log2(decimation)/2 (rounded)
          // But cap at 3 bits maximum precision (17-bit format limit)
          // All outputs are 17-bit (sign-extended for lower decimation)
          // Precision calculation:
          // Output_bits = (DATA_WIDTH + log2(decimation)) - shift
          // Precision_bits = Output_bits - DATA_WIDTH = log2(decimation) - shift
          bram_in <= current_accumulator >>> shift_amount;

          bram_addr <= addr_counter;
          bram_we <= 1;  // Assert write enable

          // Reset for next average calculation
          current_accumulator <= 0;
          addr_counter <= addr_counter + 1;  // Increment for next averaged sample
          averaged_sample_count <= averaged_sample_count + 1;
        end

      end else begin  // If not enabled, reset internal states
        sample_count          <= 0;
        current_accumulator   <= 0;
        addr_counter          <= 0;
        bram_we               <= 0;
        bram_in               <= 0;
        bram_addr             <= 0;
        averaged_sample_count <= 0;
      end
    end
  end

endmodule
