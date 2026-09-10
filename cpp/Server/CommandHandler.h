#pragma once

#include "../Hardware/IElectronicBoard.h"
#include "../Hardware/IRedPitayaHardware.h"
#include "../SignalProcessing/FFTProcessor.h"
#include "../SignalProcessing/ResonanceAnalyzer.h"
#include "../SignalProcessing/SignalGenerator.h"
#include "HardwareFactories.h"
#include "Protocol.h"

#include <memory>
#include <vector>

namespace AFM
{

struct SystemStatus
{
  bool hardwareInitialized;
  bool boardConnected;
  bool measurementInProgress;
  uint16_t currentDecimation;
  OperatingMode mode;

  SystemStatus()
      : hardwareInitialized(false)
      , boardConnected(false)
      , measurementInProgress(false)
      , currentDecimation(IRedPitayaHardware::DEFAULT_DECIMATION)
      , mode(OperatingMode::FULL)
  {
  }
};

/**
 * @brief Command handler for AFM server
 *
 * Hardware instances are created lazily on SYSTEM:INIT through the injected
 * factories. Tests inject factory functions returning mocks; production uses
 * the defaults from HardwareFactories (real /dev/mem + UART implementations).
 */
class CommandHandler
{
public:
  /**
   * @brief Construct a command handler
   * @param hardwareFactory Creates the Red Pitaya hardware on SYSTEM:INIT
   * @param boardFactory Creates the electronic board on SYSTEM:INIT
   * @param measurementTimeoutMs Timeout for blocking measurement waits
   * @param mode Operating mode; RP_ONLY never creates or probes the board
   */
  explicit CommandHandler(HardwareFactory hardwareFactory = defaultHardwareFactory,
                          BoardFactory boardFactory = defaultBoardFactory,
                          int measurementTimeoutMs = 5000,
                          OperatingMode mode = OperatingMode::FULL);
  ~CommandHandler();

  CommandHandler(const CommandHandler&) = delete;
  CommandHandler& operator=(const CommandHandler&) = delete;

  /**
   * @brief Handle an incoming command
   * @param cmd Parsed command
   * @return Response string to send to client
   */
  std::string handleCommand(const ParsedCommand& cmd);

  /**
   * @brief Get current system status
   */
  const SystemStatus& getStatus() const
  {
    return m_status;
  }

private:
  // IEEE 488.2 common commands
  std::string _handleIdn(const ParsedCommand& cmd);
  std::string _handleRst(const ParsedCommand& cmd);
  std::string _handleOpc(const ParsedCommand& cmd);

  // SYSTEM subsystem
  std::string _handleSystPing(const ParsedCommand& cmd);
  std::string _handleSystVersion(const ParsedCommand& cmd);
  std::string _handleSystStatus(const ParsedCommand& cmd);
  std::string _handleSystInit(const ParsedCommand& cmd);
  std::string _handleSystDeinit(const ParsedCommand& cmd);
  std::string _handleSystMode(const ParsedCommand& cmd);

  // BOARD subsystem
  std::string _handleBoardMuxRoute(const ParsedCommand& cmd);
  std::string _handleBoardMuxDisconnect(const ParsedCommand& cmd);
  std::string _handleBoardGain(const ParsedCommand& cmd);
  std::string _handleBoardReset(const ParsedCommand& cmd);
  std::string _handleBoardStatus(const ParsedCommand& cmd);

  // MEASURE subsystem
  std::string _handleMeasSinc(
      const ParsedCommand& cmd); // we might want to add an argument for the windowing (current
                                 // default using Hann window)
  std::string _handleMeasSweep(const ParsedCommand& cmd);

  std::string _handleUnknown(const ParsedCommand& cmd);

  std::string _boardUnavailableError() const;
  bool _checkInitialized(std::string& errorResponse);
  bool _validateSampleCount(int numSamples, std::string& errorResponse);
  bool _validateDecimation(int decimation, std::string& errorResponse);

  /**
   * @brief Wait for measurement completion with timeout
   * @return true if measurement completed, false on timeout
   */
  bool _waitForMeasurement();

  HardwareFactory m_hardwareFactory;
  BoardFactory m_boardFactory;
  int m_measurementTimeoutMs;

  std::unique_ptr<IRedPitayaHardware> m_hardware;
  std::unique_ptr<IElectronicBoard> m_board;
  std::unique_ptr<SignalGenerator> m_signalGen;
  std::unique_ptr<FFTProcessor> m_fftProcessor;
  std::unique_ptr<ResonanceAnalyzer> m_resonanceAnalyzer;

  SystemStatus m_status;
};

} // namespace AFM
