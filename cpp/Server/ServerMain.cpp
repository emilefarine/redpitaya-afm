#include "CommandHandler.h"
#include "Protocol.h"
#include "ServerOptions.h"
#include "TCPServer.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <fstream>
#include <iostream>
#include <memory>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

static constexpr const char* FPGA_BITSTREAM_PATH = "/root/afm/fpga/red_pitaya_top.bit.bin";

// Safe signal handling: only set an atomic flag
static volatile std::atomic<bool> g_stopSignal{false};

/**
 * @brief Load FPGA bitstream
 * @param bitstreamPath Path to the .bit.bin file
 * @return true if successful
 */
bool loadFpgaBitstream(const std::string& bitstreamPath)
{
  std::cout << "[FPGA] Loading bitstream: " << bitstreamPath << std::endl;

  std::ifstream bitfile(bitstreamPath);
  if (!bitfile.good())
  {
    std::cerr << "[FPGA] Error: Cannot open bitstream file: " << bitstreamPath << std::endl;
    return false;
  }
  bitfile.close();

  // Use fpgautil to load the bitstream
  // This handles Device Tree Overlays and FPGA Manager correctly, ensuring
  // that AXI clocks and interfaces are properly set up.
  // Writing directly to /dev/xdevcfg is deprecated and can lead to "VERSION register = 0x0"
  std::cout << "[FPGA] Executing: fpgautil -b " << bitstreamPath << std::endl;

  // Build the argv once before fork so the child performs no allocations.
  std::vector<std::string> fpgautilArgs = AFM::buildFpgautilArgs(bitstreamPath);
  std::vector<char*> fpgautilArgv;
  fpgautilArgv.reserve(fpgautilArgs.size() + 2);
  fpgautilArgv.push_back(nullptr); // argv[0], replaced per candidate in the child
  for (const std::string& arg : fpgautilArgs)
  {
    fpgautilArgv.push_back(const_cast<char*>(arg.c_str()));
  }
  fpgautilArgv.push_back(nullptr);

  // Use fork/exec instead of std::system() to avoid shell injection vulnerabilities.
  // With the exec family, arguments are passed directly to the process without shell
  // interpretation, so shell metacharacters in bitstreamPath have no effect.
  pid_t pid = fork();
  if (pid < 0)
  {
    std::cerr << "[FPGA] Error: fork() failed" << std::endl;
    return false;
  }

  if (pid == 0)
  {
    // Child process: execute fpgautil only from known absolute locations
    static constexpr const char* c_FpgautilCandidates[] = {
        "/opt/redpitaya/bin/fpgautil",
        "/usr/bin/fpgautil",
        "/usr/local/bin/fpgautil",
        "/usr/sbin/fpgautil",
        "/sbin/fpgautil",
    };

    for (const char* candidate : c_FpgautilCandidates)
    {
      fpgautilArgv[0] = const_cast<char*>(candidate);
      execv(candidate, fpgautilArgv.data());
    }

    _exit(127);
  }

  // Parent process: wait for child to finish
  int status;
  if (waitpid(pid, &status, 0) < 0)
  {
    std::cerr << "[FPGA] Error: waitpid() failed" << std::endl;
    return false;
  }

  int result = WIFEXITED(status) ? WEXITSTATUS(status) : -1;

  if (result != 0)
  {
    std::cerr << "[FPGA] Error: fpgautil failed with code " << result << std::endl;
    return false;
  }

  std::cout << "[FPGA] Bitstream loaded successfully" << std::endl;
  return true;
}

/**
 * @brief Signal handler for graceful shutdown
 *
 * IMPORTANT: Signal handlers must be async-signal-safe.
 * - NO std::cout (uses locks, can deadlock)
 * - NO complex function calls (thread::join, etc.)
 * - Only set atomic flags
 */
void signalHandler(int)
{
  g_stopSignal.store(true);
}

/**
 * @brief Print usage information
 */
