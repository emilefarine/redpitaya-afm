////////////////////////////////////////////////////////////////////////////////
// Red Pitaya TOP module. It connects external pins and PS part with
// other application modules.
// Authors: Matej Oblak, Iztok Jeras
// (c) Red Pitaya  http://www.redpitaya.com
////////////////////////////////////////////////////////////////////////////////

// https://redpitaya.readthedocs.io/en/latest/developerGuide/software/build/fpga/regset/2.00-30/v0.94.html#housekeeping
// génération : decimation par 4

/**
 * GENERAL DESCRIPTION:
 *
 * Top module connects PS part with rest of Red Pitaya applications.  
 *
 *
 *
 * --- ORIGINAL VERSION (official v0.94)---
 *
 *                   /-------\      
 *   PS DDR <------> |  PS   |      AXI <-> custom bus
 *   PS MIO <------> |   /   | <------------+
 *   PS CLK -------> |  ARM  |              |
 *                   \-------/              |
 *                                          |
 *                            /-------\     |
 *                         -> | SCOPE | <---+
 *                         |  \-------/     |
 *                         |                |
 *            /--------\   |   /-----\      |
 *   ADC ---> |        | --+-> |     |      |
 *            | ANALOG |       | PID | <----+
 *   DAC <--- |        | <---- |     |      |
 *            \--------/   ^   \-----/      |
 *                         |                |
 *                         |  /-------\     |
 *                         -- |  ASG  | <---+ 
 *                            \-------/     |
 *                                          |
 *             /--------\                   |
 *    RX ----> |        |                   |
 *   SATA      | DAISY  | <-----------------+
 *    TX <---- |        | 
 *             \--------/ 
 *               |    |
 *               |    |
 *               (FREE)
 *
 *
 *
 *
 * --- NEW VERSION (custom 1.0) ---
 *
 *                   /-------\      
 *   PS DDR <------> |  PS   |      AXI <-> custom bus
 *   PS MIO <------> |   /   | <------------+
 *   PS CLK -------> |  ARM  |              |
 *                   \-------/              |
 *                                          |
 *                            /-------\     |
 *                         -> |AVERAGE| <---+ (new module)
 *                         |  \-------/     |
 *                         |                |
 *            /--------\   |   /-------\    |
 *   ADC ---> |        | --+-> |       |    |
 *            | ANALOG |       |measure| <--+ (new module)
 *   DAC <--- |        | <---- | ctrl  |    |
 *            \--------/   ^   \-------/    |
 *                         |                |
 *                         |  /-------\     |
 *                         -- |DECIMAT| <---+ (new module)
 *                            \-------/     
 *  
 *
 * Inside analog module, ADC data is translated from unsigned neg-slope into
 * two's complement. Similar is done on DAC data.
 *
 * Scope module stores data from ADC into RAM, arbitrary signal generator (ASG)
 * sends data from RAM to DAC. MIMO PID uses ADC ADC as input and DAC as its output.
 *
 * Daisy chain connects with other boards with fast serial link. Data which is
 * send and received is at the moment undefined. This is left for the user.
 */

////////////////////////////////////////////////////////////////////////////////
// Project      : 
// Author       : 
// Date         : 
// Description  : Red Pitaya TOP module. Connects external pins and PS part with 
// other application modules
// Original version authors: Matej Oblak, Iztok Jeras - http://www.redpitaya.com
////////////////////////////////////////////////////////////////////////////////

