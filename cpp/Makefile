# Compiler for C++ (native build on Red Pitaya)
CXX = g++

# Red Pitaya paths (native build only)
RP_INCLUDE = /opt/redpitaya/include
RP_LIB     = /opt/redpitaya/lib

# Red Pitaya libraries (native build only)
LDLIBS = -static -lrp-hw-can -lrp -lrp-hw-calib -lrp-hw-profiles \
         -lrp-gpio -lrp-i2c -lrp-hw -lm -lstdc++ -lpthread -li2c -lsocketcan

# Signal processing libraries
SIGNAL_LDLIBS = -lfftw3f -lm -lstdc++

# C++ compiler flags
CXXFLAGS  = -std=c++17 -Wall -O2
CXXFLAGS += -I$(RP_INCLUDE)
CXXFLAGS += -I$(RP_INCLUDE)/api250-12
CXXFLAGS += -ISignalProcessing  # Add SignalProcessing folder to include path
CXXFLAGS += -IHardware          # Add Hardware folder to include path

LDFLAGS = -L$(RP_LIB)

# Signal processing directory
SIGNAL_DIR = SignalProcessing

# Hardware abstraction layer directory
HARDWARE_DIR = Hardware

# Test programs directory
TEST_DIR = Test

# Server directory
SERVER_DIR = Server

# Tools directory
TOOLS_DIR = Tools

# Output directory for compiled binaries
OUT_DIR = out

# C++ programs to build (only production-ready ones)
PRGS = test_all

# Test programs (in Test/ folder)
TEST_PRGS = hardware_loopback_test quartz_sweep_config quartz_sinc_broadband electronic_board_test single_bram_validation_test calibration_loopback_test

# Server program
SERVER_PRG = afm_server

# Tool programs
TOOL_PRGS = oled_ip

# Signal processing library objects (in SignalProcessing folder)
SIGNAL_PROCESSING_OBJS = $(SIGNAL_DIR)/SignalGenerator.o $(SIGNAL_DIR)/FFTProcessor.o $(SIGNAL_DIR)/ResonanceAnalyzer.o $(SIGNAL_DIR)/CalibrationManager.o

# Hardware abstraction layer objects (in Hardware folder)
HARDWARE_OBJS = $(HARDWARE_DIR)/RedPitayaHardware.o $(HARDWARE_DIR)/ElectronicBoardUART.o

# Server objects (in Server folder)
SERVER_OBJS = $(SERVER_DIR)/TCPServer.o $(SERVER_DIR)/CommandHandler.o

# OLED display objects (in Hardware folder)
OLED_OBJS = $(HARDWARE_DIR)/SSD1306.o

OBJS := $(SIGNAL_PROCESSING_OBJS) $(HARDWARE_OBJS)
SRC  := $(patsubst %,%.cpp,$(PRGS))

# Default target
all: $(PRGS)

# Build all test programs
tests: $(TEST_PRGS)

# Build server
server: $(SERVER_PRG)

# Build tools
tools: $(TOOL_PRGS)

# Create output directory
$(OUT_DIR):
	mkdir -p $(OUT_DIR)

# Compile signal processing library objects in SignalProcessing folder
$(SIGNAL_DIR)/%.o: $(SIGNAL_DIR)/%.cpp $(SIGNAL_DIR)/%.h
	$(CXX) -c $< $(CXXFLAGS) -o $@

# Compile hardware abstraction layer objects in Hardware folder
$(HARDWARE_DIR)/%.o: $(HARDWARE_DIR)/%.cpp $(HARDWARE_DIR)/%.h
	$(CXX) -c $< $(CXXFLAGS) -o $@

# Compile server objects in Server folder
$(SERVER_DIR)/%.o: $(SERVER_DIR)/%.cpp $(SERVER_DIR)/%.h
	$(CXX) -c $< $(CXXFLAGS) -IServer -o $@

# Production Programs
# -------------------

# Compile comprehensive signal processing test (main program - now in Test folder)
test_all: $(OUT_DIR) $(TEST_DIR)/test_all.cpp $(SIGNAL_PROCESSING_OBJS)
	$(CXX) $(TEST_DIR)/test_all.cpp $(SIGNAL_PROCESSING_OBJS) $(CXXFLAGS) $(SIGNAL_LDLIBS) -o $(OUT_DIR)/$@

# Test Programs (in Test/ folder)
# -------------------------------

# Compile digital_led_blink (Basic RedPitaya hardware test)
digital_led_blink: $(OUT_DIR) $(TEST_DIR)/digital_led_blink.cpp
	$(CXX) $(TEST_DIR)/digital_led_blink.cpp $(CXXFLAGS) $(LDFLAGS) $(LDLIBS) -o $(OUT_DIR)/$@

# Compile hardware loopback test (FPGA custom module test - DAC → Cable → ADC)
hardware_loopback_test: $(OUT_DIR) $(TEST_DIR)/hardware_loopback_test.cpp $(SIGNAL_PROCESSING_OBJS) $(HARDWARE_OBJS)
	$(CXX) $(TEST_DIR)/hardware_loopback_test.cpp $(SIGNAL_PROCESSING_OBJS) $(HARDWARE_OBJS) $(CXXFLAGS) $(SIGNAL_LDLIBS) -o $(OUT_DIR)/$@

quartz_sweep_config: $(OUT_DIR) $(TEST_DIR)/quartz_sweep_config.cpp $(SIGNAL_PROCESSING_OBJS) $(HARDWARE_OBJS)
	$(CXX) $(TEST_DIR)/quartz_sweep_config.cpp $(SIGNAL_PROCESSING_OBJS) $(HARDWARE_OBJS) $(CXXFLAGS) $(SIGNAL_LDLIBS) -o $(OUT_DIR)/$@

