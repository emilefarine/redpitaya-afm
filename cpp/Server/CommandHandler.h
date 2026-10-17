#pragma once

#include "../Hardware/IElectronicBoard.h"
#include "../Hardware/InputSafety.h"
#include "../Hardware/IRedPitayaHardware.h"
#include "../SignalProcessing/FFTProcessor.h"
#include "../SignalProcessing/SaturationDetector.h"
#include "../SignalProcessing/SignalGenerator.h"
#include "HardwareFactories.h"
#include "Protocol.h"

#include <functional>
#include <memory>
#include <vector>

namespace AFM
{

/** @brief Source of the monitored excitation / return loop */
enum class AdcLoopMode : uint8_t
{
  AUTO,   // From the MUX routing
  MANUAL, // From BOARD:ADC:LOOP <excite>,<return>
  OFF
};

struct SystemStatus
{
  bool hardwareInitialized;
  bool boardConnected;
  bool measurementInProgress;
  uint16_t currentDecimation;
  OperatingMode mode;

  // ADC input protection state (see BOARD:ADC:LOOP in Protocol.h)
  AdcLoopMode loopMode;
  uint8_t loopExciteChannel;    ///< MANUAL only (0-based)
  uint8_t loopReturnChannel;    ///< MANUAL only (0-based)
  bool adcProtectEnabled;       ///< Refuse MEASURE predicted to overdrive
  float adcSampleGain;          ///< Expected return / excitation ratio of the AFM
  bool adcOverdrive;            ///< Estimate exceeds ADC full scale
  bool adcSaturated;            ///< Saturation detected in the last acquisition
  float adcSaturationRatio;     ///< Fraction of saturated samples (last acquisition)

  SystemStatus()
      : hardwareInitialized(false)
      , boardConnected(false)
      , measurementInProgress(false)
      , currentDecimation(IRedPitayaHardware::DEFAULT_DECIMATION)
      , mode(OperatingMode::FULL)
      , loopMode(AdcLoopMode::AUTO)
      , loopExciteChannel(0)
      , loopReturnChannel(0)
      , adcProtectEnabled(true)
      , adcSampleGain(1.0f)
      , adcOverdrive(false)
      , adcSaturated(false)
      , adcSaturationRatio(0.0f)
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
  /** @brief Default timeout for blocking measurement waits */
  static constexpr int DEFAULT_MEASUREMENT_TIMEOUT_MS = 5000;

  /** @brief Returns true when the running command must stop */
  using CancelCheck = std::function<bool()>;

  /**
   * @brief Construct a command handler
   * @param hardwareFactory Creates the Red Pitaya hardware on SYSTEM:INIT
   * @param boardFactory Creates the electronic board on SYSTEM:INIT
   * @param measurementTimeoutMs Timeout for blocking measurement waits
   * @param mode Operating mode; RP_ONLY never creates or probes the board
   */
  explicit CommandHandler(HardwareFactory hardwareFactory = defaultHardwareFactory,
                          BoardFactory boardFactory = defaultBoardFactory,
                          int measurementTimeoutMs = DEFAULT_MEASUREMENT_TIMEOUT_MS,
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

  /** @brief Checked between sweep points */
  void setCancelCheck(CancelCheck check)
  {
    m_cancelCheck = std::move(check);
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
  std::string _handleBoardAdcLoop(const ParsedCommand& cmd);
  std::string _handleBoardAdcProtect(const ParsedCommand& cmd);
  std::string _handleBoardAdcSampleGain(const ParsedCommand& cmd);
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
  bool _requireBoard(std::string& errorResponse);
  bool _validateSampleCount(int numSamples, std::string& errorResponse);
  bool _validateDecimation(int decimation, std::string& errorResponse);
  bool _validateChannel1Based(int channel, const char* name, std::string& errorResponse);
  bool _validateAmplitude(float amplitude, std::string& errorResponse);
  bool _validateNyquist(double topHz, uint16_t dec, const char* what,
                        std::string& errorResponse);

  // Shared by the MEASURE handlers
  bool _parseMeasTailArgs(const ParsedCommand& cmd, int& decimation, float& amplitude,
                          std::string& errorResponse);
  void _trackExcitationAmplitude(float amplitude);
  double _applyDecimation(uint16_t dec, std::string& errorResponse);

  // ADC input protection helpers
  struct AdcLoopEstimate
  {
    bool active;
    bool returnOpen;       ///< Nothing routed to the ADC
    bool returnKnown;      ///< Routing could be read
    uint8_t exciteChannel; ///< 0-based
    uint8_t returnChannel; ///< 0-based
    float peak;            ///< V at the ADC input
  };

  AdcLoopEstimate _estimateAdcLoop(float amplitude) const;
  float _loopPeak(float amplitude, uint8_t exciteChannel, uint8_t returnChannel) const;
  std::string _describeOverdrive(const AdcLoopEstimate& estimate, float amplitude) const;
  bool _checkAdcProtection(float amplitude, std::string& errorResponse) const;
  bool _updateAdcOverdriveEstimate();
  std::string _buildOverdriveSuffix();
  std::string _buildOkWithOverdriveWarning();
  void _refreshBoardCacheFromBoard();
  void _refreshBoardCacheFromStatus(const std::string& statusText);
  void _resetBoardCache();

  /**
   * @brief Wait for measurement completion with timeout
   * @return true if measurement completed, false on timeout
   */
  bool _waitForMeasurement();

  HardwareFactory m_hardwareFactory;
  BoardFactory m_boardFactory;
  int m_measurementTimeoutMs;
  CancelCheck m_cancelCheck;

  std::unique_ptr<IRedPitayaHardware> m_hardware;
  std::unique_ptr<IElectronicBoard> m_board;
  std::unique_ptr<SignalGenerator> m_signalGen;
  std::unique_ptr<FFTProcessor> m_fftProcessor;

  SystemStatus m_status;

  /** Board state cache: read at SYSTEM:INIT, updated by BOARD commands and BOARD:STATUS? */
  GainSetting m_channelGains[IElectronicBoard::NUM_CHANNELS];
  uint8_t m_muxRouting[IElectronicBoard::NUM_CHANNELS]; ///< Input per output, or DISCONNECTED
  bool m_muxRoutingKnown[IElectronicBoard::NUM_CHANNELS];
  /** Amplitude of the last commanded excitation (fraction of +/-1 V DAC FS) */
  float m_lastExcitationAmplitude;
};

} // namespace AFM