/**
 * GENERAL DESCRIPTION:
 *
 * Top module connects PS part with rest of Red Pitaya applications.
 *
 *                   /-------\      
 *   PS DDR <------> |  PS   |      AXI <-> custom bus
 *   PS MIO <------> |   /   | <---------------+
 *   PS CLK -------> |  ARM  |                 |
 *                   \-------/                 |
 *                                             |
 *                            /-------\        |
 *                         -> | AVERAGER | <---+
 *                         |  \-------/        |
 *                         |                   |
 *            /--------\   |   /-----\         | 
 *   ADC ---> |        | --+-> |     |         |
 *            | ANALOG |       |MEASURE_CTRL| <----+
 *   DAC <--- |        | <---- |     |         |
 *            \--------/   ^   \-----/      |
 *                         |                |
 *                         |  /-------\     |
 *                         -- |  DECIMATOR  | <---+ 
 *                            \-------/     
 *
 *
 * Inside analog module, ADC data is translated from unsigned neg-slope into
 * two's complement. Similar is done on DAC data.
 *
 * Scope module stores data from ADC into RAM, arbitrary signal generator (ASG)
 * sends data from RAM to DAC. MIMO PID uses ADC ADC as input and DAC as its output.
 *
 * Daisy chain connects with other boards with fast serial link. Data which is
 * send and received is at the moment undefined. This is left for the user.
 */



module red_pitaya_top #(
    // identification
    bit [0:5*32-1] GITH = '0,
    // module numbers
    parameter MNA = 2,  // number of acquisition modules
    parameter MNG = 2,  // number of generator   modules
    parameter DWE_Z10 = 8,
    parameter DWE_Z20 = 11,
`ifdef Z20_14
    parameter DWE = DWE_Z20
`else
    parameter DWE = DWE_Z10