quartz_sinc_broadband: $(OUT_DIR) $(TEST_DIR)/quartz_sinc_broadband.cpp $(SIGNAL_PROCESSING_OBJS) $(HARDWARE_OBJS)
	$(CXX) $(TEST_DIR)/quartz_sinc_broadband.cpp $(SIGNAL_PROCESSING_OBJS) $(HARDWARE_OBJS) $(CXXFLAGS) $(SIGNAL_LDLIBS) -o $(OUT_DIR)/$@

# Electronic Board UART test (no RP hardware libs needed, just UART)
electronic_board_test: $(OUT_DIR) $(TEST_DIR)/electronic_board_test.cpp $(HARDWARE_DIR)/ElectronicBoardUART.o
	$(CXX) $(TEST_DIR)/electronic_board_test.cpp $(HARDWARE_DIR)/ElectronicBoardUART.o $(CXXFLAGS) -o $(OUT_DIR)/$@

# Single-BRAM validation test suite (6 sub-tests for in-place overwrite safety)
single_bram_validation_test: $(OUT_DIR) $(TEST_DIR)/single_bram_validation_test.cpp $(SIGNAL_PROCESSING_OBJS) $(HARDWARE_OBJS)
	$(CXX) $(TEST_DIR)/single_bram_validation_test.cpp $(SIGNAL_PROCESSING_OBJS) $(HARDWARE_OBJS) $(CXXFLAGS) $(SIGNAL_LDLIBS) -o $(OUT_DIR)/$@

# Calibration loopback comparison test (DAC -> board -> ADC, raw vs calibrated)
calibration_loopback_test: $(OUT_DIR) $(TEST_DIR)/calibration_loopback_test.cpp $(SIGNAL_PROCESSING_OBJS) $(HARDWARE_OBJS)
	$(CXX) $(TEST_DIR)/calibration_loopback_test.cpp $(SIGNAL_PROCESSING_OBJS) $(HARDWARE_OBJS) $(CXXFLAGS) $(SIGNAL_LDLIBS) -o $(OUT_DIR)/$@

# Server Program
# --------------

# AFM Server (TCP server for remote measurement control)
afm_server: $(OUT_DIR) $(SERVER_DIR)/ServerMain.cpp $(SERVER_OBJS) $(SIGNAL_PROCESSING_OBJS) $(HARDWARE_OBJS)
	$(CXX) $(SERVER_DIR)/ServerMain.cpp $(SERVER_OBJS) $(SIGNAL_PROCESSING_OBJS) $(HARDWARE_OBJS) $(CXXFLAGS) -IServer $(SIGNAL_LDLIBS) -lpthread -o $(OUT_DIR)/$@

# Tool Programs
# -------------

# Compile OLED IP display tool (standalone binary)
oled_ip: $(OUT_DIR) $(TOOLS_DIR)/oled_ip.cpp $(OLED_OBJS)
	$(CXX) $(TOOLS_DIR)/oled_ip.cpp $(OLED_OBJS) $(CXXFLAGS) -o $(OUT_DIR)/$@


# Clean object files
clean:
	$(RM) *.o
	$(RM) $(SIGNAL_DIR)/*.o
	$(RM) $(HARDWARE_DIR)/*.o
	$(RM) $(SERVER_DIR)/*.o
	$(RM) $(TOOLS_DIR)/*.o
	$(RM) *.csv

# Clean everything
clean_all: clean
	$(RM) -r $(OUT_DIR)

# Help target
help:
	@echo "TM Project - RedPitaya AFM Signal Processing"
	@echo "=============================================="
	@echo ""
	@echo "IMPORTANT: Always run 'make' from the cpp/ directory, not from Test/"
	@echo ""
	@echo "Available targets:"
	@echo "  all                       - Build main programs (default)"
	@echo "  tests                     - Build all test programs"
	@echo "  server                    - Build AFM server"
	@echo "  tools                     - Build tool programs (oled_ip)"
	@echo "  clean                     - Remove object files and CSV files"
	@echo "  clean_all                 - Remove all generated files"
	@echo "  help                      - Show this help message"
	@echo ""
	@echo "Main programs:"
	@echo "  test_all                  - Comprehensive signal processing test suite"
	@echo "  afm_server                - TCP server for remote measurement control"
	@echo ""
	@echo "Tool programs (in Tools/ folder):"
	@echo "  oled_ip                   - Display IP address on SSD1306 OLED via I2C"
	@echo ""
	@echo "Test programs (in Test/ folder):"
	@echo "  digital_led_blink         - Basic RedPitaya LED test"
	@echo "  hardware_loopback_test    - FPGA custom module test (requires bitfile)"
	@echo "  quartz_sweep_config       - Quartz resonator sweep test"
	@echo "  quartz_sinc_broadband     - Broadband sinc signal test"
	@echo "  electronic_board_test     - Electronic board UART test"
	@echo "  single_bram_validation_test - Single-BRAM in-place overwrite validation"
	@echo "  calibration_loopback_test - Calibration loopback comparison (raw vs calibrated)"
	@echo ""
	@echo "Server usage:"
	@echo "  On RedPitaya: ./out/afm_server [port]"
	@echo "  Python client: python afm_cli.py <redpitaya_ip>"
	@echo ""
	@echo "Signal processing pipeline:"
	@echo "  • Signal generation (sinc, sine waves)"
	@echo "  • FFT analysis with FFTW3"
	@echo "  • Resonance detection (frequency, Q-factor, amplitude)"
	@echo "  • CSV export for visualization"

.PHONY: all tests server tools clean clean_all help