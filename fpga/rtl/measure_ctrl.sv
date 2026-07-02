`timescale 1ns / 1ps

module measure_ctrl #(
    parameter DATA_WIDTH = 14,  // DAC / ADC data width
    parameter PREC_BIT = 3,  // NB of precision bit added with averager
    parameter ADDR_WIDTH = 10,
    parameter GEN_ADDR_W = 12,
    parameter PROPAGATION_DELAY = 4  // time between start of RUNNING state and first sample on DAC
) (
    input logic clk,
    input logic rstn,

    // Interface registre
    input  logic        start_measure,
    input  logic [31:0] sig_size,
    input  logic [31:0] delay,           // delay between gen and acq
    input  logic [10:0] decimation_reg,  // Runtime decimation value from AXI register
    output logic        end_measure,
    output logic [31:0] count_measure,   // Now directly from averager
    output logic        busy_o,

    // Interface ADC
    input logic signed [DATA_WIDTH-1:0] adc_data,

    // Interface DAC
    output logic signed [DATA_WIDTH-1:0] gen_out,

    // Interface BRAM pour DAC (Generation) - 14-bit data from BRAM (DAC width)
    input  logic signed [DATA_WIDTH-1:0] bram_out,
    output logic                         bram_en,   // read from generation bBRAM
    output logic        [GEN_ADDR_W-1:0] bram_addr,

    // Interface BRAM pour ADC (Acquisition) - Provides 17-bit data to BRAM_CUSTOM
    output logic        [             ADDR_WIDTH-1:0] bram_in_addr,
    output logic signed [DATA_WIDTH + PREC_BIT - 1:0] bram_in_data,
    output logic                                      bram_we        // write to acquistion BRAM
);

  // === FSM ===
  typedef enum logic [1:0] {
    IDLE,
    RUNNING,
    FINISHED
  } state_t;

  state_t state, next_state;

  // === Compteurs ===
  logic [          31:0] internal_count;
  logic [          31:0] delay_counter;
  logic                  acq_enabled;
  logic                  gen_enabled;

  // Internal signal for averaged sample count from averager
  logic [ADDR_WIDTH-1:0] averaged_samples_from_averager;


  // === Generation (starts at RUNNING) ===
  decimator #(
      .ADDR_WIDTH(GEN_ADDR_W),
      .DATA_WIDTH(DATA_WIDTH),  // gen BRAM 14-bit
      .IO_WIDTH  (DATA_WIDTH)   // DAC output width (14 bits)
  ) u_decimator (
      .clk             (clk),
      .rstn            (rstn),
      .enabled         (gen_enabled),
      .decimation_value(decimation_reg),  // Use runtime decimation from AXI register
      .bram_out        (bram_out),        // 14-bit generation BRAM data
      .bram_en         (bram_en),
      .bram_addr       (bram_addr),
      .gen_out         (gen_out)
  );

  // === Acquisition (starts at RUNNING + delay) ===
  logic signed [DATA_WIDTH-1:0] adc_data_masked;

  assign adc_data_masked = acq_enabled ? adc_data : '0;

  averager #(
      .DATA_WIDTH(DATA_WIDTH),
      .ADDR_WIDTH(ADDR_WIDTH),
      .PREC_BIT  (PREC_BIT)
  ) u_averager (
      .clk                  (clk),
      .reset_n              (rstn),
      .enabled              (acq_enabled),
      .decimation_value     (decimation_reg),
      .adc_data             (adc_data_masked),
      .bram_addr            (bram_in_addr),
      .bram_in              (bram_in_data),
      .bram_we              (bram_we),
      .averaged_sample_count(averaged_samples_from_averager)
  );

  // Connect the averaged sample count to the output port
  assign count_measure = averaged_samples_from_averager;
  assign busy_o = (state == RUNNING);

  // Single-BRAM mode safety: never allow zero-sample delay.
  logic [31:0] delay_eff;
  assign delay_eff = (delay == 32'd0) ? 32'd1 : delay;

  localparam GEN_START_OFFSET = PROPAGATION_DELAY - 2;
  localparam ACQ_START_OFFSET = PROPAGATION_DELAY - 1;

  // === Pre-computed measurement thresholds (in clock ticks) ===
  // sig_size, delay and decimation are constant during a measurement (the PS
  // sets them before start_measure), so the products sig_size*decimation and
  // delay*decimation are constants too.  Computing them combinationally inside
  // the per-cycle FSM compares produced a long multiply -> 32-bit compare carry
  // path (2x DSP48 + 13x CARRY4 = 15.7 ns) that failed timing at 125 MHz.
  // Pre-compute the products in a 2-stage pipeline so the FSM only does
  // counter-vs-threshold compares.  The 2-cycle latency is irrelevant: inputs
  // settle many cycles before start_measure is asserted.
  logic [31:0] prod_sig;       // sig_size  * decimation_reg  (stage 1)
  logic [31:0] prod_dly;       // delay_eff * decimation_reg  (stage 1)
  logic [31:0] thr_acq_start;  // delay_counter target to enable acquisition
  logic [31:0] thr_gen_end;    // internal_count target to stop generation
  logic [31:0] thr_meas_end;   // internal_count target to end the whole measure

  // sig_size and delay are bounded by the shared-BRAM depth (<= 65536 samples,
  // enforced in software by MAX_SAMPLES), so 18 bits is more than enough.
  // Narrowing the wide multiply operand keeps each product in a SINGLE DSP48E1
  // (25x18) instead of a 2-DSP cascade (PCOUT->PCIN), which was the critical path.
  logic [17:0] sig_size_m;
  logic [17:0] delay_eff_m;
  assign sig_size_m  = sig_size[17:0];
  assign delay_eff_m = delay_eff[17:0];

  always_ff @(posedge clk or negedge rstn) begin
    if (!rstn) begin
      prod_sig      <= 32'd0;
      prod_dly      <= 32'd0;
      thr_acq_start <= 32'd0;
      thr_gen_end   <= 32'd0;
      thr_meas_end  <= 32'd0;
    end else begin
      // Stage 1: products (single DSP48 each, with output register)
      prod_sig <= sig_size_m  * decimation_reg;
      prod_dly <= delay_eff_m * decimation_reg;
      // Stage 2: add fixed offsets (reuse the two products; no 3rd multiply)
      thr_acq_start <= prod_dly + (PROPAGATION_DELAY - ACQ_START_OFFSET);
      thr_gen_end   <= prod_sig + (PROPAGATION_DELAY - GEN_START_OFFSET);
      thr_meas_end  <= prod_sig + prod_dly + PROPAGATION_DELAY;
    end
  end

  // === FSM State register ===
  always_ff @(posedge clk or negedge rstn) begin
    if (!rstn) begin
      state          <= IDLE;
      internal_count <= 0;
      delay_counter  <= 0;
      end_measure    <= 0;
      acq_enabled    <= 0;
      gen_enabled    <= 0;

    end else begin
      state <= next_state;

      if (state == IDLE) begin
        internal_count <= 0;
        delay_counter  <= 0;
        acq_enabled    <= 0;
        gen_enabled    <= 0;
        end_measure    <= 0;
      end

      // === Manage generation & acquisition ===
      if (state == RUNNING) begin

        gen_enabled <= 1;  // Default: generation immediately enabled at start_measure

        // --- Manage acquisition start ---
        if (!acq_enabled) begin

          // increment delay counter
          if (delay_counter < thr_acq_start) begin
            delay_counter <= delay_counter + 1;

            // if delay passed, activate acquisition
          end else begin
            acq_enabled   <= 1;
            delay_counter <= (delay_eff - 1);
          end
        end

        // Increment samples counter
        internal_count <= internal_count + 1;

        // end generation (sig_size defines total samples for generation)
        if (internal_count >= thr_gen_end) begin
          gen_enabled <= 0;
        end

        // End of full measure (gen + delay + acq)
        if (internal_count >= thr_meas_end) begin
          gen_enabled <= 0;
          acq_enabled <= 0;
          end_measure <= 1;  // signals to PS that the measure ended
        end
      end

      // === Reset counters ===
      if (state == FINISHED) begin  // wait for PS to acknowlege (start = 0)

        internal_count <= 0;    // reset counters
        delay_counter  <= 0;    // reset counters
        acq_enabled    <= 0;    // disable acquisiton (to not overwrite acquired samples)
        gen_enabled    <= 0;    // disable generation (send zeros on DAC)

        if (start_measure)  // If PS still asserts start_measure, keep end_measure high
          end_measure <= 1;
        else end_measure <= 0;  // PS has acknowledged, clear end_measure

      end
    end
  end

  // === FSM Transitions ===
  always_comb begin
    next_state = state;
    case (state)
      IDLE:
      if (start_measure) begin  // PS asked for start
        next_state = RUNNING;
      end
      RUNNING:
      if (internal_count >= thr_meas_end)  // gen and acq finished
        next_state = FINISHED;

      FINISHED:
      if (!start_measure)  // acknowledged by PS (start_measure becomes 0)
        next_state = IDLE;

      default: next_state = IDLE;
    endcase
  end

endmodule