`endif
) (
    // PS connections
    inout logic [54-1:0] FIXED_IO_mio,
    inout logic          FIXED_IO_ps_clk,
    inout logic          FIXED_IO_ps_porb,
    inout logic          FIXED_IO_ps_srstb,
    inout logic          FIXED_IO_ddr_vrn,
    inout logic          FIXED_IO_ddr_vrp,
    // DDR
    inout logic [15-1:0] DDR_addr,
    inout logic [ 3-1:0] DDR_ba,
    inout logic          DDR_cas_n,
    inout logic          DDR_ck_n,
    inout logic          DDR_ck_p,
    inout logic          DDR_cke,
    inout logic          DDR_cs_n,
    inout logic [ 4-1:0] DDR_dm,
    inout logic [32-1:0] DDR_dq,
    inout logic [ 4-1:0] DDR_dqs_n,
    inout logic [ 4-1:0] DDR_dqs_p,
    inout logic          DDR_odt,
    inout logic          DDR_ras_n,
    inout logic          DDR_reset_n,
    inout logic          DDR_we_n,

    // Red Pitaya periphery

    // ADC
    input logic [MNA-1:0][16-1:0] adc_dat_i,  // ADC data
    input logic [2-1:0] adc_clk_i,  // ADC clock {p,n}
    output logic [2-1:0] adc_clk_o,  // optional ADC clock source (unused) [0] = p; [1] = n
    output logic adc_cdcs_o,  // ADC clock duty cycle stabilizer
    // DAC
    output logic [14-1:0] dac_dat_o,  // DAC combined data
    output logic dac_wrt_o,  // DAC write
    output logic dac_sel_o,  // DAC channel select
    output logic dac_clk_o,  // DAC clock
    output logic dac_rst_o,  // DAC reset
    // PWM DAC
    output logic [4-1:0] dac_pwm_o,  // 1-bit PWM DAC (unused in custom design)
    // XADC
    input logic [5-1:0] vinp_i,  // voltages p
    input logic [5-1:0] vinn_i,  // voltages n
    // Expansion connector
    inout logic [DWE-1:0] exp_p_io,
    inout logic [DWE-1:0] exp_n_io,

    // SATA connector
    /*
  output logic [ 2-1:0] daisy_p_o  ,  // line 1 is clock capable
  output logic [ 2-1:0] daisy_n_o  ,
  input  logic [ 2-1:0] daisy_p_i  ,  // line 1 is clock capable
  input  logic [ 2-1:0] daisy_n_i  ,
  */
    // LED
    inout logic [8-1:0] led_o
);

  ////////////////////////////////////////////////////////////////////////////////
  // local signals
  ////////////////////////////////////////////////////////////////////////////////

  // GPIO input data width


  localparam int unsigned GDW = DWE;

  logic [ 4-1:0] fclk;  //[0]-125MHz, [1]-250MHz, [2]-50MHz, [3]-200MHz
  logic [ 4-1:0] frstn;

  logic [16-1:0] par_dat;

  //logic          daisy_trig;
  //logic [ 3-1:0] daisy_mode;
  logic          trig_ext;
  logic          trig_output_sel;
  logic          trig_asg_out;
  logic [ 4-1:0] trig_ext_asg01;

  // AXI masters
  logic axi1_clk, axi0_clk;
  logic axi1_rstn, axi0_rstn;
  logic [32-1:0] axi1_waddr, axi0_waddr;
  logic [64-1:0] axi1_wdata, axi0_wdata;
  logic [8-1:0] axi1_wsel, axi0_wsel;
  logic axi1_wvalid, axi0_wvalid;
  logic [4-1:0] axi1_wlen, axi0_wlen;
  logic axi1_wfixed, axi0_wfixed;
  logic axi1_werr, axi0_werr;
  logic axi1_wrdy, axi0_wrdy;

  // PLL signals
  logic adc_clk_in;
  logic pll_adc_clk;
  logic pll_dac_clk_1x;
  logic pll_dac_clk_2x;
  logic pll_dac_clk_2p;
  logic pll_ser_clk;
  logic pll_pwm_clk;
  logic pll_locked;
  // fast serial signals
  logic ser_clk;
  // PWM clock and reset
  logic pwm_clk;
  logic pwm_rstn;

  // ADC clock/reset
  logic adc_clk;
  logic adc_rstn;
  logic adc_clk_daisy;
  //logic                 scope_trigo;

  //CAN
  logic CAN0_rx, CAN0_tx;
  logic CAN1_rx, CAN1_tx;
  logic can_on;


  // stream bus type
  localparam type SBA_T = logic signed [14-1:0];  // acquire
  localparam type SBG_T = logic signed [14-1:0];  // generate

  SBA_T [MNA-1:0] adc_dat;

  // DAC signals
  logic           dac_clk_1x;
  logic           dac_clk_2x;
  logic           dac_clk_2p;
  logic           dac_rst;

  logic [14-1:0] dac_dat_a, dac_dat_b;
  logic [14-1:0] dac_a, dac_b;
  logic signed [15-1:0] dac_a_sum, dac_b_sum;

  // ASG
  SBG_T [2-1:0] asg_dat;

  // PID
  //SBA_T [2-1:0]            pid_dat;

  // configuration
  logic         digital_loop;

  // system bus
  sys_bus_if ps_sys (
      .clk (adc_clk),
      .rstn(adc_rstn)
  );
  sys_bus_if sys[8-1:0] (
      .clk (adc_clk),
      .rstn(adc_rstn)
  );

  // GPIO interface
  gpio_if #(.DW(3 * GDW)) gpio ();

  ////////////////////////////////////////////////////////////////////////////////
  // PLL (clock and reset)
  ////////////////////////////////////////////////////////////////////////////////

  // diferential clock input
  IBUFDS i_clk (
      .I (adc_clk_i[1]),
      .IB(adc_clk_i[0]),
      .O (adc_clk_in)
  );  // differential clock input

  red_pitaya_pll pll (
      // inputs
      .clk       (adc_clk_in),      // clock
      .rstn      (frstn[0]),        // reset - active low
      // output clocks
      .clk_adc   (pll_adc_clk),     // ADC clock
      .clk_dac_1x(pll_dac_clk_1x),  // DAC clock 125MHz
      .clk_dac_2x(pll_dac_clk_2x),  // DAC clock 250MHz
      .clk_dac_2p(pll_dac_clk_2p),  // DAC clock 250MHz -45DGR
      .clk_ser   (pll_ser_clk),     // fast serial clock
      .clk_pdm   (pll_pwm_clk),     // PWM clock
      // status outputs
      .pll_locked(pll_locked)
  );

  BUFG bufg_adc_clk (
      .O(adc_clk),
      .I(pll_adc_clk)
  );
  BUFG bufg_dac_clk_1x (
      .O(dac_clk_1x),
      .I(pll_dac_clk_1x)
  );
  BUFG bufg_dac_clk_2x (
      .O(dac_clk_2x),
      .I(pll_dac_clk_2x)
  );
  BUFG bufg_dac_clk_2p (
      .O(dac_clk_2p),
      .I(pll_dac_clk_2p)
  );
  BUFG bufg_ser_clk (
      .O(ser_clk),
      .I(pll_ser_clk)
  );
  BUFG bufg_pwm_clk (
      .O(pwm_clk),
      .I(pll_pwm_clk)
  );

  logic [32-1:0] locked_pll_cnt, locked_pll_cnt_r, locked_pll_cnt_r2;
  always @(posedge fclk[0]) begin
    if (~frstn[0]) locked_pll_cnt <= 'h0;
    else if (~pll_locked) locked_pll_cnt <= locked_pll_cnt + 'h1;
  end

  always @(posedge adc_clk) begin
    locked_pll_cnt_r  <= locked_pll_cnt;
    locked_pll_cnt_r2 <= locked_pll_cnt_r;
  end

  // ADC reset (active low)
  always @(posedge adc_clk) adc_rstn <= frstn[0] & pll_locked;

  // DAC reset (active high)
  always @(posedge dac_clk_1x) dac_rst <= ~frstn[0] | ~pll_locked;

  // PWM reset (active low)
  always @(posedge pwm_clk) pwm_rstn <= frstn[0] & pll_locked;


  assign trig_ext = gpio.i[GDW];
  ////////////////////////////////////////////////////////////////////////////////
  //  Connections to PS
  ////////////////////////////////////////////////////////////////////////////////

  red_pitaya_ps ps (
      .FIXED_IO_mio     (FIXED_IO_mio),
      .FIXED_IO_ps_clk  (FIXED_IO_ps_clk),
      .FIXED_IO_ps_porb (FIXED_IO_ps_porb),
      .FIXED_IO_ps_srstb(FIXED_IO_ps_srstb),
      .FIXED_IO_ddr_vrn (FIXED_IO_ddr_vrn),
      .FIXED_IO_ddr_vrp (FIXED_IO_ddr_vrp),
      // DDR
      .DDR_addr         (DDR_addr),
      .DDR_ba           (DDR_ba),
      .DDR_cas_n        (DDR_cas_n),
      .DDR_ck_n         (DDR_ck_n),
      .DDR_ck_p         (DDR_ck_p),
      .DDR_cke          (DDR_cke),
      .DDR_cs_n         (DDR_cs_n),
      .DDR_dm           (DDR_dm),
      .DDR_dq           (DDR_dq),
      .DDR_dqs_n        (DDR_dqs_n),
      .DDR_dqs_p        (DDR_dqs_p),
      .DDR_odt          (DDR_odt),
      .DDR_ras_n        (DDR_ras_n),
      .DDR_reset_n      (DDR_reset_n),
      .DDR_we_n         (DDR_we_n),
      // system signals
      .fclk_clk_o       (fclk),
      .fclk_rstn_o      (frstn),
      // ADC analog inputs
      .vinp_i           (vinp_i),
      .vinn_i           (vinn_i),
      // CAN0
      .CAN0_rx          (CAN0_rx),
      .CAN0_tx          (CAN0_tx),
      // CAN1
      .CAN1_rx          (CAN1_rx),
      .CAN1_tx          (CAN1_tx),
      // GPIO
      .gpio             (gpio),
      // system read/write channel
      .bus              (ps_sys),
      // AXI masters
      .axi1_clk_i       (axi1_clk),
      .axi0_clk_i       (axi0_clk),           // global clock
      .axi1_rstn_i      (axi1_rstn),
      .axi0_rstn_i      (axi0_rstn),          // global reset
      .axi1_waddr_i     (axi1_waddr),
      .axi0_waddr_i     (axi0_waddr),         // system write address
      .axi1_wdata_i     (axi1_wdata),
      .axi0_wdata_i     (axi0_wdata),         // system write data
      .axi1_wsel_i      (axi1_wsel),
      .axi0_wsel_i      (axi0_wsel),          // system write byte select
      .axi1_wvalid_i    (axi1_wvalid),
      .axi0_wvalid_i    (axi0_wvalid),        // system write data valid
      .axi1_wlen_i      (axi1_wlen),
      .axi0_wlen_i      (axi0_wlen),          // system write burst length
      .axi1_wfixed_i    (axi1_wfixed),
      .axi0_wfixed_i    (axi0_wfixed),        // system write burst type (fixed / incremental)
      .axi1_werr_o      (axi1_werr),
      .axi0_werr_o      (axi0_werr),          // system write error
      .axi1_wrdy_o      (axi1_wrdy),
      .axi0_wrdy_o      (axi0_wrdy)           // system write ready
  );

  ////////////////////////////////////////////////////////////////////////////////
  // system bus decoder & multiplexer (it breaks memory addresses into 8 regions)
  ////////////////////////////////////////////////////////////////////////////////

  sys_bus_interconnect #(
      .SN(8),
      .SW(20)
  ) sys_bus_interconnect (
      .bus_m(ps_sys),
      .bus_s(sys)
  );

  // Stub unused system bus slots (only sys[0] = HK, sys[6] = custom PS/PL interface are used)
  generate
    for (genvar i = 1; i < 6; i++) begin : stub_unused_sys
      sys_bus_stub sys_bus_stub_unused (sys[i]);
    end : stub_unused_sys
  endgenerate
  sys_bus_stub sys_bus_stub_7 (sys[7]);


  ////////////////////////////////////////////////////////////////////////////////
  // ADC IO
  ////////////////////////////////////////////////////////////////////////////////

  ODDR i_adc_clk_p (
      .Q (adc_clk_o[0]),
      .D1(1'b1),
      .D2(1'b0),
      .C (adc_clk_daisy),
      .CE(1'b1),
      .R (1'b0),
      .S (1'b0)
  );
  ODDR i_adc_clk_n (
      .Q (adc_clk_o[1]),
      .D1(1'b0),
      .D2(1'b1),
      .C (adc_clk_daisy),
      .CE(1'b1),
      .R (1'b0),
      .S (1'b0)
  );

  assign adc_cdcs_o = 1'b1;

  logic [2-1:0][14-1:0] adc_dat_raw;

  // IO block registers should be used here
  // lowest 2 bits reserved for 16bit ADC

  assign adc_dat_raw[0] = adc_dat_i[0][16-1:2];
  assign adc_dat_raw[1] = adc_dat_i[1][16-1:2];

  // transform into 2's complement (negative slope)
  always @(posedge adc_clk) begin
    adc_dat[0] <= digital_loop ? dac_a : {adc_dat_raw[0][14-1], ~adc_dat_raw[0][14-2:0]};
    adc_dat[1] <= digital_loop ? dac_b : {adc_dat_raw[1][14-1], ~adc_dat_raw[1][14-2:0]};
  end


  ////////////////////////////////////////////////////////////////////////////////
  // DAC IO (UPDATED - PID AND ASG REMOVED)
  ////////////////////////////////////////////////////////////////////////////////

  logic [13:0] data_for_a;
  logic [13:0] data_for_b;

  assign data_for_b = '0;  // DAC channel B unused
  assign dac_pwm_o  = '0;  // PWM DAC unused in custom design

  // Generate a new sample at each cycle
  always @(posedge dac_clk_1x) begin
    // Conversion signed→unsigned Red Pitaya style (bit de signe conservé, bits restants inversés)
    dac_dat_a <= {data_for_a[13], ~data_for_a[12:0]};
    dac_dat_b <= {data_for_b[13], ~data_for_b[12:0]};
  end

  // DDR outputs (cette primitive envoie une donnée sur le front montant (D1) et une autre sur le front descendant (D2) de l'horloge)
  ODDR oddr_dac_clk (
      .Q (dac_clk_o),
      .D1(1'b0),
      .D2(1'b1),
      .C (dac_clk_2p),
      .CE(1'b1),
      .R (1'b0),
      .S (1'b0)
  );
  ODDR oddr_dac_wrt (
      .Q (dac_wrt_o),
      .D1(1'b0),
      .D2(1'b1),
      .C (dac_clk_2x),
      .CE(1'b1),
      .R (1'b0),
      .S (1'b0)
  );
  ODDR oddr_dac_sel (
      .Q (dac_sel_o),
      .D1(1'b1),
      .D2(1'b0),
      .C (dac_clk_1x),
      .CE(1'b1),
      .R (dac_rst),
      .S (1'b0)
  );
  ODDR oddr_dac_rst (
      .Q (dac_rst_o),
      .D1(dac_rst),
      .D2(dac_rst),
      .C (dac_clk_1x),
      .CE(1'b1),
      .R (1'b0),
      .S (1'b0)
  );
  ODDR oddr_dac_dat[14-1:0] (
      .Q (dac_dat_o),
      .D1(dac_dat_b),
      .D2(dac_dat_a),
      .C (dac_clk_1x),
      .CE(1'b1),
      .R (dac_rst),
      .S (1'b0)
  );


  ////////////////////////////////////////////////////////////////////////////////
  //  House Keeping
  ////////////////////////////////////////////////////////////////////////////////

  logic [DWE-1:0] exp_p_in, exp_n_in;
  logic [DWE-1:0] exp_p_out, exp_n_out;
  logic [DWE-1:0] exp_p_dir, exp_n_dir;
  logic [DWE-1:0] exp_p_otr, exp_n_otr;
  logic [DWE-1:0] exp_p_dtr, exp_n_dtr;
  logic [DWE-1:0] exp_p_alt, exp_n_alt;
  logic [DWE-1:0] exp_p_altr, exp_n_altr;
  logic [DWE-1:0] exp_p_altd, exp_n_altd;

  red_pitaya_hk #(
      .DWE(DWE)
  ) i_hk (
      // system signals
      .clk_i       (adc_clk),            // clock
      .rstn_i      (adc_rstn),           // reset - active low
      // LED
      .led_o       (led_o[8-1:0]),       // LED output todo : back to normal
      // global configuration
      .digital_loop(digital_loop),
      .daisy_mode_o(),                   // left unconnected, daisy chain unused
      // Expansion connector
      .exp_p_dat_i (exp_p_in),           // input data
      .exp_p_dat_o (exp_p_out),          // output data
      .exp_p_dir_o (exp_p_dir),          // 1-output enable
      .exp_n_dat_i (exp_n_in),
      .exp_n_dat_o (exp_n_out),
      .exp_n_dir_o (exp_n_dir),
      .diag_i      (locked_pll_cnt_r2),
      .can_on_o    (can_on),
      // System bus
      .sys_addr    (sys[0].addr),
      .sys_wdata   (sys[0].wdata),
      .sys_wen     (sys[0].wen),
      .sys_ren     (sys[0].ren),
      .sys_rdata   (sys[0].rdata),
      .sys_err     (sys[0].err),
      .sys_ack     (sys[0].ack)
  );

  ////////////////////////////////////////////////////////////////////////////////
  // LED
  ////////////////////////////////////////////////////////////////////////////////

  ////////////////////////////////////////////////////////////////////////////////
  // GPIO
  ////////////////////////////////////////////////////////////////////////////////

  assign exp_p_alt  = {DWE{1'b0}};
  //assign exp_n_alt  = {{DWE-8{1'b0}},  can_on,  can_on, 5'h0, daisy_mode[1]  };

  assign exp_p_altr = {DWE{1'b0}};
  assign exp_n_altr = {{DWE - 8{1'b0}}, CAN0_tx, CAN1_tx, 5'h0, trig_output_sel};

  assign exp_p_altd = {DWE{1'b0}};
  assign exp_n_altd = {{DWE - 8{1'b0}}, 1'b1, 1'b1, 5'h0, 1'b1};

  genvar GM;
  generate
    for (GM = 0; GM < DWE; GM = GM + 1) begin : gpios
      assign exp_p_otr[GM] = exp_p_alt[GM] ? exp_p_altr[GM] : exp_p_out[GM];
      assign exp_n_otr[GM] = exp_n_alt[GM] ? exp_n_altr[GM] : exp_n_out[GM];

      assign exp_p_dtr[GM] = exp_p_alt[GM] ? exp_p_altd[GM] : exp_p_dir[GM];
      assign exp_n_dtr[GM] = exp_n_alt[GM] ? exp_n_altd[GM] : exp_n_dir[GM];
    end
  endgenerate

  IOBUF i_iobufp[DWE-1:0] (
      .O (exp_p_in),
      .IO(exp_p_io),
      .I (exp_p_otr),
      .T (~exp_p_dtr)
  );
  IOBUF i_iobufn[DWE-1:0] (
      .O (exp_n_in),
      .IO(exp_n_io),
      .I (exp_n_otr),
      .T (~exp_n_dtr)
  );

  assign gpio.i[2*GDW-1:GDW] = exp_p_in[GDW-1:0];
  assign gpio.i[3*GDW-1:2*GDW] = exp_n_in[GDW-1:0];

  assign CAN0_rx = can_on & exp_p_in[7];
  assign CAN1_rx = can_on & exp_p_in[6];


  ////////////////////////////////////////////////////////////////////////////////
  // PL/PS interface instaciation (brams and register) (NEW)
  ////////////////////////////////////////////////////////////////////////////////

  // Parameters for BRAM generation
  localparam GEN_BRAM_DATA_WIDTH = 14;  // DAC is 14-bit, no extra precision needed
  localparam ACQ_BRAM_DATA_WIDTH = 17;  // 14 ADC + 3 precision bits
  localparam GEN_BRAM_ADDR_WIDTH = 16;  // 2^16 = 65536 samples (shared BRAM depth)
  localparam ACQ_BRAM_ADDR_WIDTH = 16;

  // Signals between PS/PL interface and measure logic (measure_ctrl module)
  logic                            gen_bram_ren_sig;
  logic [ GEN_BRAM_ADDR_WIDTH-1:0] gen_bram_addr_sig;
  logic [GEN_BRAM_DATA_WIDTH -1:0] gen_bram_data_out_sig;
  logic                            acq_bram_wen_sig;
  logic [ ACQ_BRAM_ADDR_WIDTH-1:0] acq_bram_addr_sig;
  logic [ ACQ_BRAM_DATA_WIDTH-1:0] acq_bram_data_in_sig;
  logic                            start_measure_sig;
  logic [                    31:0] sig_size_sig;
  logic [                    31:0] delay_sig;
  logic [                    10:0] decimation_reg_sig;
  logic                            end_measure_sig;
  logic [                    31:0] count_measure_sig;
  logic                            measure_busy_sig;

  ps_pl_interface_wrapper #(
      .GEN_BRAM_ADDR_WIDTH(GEN_BRAM_ADDR_WIDTH),
      .GEN_BRAM_DATA_WIDTH(GEN_BRAM_DATA_WIDTH),
      .ACQ_BRAM_ADDR_WIDTH(ACQ_BRAM_ADDR_WIDTH),
      .ACQ_BRAM_DATA_WIDTH(ACQ_BRAM_DATA_WIDTH)
  ) u_ps_pl_interface (
      .sys_clk_i  (adc_clk),       // clock
      .sys_rstn_i (adc_rstn),      // reset - active low
      .sys_addr_i (sys[6].addr),
      .sys_wdata_i(sys[6].wdata),
      .sys_sel_i  (4'b1111),
      .sys_wen_i  (sys[6].wen),
      .sys_ren_i  (sys[6].ren),
      .sys_rdata_o(sys[6].rdata),
      .sys_err_o  (sys[6].err),
      .sys_ack_o  (sys[6].ack),

      // Connexions BRAM vers measure_ctrl
      .ren_gen_i (gen_bram_ren_sig),
      .addr_gen_i(gen_bram_addr_sig),
      .dout_gen_o(gen_bram_data_out_sig), // Sortie de la BRAM de génération (vers measure_ctrl)

      .wen_acq_i (acq_bram_wen_sig),
      .addr_acq_i(acq_bram_addr_sig),
      .din_acq_i (acq_bram_data_in_sig), // Entrée de la BRAM d'acquisition (depuis measure_ctrl)

      // Connexions registre vers measure_ctrl
      .start_measure_o(start_measure_sig),
      .sig_size_o     (sig_size_sig),
      .delay_o        (delay_sig),
      .decimation_o   (decimation_reg_sig),
      .busy_i         (measure_busy_sig),
      .end_measure_i  (end_measure_sig),
      .count_measure_i(count_measure_sig)
  );



  ////////////////////////////////////////////////////////////////////////////////
  // Measure controller instancitation (custom PL logic) (replaces asg) (NEW)
  ////////////////////////////////////////////////////////////////////////////////

  // Instanciation de measure_ctrl
  measure_ctrl #(
      .DATA_WIDTH(14),                   // ADC/DAC data width
      .PREC_BIT  (3),                    // precision bits gained with a decimation of 64
      .ADDR_WIDTH(ACQ_BRAM_ADDR_WIDTH),  // acquisition BRAM address width
      .GEN_ADDR_W(GEN_BRAM_ADDR_WIDTH)   // generation BRAM address width
  ) u_measure_ctrl (
      .clk (adc_clk),
      .rstn(adc_rstn),

      // Register
      .start_measure (start_measure_sig),   // from ps_pl_interface
      .sig_size      (sig_size_sig),        // from ps_pl_interface
      .delay         (delay_sig),           // from ps_pl_interface
      .decimation_reg(decimation_reg_sig),  // from ps_pl_interface
      .end_measure   (end_measure_sig),     // to ps_pl_interface
      .count_measure (count_measure_sig),   // to ps_pl_interface
      .busy_o        (measure_busy_sig),    // to ps_pl_interface busy arbitration

      // Connect logic to generation BRAM in ps_pl_interface
      .bram_out (gen_bram_data_out_sig),  // from BRAM
      .bram_en  (gen_bram_ren_sig),       // to BRAM
      .bram_addr(gen_bram_addr_sig),      // to BRAM

      // Connect logic to acquistion BRAM in ps_pl_interface
      .bram_in_addr(acq_bram_addr_sig),     // to BRAM
      .bram_in_data(acq_bram_data_in_sig),  // to BRAM
      .bram_we     (acq_bram_wen_sig),      // to BRAM    

      // Connect logic ton ADC / DAC
      .adc_data(adc_dat[0]),  // ADC data
      .gen_out (data_for_a)   // DAC data
  );


endmodule : red_pitaya_top

