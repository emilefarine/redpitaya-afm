`timescale 1ns / 1ps


module ps_pl_interface_wrapper #(
    // These parameters should match the BRAMs in total_system_wrapper
    parameter GEN_BRAM_ADDR_WIDTH = 16,  // Corresponds to MC_GEN_ADDR_W
    parameter GEN_BRAM_DATA_WIDTH = 17, // Corresponds to GEN_BRAM_DATA_WIDTH in total_system_wrapper
    parameter ACQ_BRAM_ADDR_WIDTH = 16,  // Corresponds to MC_ADDR_WIDTH
    parameter ACQ_BRAM_DATA_WIDTH = 17  // Corresponds to ACQ_BRAM_DATA_WIDTH in total_system_wrapper
) (

    // System bus (connection to PS
    input                 sys_clk_i,    //!< bus clock (e.g., 125 MHz from PL)
    input                 sys_rstn_i,   //!< bus reset - active low
    input        [32-1:0] sys_addr_i,   //!< bus address (from PS)
    input        [32-1:0] sys_wdata_i,  //!< bus write data (from PS)
    input        [ 4-1:0] sys_sel_i,    //!< bus write byte select
    input                 sys_wen_i,    //!< bus write enable (from PS)
    input                 sys_ren_i,    //!< bus read enable (from PS)
    output logic [32-1:0] sys_rdata_o,  //!< bus read data
    output logic          sys_err_o,    //!< bus error indicator
    output logic          sys_ack_o,    //!< bus acknowledge signal

    // BRAMs interfaces for measure_ctrl (connected to A ports)
    input ren_gen_i,  // measure controller will only read this BRAM
    input [GEN_BRAM_ADDR_WIDTH-1:0] addr_gen_i,
    output logic [GEN_BRAM_DATA_WIDTH-1:0] dout_gen_o,

    input                           wen_acq_i,   // measure controller will only write this BRAM
    input [ACQ_BRAM_ADDR_WIDTH-1:0] addr_acq_i,
    input [ACQ_BRAM_DATA_WIDTH-1:0] din_acq_i,

    // Register interface for measure_ctrl (direct connection)
    output logic        start_measure_o,
    output logic [31:0] sig_size_o,
    output logic [31:0] delay_o,          // delay between gen and acq

    output [10:0] decimation_o,    // decimation factor (16-1024)
    input         busy_i,          // high while measure_ctrl FSM is RUNNING
    input         end_measure_i,
    input  [31:0] count_measure_i
);


  // --- addresses of each zone
  localparam BRAM_SHARED_ADDR = 32'h4060_0000;
  localparam REG_BASE_ADDR = 32'h4064_0000;
  localparam REG_ADDR_MASK = 32'hFFFC_0000;  // 14 high bits determine zone (256KB BRAM window)
  localparam SHARED_BRAM_ADDR_WIDTH =
     (GEN_BRAM_ADDR_WIDTH >= ACQ_BRAM_ADDR_WIDTH) ? GEN_BRAM_ADDR_WIDTH : ACQ_BRAM_ADDR_WIDTH;
  localparam SHARED_BRAM_DATA_WIDTH =
     (GEN_BRAM_DATA_WIDTH >= ACQ_BRAM_DATA_WIDTH) ? GEN_BRAM_DATA_WIDTH : ACQ_BRAM_DATA_WIDTH;

  // ============= get selected zone =============
  logic sel_reg, sel_shared_bram;

  assign sel_reg         = ((sys_addr_i & REG_ADDR_MASK) == REG_BASE_ADDR);
  assign sel_shared_bram = ((sys_addr_i & REG_ADDR_MASK) == BRAM_SHARED_ADDR);

  // ============= Control register =============

  // --- Access register from system bus ---
  logic [ 3:0] reg_addr;
  logic [31:0] reg_rdata;
  logic [31:0] reg_wdata;
  logic reg_wen, reg_ren;

  assign reg_addr  = sys_addr_i[5:2];  // ex : 0x4060_0008 -> index 2
  assign reg_wdata = sys_wdata_i;
  assign reg_wen   = sel_reg & sys_wen_i;
  assign reg_ren   = sel_reg & sys_ren_i;


  measure_reg u_reg (
      .clk (sys_clk_i),
      .rstn(sys_rstn_i),

      // --- Access register from system bus ---
      .addr (reg_addr),
      .wdata(reg_wdata),
      .wen  (reg_wen),
      .ren  (reg_ren),
      .rdata(reg_rdata),

      // --- Access register from measure_ctrl ---
      .start_measure(start_measure_o),
      .sig_size(sig_size_o),
      .delay(delay_o),
      .decimation(decimation_o),
      .end_measure(end_measure_i),
      .count_measure(count_measure_i),
      .busy_i(busy_i),                          // STATUS bit[0]: FSM in RUNNING state
      .ps_access_denied_i(sel_shared_bram & (sys_wen_i | sys_ren_i) & busy_i)  // STATUS bit[1]: PS tried BRAM access during busy
  );


  // ============= Shared BRAM =============
  logic [SHARED_BRAM_DATA_WIDTH-1:0] dout_a_shared;
  logic [SHARED_BRAM_DATA_WIDTH-1:0] dout_b_shared;
  logic [SHARED_BRAM_ADDR_WIDTH-1:0] bram_addr_ps;
  logic                              bram_ps_req;
  logic                              bram_port_b_en;
  logic                              bram_port_b_we;
  logic [SHARED_BRAM_ADDR_WIDTH-1:0] bram_port_b_addr;
  logic [SHARED_BRAM_DATA_WIDTH-1:0] bram_port_b_din;

  assign bram_addr_ps = sys_addr_i[SHARED_BRAM_ADDR_WIDTH+1:2];
  assign bram_ps_req = sel_shared_bram & (sys_wen_i | sys_ren_i);

  // Port B arbitration:
  // - busy_i=1 -> measure_ctrl acquisition writes (PS BRAM access denied)
  // - busy_i=0 -> PS read/write shared BRAM
  assign bram_port_b_en = busy_i ? wen_acq_i : bram_ps_req;
  assign bram_port_b_we = busy_i ? wen_acq_i : (sel_shared_bram & sys_wen_i);
  assign bram_port_b_addr = busy_i ? SHARED_BRAM_ADDR_WIDTH'(addr_acq_i) : bram_addr_ps;
  assign bram_port_b_din  = busy_i ? SHARED_BRAM_DATA_WIDTH'(din_acq_i)
                                    : SHARED_BRAM_DATA_WIDTH'(sys_wdata_i);

  bram_custom #(
      .ADDR_WIDTH(SHARED_BRAM_ADDR_WIDTH),
      .DATA_WIDTH(SHARED_BRAM_DATA_WIDTH)
  ) bram_shared (
      .clk   (sys_clk_i),
      .rstn  (sys_rstn_i),
      .en_a  (ren_gen_i),
      .we_a  (1'b0),
      .addr_a(SHARED_BRAM_ADDR_WIDTH'(addr_gen_i)),
      .din_a ('0),
      .dout_a(dout_a_shared),
      .en_b  (bram_port_b_en),
      .we_b  (bram_port_b_we),
      .addr_b(bram_port_b_addr),
      .din_b (bram_port_b_din),
      .dout_b(dout_b_shared)
  );

  assign dout_gen_o = dout_a_shared[GEN_BRAM_DATA_WIDTH-1:0];


  // ============= Read latency handling for BRAMs =============
  // (to synchronize all response signals)

  // This logic ensures a 1-cycle latency for all reads, consistent with red_pitaya_hk.
  logic sys_ren_d1;  // Registered read enable for any read
  logic sel_reg_d1;  // Registered selection for REG_BASE_ADDR
  logic sel_shared_bram_d1;  // Registered selection for BRAM_SHARED_ADDR
  logic bram_ps_denied_rd_d1;

  // sys_rdata_o is already declared as output reg, no need for sys_rdata_d1 if assigning directly
  // to sys_rdata_o in the same always block.

  always_ff @(posedge sys_clk_i or negedge sys_rstn_i) begin
    if (!sys_rstn_i) begin
      sys_ren_d1           <= 1'b0;
      sel_reg_d1           <= 1'b0;
      sel_shared_bram_d1   <= 1'b0;
      bram_ps_denied_rd_d1 <= 1'b0;
      sys_rdata_o          <= 32'h0;  // Initialize sys_rdata_o on reset
    end else begin
      // Register the read request and selection for the next cycle
      sys_ren_d1           <= sys_ren_i;
      sel_reg_d1           <= sel_reg;
      sel_shared_bram_d1   <= sel_shared_bram;
      bram_ps_denied_rd_d1 <= sel_shared_bram & sys_ren_i & busy_i;

      // Capture the data for the next cycle based on current cycle's selection
      // For registers, reg_rdata is combinational, so it's valid immediately.
      // For BRAMs, dout_b_shared is valid after 1 cycle.
      // By registering them here, they will all be available in sys_rdata_o at the same time (1 cycle latency).
      if (sys_ren_d1) begin  // This condition should apply to the _delayed_ read enable
        if (sel_reg_d1) begin
          sys_rdata_o <= reg_rdata;
        end else if (sel_shared_bram_d1) begin
          if (bram_ps_denied_rd_d1) sys_rdata_o <= 32'h0;
          else sys_rdata_o <= {{(32 - SHARED_BRAM_DATA_WIDTH) {1'b0}}, dout_b_shared};
        end else begin
          sys_rdata_o <= 32'h0;  // Default or previous value if no valid read
        end
      end else if (!sys_wen_i && !sys_ren_i) begin  // Only update if no active transaction
        sys_rdata_o <= 32'h0;  // Clear rdata if no read/write is active
      end
    end
  end

  // ============= Outputs =============

  // sys_ack_o and sys_err_o are sequential outputs
  always_ff @(posedge sys_clk_i or negedge sys_rstn_i) begin
    if (!sys_rstn_i) begin
      sys_err_o <= 1'b0;
      sys_ack_o <= 1'b0;
    end else begin
      // Raise bus error when PS tries to access shared BRAM while FSM is busy.
      sys_err_o <= (sel_shared_bram & sys_wen_i & busy_i) | bram_ps_denied_rd_d1;

      // Acknowledge on the cycle AFTER sys_wen_i or sys_ren_i was active for any peripheral.
      // sys_ack_o should be asserted for one cycle when a transaction completes.
      // For writes, it's typically asserted in the cycle after sys_wen_i.
      // For reads, it's asserted in the cycle after sys_ren_i (when rdata is valid).
      if (sys_wen_i) begin  // Acknowledge for write (1 cycle latency)
        sys_ack_o <= 1'b1;
      end else if (sys_ren_d1) begin  // Acknowledge for ANY read (register or BRAM) after 1 cycle
        sys_ack_o <= 1'b1;
      end else begin
        sys_ack_o <= 1'b0;  // No active transaction
      end
    end
  end
endmodule
