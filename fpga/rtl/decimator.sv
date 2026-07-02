`timescale 1ns / 1ps

module decimator #(
    parameter ADDR_WIDTH = 12,
    parameter DATA_WIDTH = 14,  // BRAM data width (DAC WIDTH)
    parameter IO_WIDTH   = 14   // Output width to DAC
) (
    input  logic                           clk,
    input  logic                           rstn,
    input  logic                           enabled,
    input  logic          [          10:0] decimation_value,
    input  logic signed   [DATA_WIDTH-1:0] bram_out,          // Data from generation BRAM
    output logic                           bram_en,           // enable reading BRAM
    output logic unsigned [ADDR_WIDTH-1:0] bram_addr,
    output logic signed   [  IO_WIDTH-1:0] gen_out            // output to dac
);

  localparam CNT_WIDTH = 11;

  logic        [CNT_WIDTH-1:0] count;  // Tick counter
  logic signed [ IO_WIDTH-1:0] value;  // Stored value to send [decimation] x

  // Internal signal to track if a BRAM read was requested in the previous cycle
  logic                        bram_read_active_q;

  // BRAM read logic and output generation
  always_ff @(posedge clk or negedge rstn) begin
    if (!rstn) begin
      count              <= 0;
      bram_addr          <= 0;
      bram_en            <= 0;
      value              <= 0;
      gen_out            <= 0;
      bram_read_active_q <= 0;  // Reset read active flag
    end else begin
      bram_en            <= 0;  // Default to no read
      bram_read_active_q <= bram_en;  // Default to no request active for the *next* cycle

      if (enabled) begin
        // --- BRAM Address and Enable Control ---
        // Request a new sample from BRAM when count is 0
        if (count == 0) begin
          bram_en <= 1;  // Assert read enable for current cycle (requesting data for next cycle)
        end

        // --- Output to DAC ---
        if (bram_read_active_q) begin  // If a read was requested last cycle, data is now valid
          value   <= bram_out[IO_WIDTH-1:0];  // Latch the relevant bits from the 17-bit BRAM output
          gen_out <= bram_out[IO_WIDTH-1:0];  // Update DAC output immediatly
        end else begin
          gen_out <= value;  // Output the currently stored 'value'
        end

        // --- Decimation Counter ---
        if (count == decimation_value - 1) begin
          count <= 0;  // Reset counter for next decimation period
          bram_addr <= bram_addr + 1;  // Increment address for the *next* read
        end else begin
          count <= count + 1;  // Increment counter
        end
      end else begin  // If not enabled
        count              <= 0;
        bram_addr          <= 0;
        bram_en            <= 0;
        value              <= 0;
        gen_out            <= 0;
        bram_read_active_q <= 0;
      end
    end
  end

endmodule
