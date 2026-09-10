# Compiler for C++ (native build on Red Pitaya)
CXX = g++

# Red Pitaya paths (native build only)
RP_INCLUDE = /opt/redpitaya/include

# Signal processing libraries
SIGNAL_LDLIBS = -lfftw3f -lm -lstdc++

# C++ compiler flags
CXXFLAGS  = -std=c++17 -Wall -O2
CXXFLAGS += -I$(RP_INCLUDE)
CXXFLAGS += -I$(RP_INCLUDE)/api250-12
CXXFLAGS += -ICommon            # Add Common folder to include path
CXXFLAGS += -ISignalProcessing  # Add SignalProcessing folder to include path
CXXFLAGS += -IHardware          # Add Hardware folder to include path

# Signal processing directory
SIGNAL_DIR = SignalProcessing

# Hardware abstraction layer directory
HARDWARE_DIR = Hardware

# Server directory
SERVER_DIR = Server

# Tools directory
TOOLS_DIR = Tools

# Output directory for compiled binaries
OUT_DIR = out

# Server program
SERVER_PRG = afm_server

# Tool programs
TOOL_PRGS = oled_ip

# ---------------------------------------------------------------------------
# Host-side unit tests (dev machine, POSIX-like shell)
# These targets are NOT used on the Red Pitaya.
# Prerequisites:
#   Windows MSYS2 UCRT64 shell: pacman -S mingw-w64-ucrt-x86_64-gtest \
#                               mingw-w64-ucrt-x86_64-fftw
#   Linux: apt install libgtest-dev libgmock-dev libfftw3-dev
#   Anywhere: pip install gcovr
# Run from cpp/ in the MSYS2 UCRT64 shell (Windows) or any POSIX shell:
#   make test_host / make coverage
# ---------------------------------------------------------------------------

UNITTEST_DIR    = tests/unit
HOST_BUILD_DIR  = out/host
HOST_OBJ_DIR    = $(HOST_BUILD_DIR)/obj
HOST_COV_DIR    = $(HOST_BUILD_DIR)/coverage

HOST_CXX       ?= g++
HOST_UNITTEST   = $(HOST_BUILD_DIR)/unit_tests.exe

# Portable production sources covered by host tests (POSIX-only classes are
# excluded: RedPitayaHardware, ElectronicBoardUART, TCPServer, HardwareFactories)
HOST_SRCS  = $(SIGNAL_DIR)/SignalGenerator.cpp \
             $(SIGNAL_DIR)/FFTProcessor.cpp \
             $(SIGNAL_DIR)/ResonanceAnalyzer.cpp \
             $(HARDWARE_DIR)/BoardProtocol.cpp \
             $(SERVER_DIR)/CommandHandler.cpp \
             $(SERVER_DIR)/ServerOptions.cpp

# POSIX-only sources that are compiled on-device but excluded from the host
# test build. check_posix type-checks them on Linux (the Windows/MSYS2 toolchain
# has no sys/mman.h, poll.h, termios.h, ...) so CI catches compile errors.
POSIX_SRCS = $(HARDWARE_DIR)/RedPitayaHardware.cpp \
             $(HARDWARE_DIR)/ElectronicBoardUART.cpp \
             $(HARDWARE_DIR)/SSD1306.cpp \
             $(SERVER_DIR)/TCPServer.cpp \
             $(SERVER_DIR)/HardwareFactories.cpp \
             $(SERVER_DIR)/ServerMain.cpp \
             $(TOOLS_DIR)/oled_ip.cpp

POSIX_CXXFLAGS = -std=c++17 -Wall -Wextra -ICommon -I$(SIGNAL_DIR) -I$(HARDWARE_DIR) -IServer

HOST_OBJS      = $(patsubst $(SIGNAL_DIR)/%.cpp,$(HOST_OBJ_DIR)/%.o,$(filter $(SIGNAL_DIR)/%,$(HOST_SRCS))) \
                 $(patsubst $(HARDWARE_DIR)/%.cpp,$(HOST_OBJ_DIR)/%.o,$(filter $(HARDWARE_DIR)/%,$(HOST_SRCS))) \
                 $(patsubst $(SERVER_DIR)/%.cpp,$(HOST_OBJ_DIR)/%.o,$(filter $(SERVER_DIR)/%,$(HOST_SRCS)))
