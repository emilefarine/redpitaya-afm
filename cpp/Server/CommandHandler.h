#pragma once

#include "../Hardware/ElectronicBoardUART.h"
#include "../Hardware/RedPitayaHardware.h"
#include "../SignalProcessing/CalibrationManager.h"
#include "../SignalProcessing/FFTProcessor.h"
#include "../SignalProcessing/ResonanceAnalyzer.h"
#include "../SignalProcessing/SignalGenerator.h"
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
  bool calibrationActive;
  uint16_t currentDecimation;

  SystemStatus()
      : hardwareInitialized(false)
      , boardConnected(false)
      , measurementInProgress(false)
      , calibrationActive(false)
      , currentDecimation(RedPitayaHardware::DEFAULT_DECIMATION)
  {
  }
};

/**
 * @brief Command handler for AFM server
 */
class CommandHandler
{
public:
  CommandHandler();
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

  // CALIBRATE subsystem
  std::string _handleCalRun(
      const ParsedCommand&
          cmd); // if we don't use Hann window during sinc measurement, the calibration will be
                // wrong. Before sinc with another window, the calibration should be also done again
  std::string _handleCalStatus(const ParsedCommand& cmd);
  std::string _handleCalClear(const ParsedCommand& cmd);

  std::string _handleUnknown(const ParsedCommand& cmd);

  bool _checkInitialized(std::string& errorResponse);
  bool _validateSampleCount(int numSamples, std::string& errorResponse);
  bool _validateDecimation(int decimation, std::string& errorResponse);

  /**
   * @brief Wait for measurement completion with timeout
   * @return true if measurement completed, false on timeout
   */
  bool _waitForMeasurement(int timeoutMs = 5000);

  std::unique_ptr<RedPitayaHardware> m_hardware;
  std::unique_ptr<ElectronicBoardUART> m_board;
  std::unique_ptr<SignalGenerator> m_signalGen;
  std::unique_ptr<FFTProcessor> m_fftProcessor;
  std::unique_ptr<ResonanceAnalyzer> m_resonanceAnalyzer;
  std::unique_ptr<CalibrationManager> m_calibration;

  SystemStatus m_status;
};

} // namespace AFM
