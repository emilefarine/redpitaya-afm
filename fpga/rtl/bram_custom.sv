`timescale 1ns / 1ps

// Simple dual-port BRAM — single clock domain.
// Port A: read-only generation port (driven by the decimator via measure_ctrl).
// Port B: shared port, arbitrated by busy in ps_pl_interface_wrapper:
//         averager acquisition write while RUNNING, PS read/write while idle.
// Both ports are driven by the same clock; no CDC required.
module bram_custom #(
    parameter ADDR_WIDTH = 17,
    parameter DATA_WIDTH = 17  // 3-bit precision: 14 ADC + 3
)(
    input logic clk,
    input logic rstn,

    // Port A — write side (measure_ctrl)
    input logic                    en_a,
    input logic                    we_a,
    input logic [ADDR_WIDTH-1:0]   addr_a,
    input logic [DATA_WIDTH-1:0]   din_a,
    output logic [DATA_WIDTH-1:0]  dout_a,

    // Port B — read side (AXI / PS)
    input logic                    en_b,
    input logic                    we_b,
    input logic [ADDR_WIDTH-1:0]   addr_b,
    input logic [DATA_WIDTH-1:0]   din_b,
    output logic [DATA_WIDTH-1:0]  dout_b
);

    localparam int unsigned RAM_DEPTH = 1 << ADDR_WIDTH;

    // Memory declaration
    logic [DATA_WIDTH-1:0] mem [0:RAM_DEPTH-1];

    // Port A
    always_ff @(posedge clk) begin
        if (!rstn) begin
            dout_a <= '0;
        end else if (en_a) begin
            if (we_a)
                mem[addr_a] <= din_a;
            dout_a <= mem[addr_a]; // 1-cycle read latency
        end else begin
            dout_a <= '0;
        end
    end

    // Port B
    always_ff @(posedge clk) begin
        if (!rstn) begin
            dout_b <= '0;
        end else if (en_b) begin
            if (we_b)
                mem[addr_b] <= din_b;
            dout_b <= mem[addr_b]; // 1-cycle read latency
        end else begin
            dout_b <= '0;
        end
    end

endmodule