UNITTEST_SRCS  = $(wildcard $(UNITTEST_DIR)/*.cpp)

HOST_CXXFLAGS = -std=c++17 -Wall -Wextra -g -O0 --coverage \
                -ICommon -I$(SIGNAL_DIR) -I$(HARDWARE_DIR) -IServer -Itests
HOST_LDLIBS   = -lgtest_main -lgmock -lgtest -lfftw3f -lpthread

# Signal processing library objects (in SignalProcessing folder)
SIGNAL_PROCESSING_OBJS = $(SIGNAL_DIR)/SignalGenerator.o $(SIGNAL_DIR)/FFTProcessor.o $(SIGNAL_DIR)/ResonanceAnalyzer.o

# Hardware abstraction layer objects (in Hardware folder)
HARDWARE_OBJS = $(HARDWARE_DIR)/RedPitayaHardware.o $(HARDWARE_DIR)/ElectronicBoardUART.o \
                $(HARDWARE_DIR)/BoardProtocol.o

# Server objects (in Server folder)
SERVER_OBJS = $(SERVER_DIR)/TCPServer.o $(SERVER_DIR)/CommandHandler.o $(SERVER_DIR)/HardwareFactories.o \
              $(SERVER_DIR)/ServerOptions.o

# OLED display objects (in Hardware folder)
OLED_OBJS = $(HARDWARE_DIR)/SSD1306.o

# Default target
all: server tools

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

# Host-side Unit Tests (any POSIX-like shell: MSYS2 UCRT64, Linux, WSL, macOS)
# ---------------------------------------------------------------------------

$(HOST_OBJ_DIR):
	mkdir -p $(HOST_OBJ_DIR)

$(HOST_BUILD_DIR):
	mkdir -p $(HOST_BUILD_DIR)

$(HOST_OBJ_DIR)/%.o: $(SIGNAL_DIR)/%.cpp | $(HOST_OBJ_DIR)
	$(HOST_CXX) -c $< $(HOST_CXXFLAGS) -o $@

$(HOST_OBJ_DIR)/%.o: $(HARDWARE_DIR)/%.cpp | $(HOST_OBJ_DIR)
	$(HOST_CXX) -c $< $(HOST_CXXFLAGS) -o $@

$(HOST_OBJ_DIR)/%.o: $(SERVER_DIR)/%.cpp | $(HOST_OBJ_DIR)
	$(HOST_CXX) -c $< $(HOST_CXXFLAGS) -o $@

$(HOST_UNITTEST): $(HOST_OBJS) $(UNITTEST_SRCS) | $(HOST_BUILD_DIR)
	$(HOST_CXX) $(UNITTEST_SRCS) $(HOST_OBJS) $(HOST_CXXFLAGS) $(HOST_LDLIBS) -o $@

# Build and run host unit tests
test_host: $(HOST_UNITTEST)
	$(HOST_UNITTEST)

# Sanitizer-enabled host tests (Linux/macOS; ASan is unreliable under MSYS2)
HOST_SAN_BUILD_DIR = out/host-san
HOST_SAN_OBJ_DIR   = $(HOST_SAN_BUILD_DIR)/obj
HOST_SAN_UNITTEST  = $(HOST_SAN_BUILD_DIR)/unit_tests_san.exe

HOST_SAN_FLAGS  = -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined
HOST_SAN_CXXFLAGS = $(filter-out --coverage,$(HOST_CXXFLAGS)) $(HOST_SAN_FLAGS)

HOST_SAN_OBJS = $(patsubst $(SIGNAL_DIR)/%.cpp,$(HOST_SAN_OBJ_DIR)/%.o,$(filter $(SIGNAL_DIR)/%,$(HOST_SRCS))) \
                $(patsubst $(HARDWARE_DIR)/%.cpp,$(HOST_SAN_OBJ_DIR)/%.o,$(filter $(HARDWARE_DIR)/%,$(HOST_SRCS))) \
                $(patsubst $(SERVER_DIR)/%.cpp,$(HOST_SAN_OBJ_DIR)/%.o,$(filter $(SERVER_DIR)/%,$(HOST_SRCS)))

$(HOST_SAN_BUILD_DIR):
	mkdir -p $(HOST_SAN_BUILD_DIR)

$(HOST_SAN_OBJ_DIR):
	mkdir -p $(HOST_SAN_OBJ_DIR)

$(HOST_SAN_OBJ_DIR)/%.o: $(SIGNAL_DIR)/%.cpp | $(HOST_SAN_OBJ_DIR)
	$(HOST_CXX) -c $< $(HOST_SAN_CXXFLAGS) -o $@

$(HOST_SAN_OBJ_DIR)/%.o: $(HARDWARE_DIR)/%.cpp | $(HOST_SAN_OBJ_DIR)
	$(HOST_CXX) -c $< $(HOST_SAN_CXXFLAGS) -o $@

$(HOST_SAN_OBJ_DIR)/%.o: $(SERVER_DIR)/%.cpp | $(HOST_SAN_OBJ_DIR)
	$(HOST_CXX) -c $< $(HOST_SAN_CXXFLAGS) -o $@

$(HOST_SAN_UNITTEST): $(HOST_SAN_OBJS) $(UNITTEST_SRCS) | $(HOST_SAN_BUILD_DIR)
	$(HOST_CXX) $(UNITTEST_SRCS) $(HOST_SAN_OBJS) $(HOST_SAN_CXXFLAGS) $(HOST_LDLIBS) -o $@

# Build and run host unit tests under AddressSanitizer and UBSan
test_host_san: $(HOST_SAN_UNITTEST)
	$(HOST_SAN_UNITTEST)

# Type-check the POSIX-only sources excluded from the host test build.
# Linux-only: the MSYS2/mingw toolchain lacks the required POSIX headers.
check_posix:
	$(HOST_CXX) $(POSIX_CXXFLAGS) -fsyntax-only $(POSIX_SRCS)

# Run host unit tests and generate HTML coverage report
coverage: $(HOST_UNITTEST)
	$(RM) $(HOST_OBJ_DIR)/*.gcda $(HOST_BUILD_DIR)/*.gcda
	$(HOST_UNITTEST)
	mkdir -p $(HOST_COV_DIR)
	gcovr --root . --filter $(SIGNAL_DIR)/ --filter Server/ --filter Common/ --filter Hardware/ \
	      --print-summary --html-details $(HOST_COV_DIR)/index.html

# Remove host test build artifacts (objects, coverage data, reports)
clean_host:
	rm -rf $(HOST_BUILD_DIR)

# Remove sanitizer test build artifacts
clean_host_san:
	rm -rf $(HOST_SAN_BUILD_DIR)


# Clean object files
clean:
	$(RM) *.o
	$(RM) $(SIGNAL_DIR)/*.o
	$(RM) $(HARDWARE_DIR)/*.o
	$(RM) $(SERVER_DIR)/*.o
	$(RM) $(TOOLS_DIR)/*.o

# Clean everything
clean_all: clean
	$(RM) -r $(OUT_DIR)

# Help target
help:
	@echo "TM Project - RedPitaya AFM Signal Processing"
	@echo "=============================================="
	@echo ""
	@echo "IMPORTANT: Always run 'make' from the cpp/ directory"
	@echo ""
	@echo "Available targets:"
	@echo "  all                       - Build server and tools (default)"
	@echo "  server                    - Build AFM server"
	@echo "  tools                     - Build tool programs (oled_ip)"
	@echo "  test_host                 - Run host unit tests (no RedPitaya needed)"
	@echo "  test_host_san             - Host unit tests under ASan/UBSan (Linux/macOS)"
	@echo "  check_posix               - Type-check POSIX-only sources (Linux only)"
	@echo "  coverage                  - Host unit tests + HTML coverage report"
	@echo "                              (out/host/coverage/index.html)"
	@echo "  clean                     - Remove object files"
	@echo "  clean_all                 - Remove all generated files"
	@echo "  clean_host                - Remove host test build artifacts"
	@echo "  clean_host_san            - Remove sanitizer test build artifacts"
	@echo "  help                      - Show this help message"
	@echo ""
	@echo "Main programs:"
	@echo "  afm_server                - TCP server for remote measurement control"
	@echo ""
	@echo "Tool programs (in Tools/ folder):"
	@echo "  oled_ip                   - Display IP address on SSD1306 OLED via I2C"
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

.PHONY: all server tools test_host test_host_san check_posix coverage clean_host clean_host_san clean clean_all help