void printUsage(const char* programName)
{
  std::cout << "AFM SCPI Measurement Server v" << AFM::VersionInfo::toString() << "\n"
            << "\n"
            << "Usage: " << programName << " [options]\n"
            << "\n"
            << "Options:\n"
            << "  -p, --port <port>       TCP port to listen on (default: "
            << AFM::ServerConfig::DEFAULT_PORT << ")\n"
            << "  -b, --bitstream <path>  FPGA bitstream path (default: " << FPGA_BITSTREAM_PATH
            << ")\n"
            << "      --no-board          Red Pitaya only: never probe the electronic board\n"
            << "  -h, --help              Show this help message\n"
            << "  -v, --version           Show version information\n"
            << "\n"
            << "SCPI Commands:\n"
            << "  IEEE 488.2: *IDN?, *RST, *OPC?\n"
            << "  System:     SYSTEM:PING, SYSTEM:VERSION, SYSTEM:STATUS,\n"
            << "              SYSTEM:INIT, SYSTEM:DEINIT, SYSTEM:SHUTDOWN\n"
            << "  Board:      BOARD:MUX:ROUTE, BOARD:MUX:DISCONNECT,\n"
            << "              BOARD:GAIN, BOARD:RESET, BOARD:STATUS\n"
            << "  Measure:    MEASURE:SINC, MEASURE:SWEEP\n"
            << std::endl;
}

int main(int argc, char* argv[])
{
  try
  {
    std::cout << "========================================\n"
              << " AFM SCPI Measurement Server\n"
              << " Version " << AFM::VersionInfo::toString() << "\n"
              << "========================================\n"
              << std::endl;

    // Parse command line arguments
    AFM::ServerOptions options;
    options.bitstreamPath = FPGA_BITSTREAM_PATH;

    std::vector<std::string> args(argv + 1, argv + argc);
    std::string parseError;
    AFM::OptionsParseResult parseResult = AFM::parseServerOptions(args, options, parseError);

    if (parseResult == AFM::OptionsParseResult::Help)
    {
      printUsage(argv[0]);
      return 0;
    }
    if (parseResult == AFM::OptionsParseResult::Version)
    {
      std::cout << "AFM SCPI Server v" << AFM::VersionInfo::toString() << std::endl;
      return 0;
    }
    if (parseResult == AFM::OptionsParseResult::Error)
    {
      std::cerr << "Error: " << parseError << std::endl;
      printUsage(argv[0]);
      return 1;
    }

    // Load FPGA bitstream
    if (!loadFpgaBitstream(options.bitstreamPath))
    {
      std::cerr << "[Error] FPGA bitstream loading failed." << std::endl;
      return 1;
    }

    // Set up signal handlers
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    // Create command handler (must be declared before server so it outlives the server thread)
    AFM::OperatingMode mode =
        options.noBoard ? AFM::OperatingMode::RP_ONLY : AFM::OperatingMode::FULL;
    auto commandHandler = std::make_unique<AFM::CommandHandler>(AFM::defaultHardwareFactory,
                                                                AFM::defaultBoardFactory, 5000,
                                                                mode);

    std::cout << "[Main] Operating mode: " << AFM::operatingModeToString(mode) << std::endl;

    // Create TCP server
    AFM::TCPServer server(options.port);

    // Set command callback
    server.setCommandHandler([&commandHandler](const AFM::ParsedCommand& cmd)
                             { return commandHandler->handleCommand(cmd); });

    std::cout << "[Main] Starting server on port " << options.port << "...\n" << std::endl;

    // Start server asynchronously so main thread can monitor shutdown signal
    if (!server.startAsync())
    {
      std::cerr << "[Error] Failed to start server: " << server.getLastError() << std::endl;
      return 1;
    }

    std::cout << "[Main] Server running. Press Ctrl+C to stop.\n" << std::endl;

    // Main event loop: wait for shutdown signal
    // This is safer than blocking in server.start() - we can respond to signals cleanly
    while (!g_stopSignal.load())
    {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // Graceful shutdown (safe to call here, not in signal handler)
    std::cout << "\n[Main] Shutdown signal received. Stopping server..." << std::endl;
    server.stop();
    std::cout << "[Main] Server stopped." << std::endl;
  }
  catch (const std::exception& e)
  {
    std::cerr << "\n[CRITICAL] Uncaught exception: " << e.what() << std::endl;
    return 1;
  }
  catch (...)
  {
    std::cerr << "\n[CRITICAL] Unknown exception occurred." << std::endl;
    return 1;
  }

  return 0;
}
