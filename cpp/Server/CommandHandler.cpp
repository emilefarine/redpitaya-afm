/**
 * @file CommandHandler.cpp
 * @brief Command handler implementation for AFM measurement server
 *
 * @date December 2025
 */

#include "CommandHandler.h"

#include "Protocol.h"

#include "../Hardware/BoardProtocol.h"
#include "../Hardware/HardwareLimits.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <thread>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace AFM
{

namespace
{

// Covers the rounding of a float step in the sweep point count
constexpr double c_SweepStepTolerance = 1.0e-6;

class ScopedMeasurement
{
public:
  ScopedMeasurement(SystemStatus& status, IRedPitayaHardware& hardware)
      : m_status(status)
      , m_hardware(hardware)
  {
    m_status.measurementInProgress = true;
  }

  ~ScopedMeasurement()
  {
    m_status.measurementInProgress = false;
    m_hardware.resetMeasurement();
  }

  ScopedMeasurement(const ScopedMeasurement&) = delete;
  ScopedMeasurement& operator=(const ScopedMeasurement&) = delete;

private:
  SystemStatus& m_status;
  IRedPitayaHardware& m_hardware;
};

} // namespace

CommandHandler::CommandHandler(HardwareFactory hardwareFactory,
                               BoardFactory boardFactory,
                               int measurementTimeoutMs,
                               OperatingMode mode)
    : m_hardwareFactory(std::move(hardwareFactory))
    , m_boardFactory(std::move(boardFactory))
    , m_measurementTimeoutMs(measurementTimeoutMs)
    , m_hardware(nullptr)
    , m_board(nullptr)
    , m_signalGen(nullptr)
    , m_fftProcessor(nullptr)
    , m_resonanceAnalyzer(nullptr)
    , m_lastExcitationAmplitude(1.0f)
{
  m_status.mode = mode;
  _resetBoardCache();
}

CommandHandler::~CommandHandler()
{
  if (m_hardware)
  {
    m_hardware->cleanup();
  }
  if (m_board)
  {
    m_board->close();
  }
}

std::string CommandHandler::handleCommand(const ParsedCommand& cmd)
{
  switch (cmd.command)
  {
  // IEEE 488.2 common commands
  case Command::IDN:
    return _handleIdn(cmd);
  case Command::RST:
    return _handleRst(cmd);
  case Command::OPC:
    return _handleOpc(cmd);

  // SYSTEM subsystem
  case Command::SYST_PING:
    return _handleSystPing(cmd);
  case Command::SYST_VERSION:
    return _handleSystVersion(cmd);
  case Command::SYST_STATUS:
    return _handleSystStatus(cmd);
  case Command::SYST_INIT:
    return _handleSystInit(cmd);
  case Command::SYST_DEINIT:
    return _handleSystDeinit(cmd);
  case Command::SYST_MODE:
    return _handleSystMode(cmd);

  // BOARD subsystem
  case Command::BOARD_MUX_ROUTE:
    return _handleBoardMuxRoute(cmd);
  case Command::BOARD_MUX_DISCONNECT:
    return _handleBoardMuxDisconnect(cmd);
  case Command::BOARD_GAIN:
    return _handleBoardGain(cmd);
  case Command::BOARD_ADC_LOOP:
    return _handleBoardAdcLoop(cmd);
  case Command::BOARD_ADC_PROTECT:
    return _handleBoardAdcProtect(cmd);
  case Command::BOARD_ADC_SAMPLEGAIN:
    return _handleBoardAdcSampleGain(cmd);
  case Command::BOARD_RESET:
    return _handleBoardReset(cmd);
  case Command::BOARD_STATUS:
    return _handleBoardStatus(cmd);

  // MEASURE subsystem
  case Command::MEAS_SINC:
    return _handleMeasSinc(cmd);
  case Command::MEAS_SWEEP:
    return _handleMeasSweep(cmd);

  // Shutdown handled in TCPServer, unknown commands
  case Command::SYST_SHUTDOWN:
  case Command::UNKNOWN:
  default:
    return _handleUnknown(cmd);
  }
}

// ---- IEEE 488.2 common commands ----

std::string CommandHandler::_handleIdn(const ParsedCommand& cmd)
{
  (void)cmd;
  return buildOkResponse("RedPitaya,AFM_SERVER," + VersionInfo::toString());
}

std::string CommandHandler::_handleRst(const ParsedCommand& cmd)
{
  (void)cmd;
  // Reset to power-on state: deinit hardware, clear signal buffer
  if (m_hardware)
  {
    m_hardware->cleanup();
    m_hardware.reset();
  }
  if (m_board)
  {
    m_board->close();
    m_board.reset();
  }
  m_signalGen.reset();
  m_fftProcessor.reset();
  m_resonanceAnalyzer.reset();
  // *RST does not reset the board, so the cache is stale until SYSTEM:INIT
  // re-reads it. Safe because the monitor is inactive until then.
  _resetBoardCache();
  m_lastExcitationAmplitude = 1.0f;
  OperatingMode mode = m_status.mode;
  m_status = SystemStatus();
  m_status.mode = mode;
  return buildOkResponse();
}

std::string CommandHandler::_handleOpc(const ParsedCommand& cmd)
{
  (void)cmd;
  // *OPC? returns 1 when all pending operations are complete.
  // Our commands are synchronous, so always complete.
  return buildOkResponse("1");
}

// ---- SYSTEM subsystem ----

std::string CommandHandler::_handleSystPing(const ParsedCommand& cmd)
{
  (void)cmd;
  return buildOkResponse("PONG");
}

std::string CommandHandler::_handleSystVersion(const ParsedCommand& cmd)
{
  (void)cmd;
  return buildOkResponse("AFM_SERVER " + VersionInfo::toString());
}

std::string CommandHandler::_handleSystMode(const ParsedCommand& cmd)
{
  (void)cmd;
  return buildOkResponse(operatingModeToString(m_status.mode));
}

std::string CommandHandler::_handleSystStatus(const ParsedCommand& cmd)
{
  (void)cmd;

  // Recompute the estimate so the reported state can never be stale
  _updateAdcOverdriveEstimate();
  const AdcLoopEstimate loop = _estimateAdcLoop(m_lastExcitationAmplitude);

  std::ostringstream oss;
  oss << "HW_INIT=" << (m_status.hardwareInitialized ? "1" : "0")
      << " BOARD=" << (m_status.boardConnected ? "1" : "0")
      << " BUSY=" << (m_status.measurementInProgress ? "1" : "0")
      << " DEC=" << m_status.currentDecimation
      << " MODE=" << operatingModeToString(m_status.mode)
      << " LOOP=";
  if (loop.active)
  {
    // Report 1-based channel numbers matching board connector labels
    oss << static_cast<int>(loop.exciteChannel + 1) << ",";
    if (loop.returnOpen)
    {
      oss << "X";
    }
    else if (!loop.returnKnown)
    {
      oss << "?";
    }
    else
    {
      oss << static_cast<int>(loop.returnChannel + 1);
    }
  }
  else
  {
    oss << "OFF";
  }
  oss << " OVERDRIVE=" << (m_status.adcOverdrive ? "1" : "0")
      << " SAT=" << (m_status.adcSaturated ? "1" : "0")
      << " PROTECT=" << (m_status.adcProtectEnabled ? "1" : "0")
      << " SAMPLEGAIN=" << m_status.adcSampleGain;

  return buildOkResponse(oss.str());
}

std::string CommandHandler::_handleSystInit(const ParsedCommand& cmd)
{
  (void)cmd;

  if (m_status.hardwareInitialized)
  {
    return buildOkResponse("Already initialized");
  }

  // Initialize Red Pitaya hardware
  m_hardware = m_hardwareFactory();
  if (!m_hardware || !m_hardware->initialize())
  {
    m_hardware.reset();
    return buildErrorResponse(ResponseStatus::ERR_HARDWARE, "Failed to initialize Red Pitaya");
  }

  // Initialize electronic board (skipped entirely in RP-only mode)
  if (m_status.mode == OperatingMode::RP_ONLY)
  {
    m_status.boardConnected = false;
  }
  else
  {
    m_board = m_boardFactory();
    if (!m_board || !m_board->initialize())
    {
      std::cout << "[CommandHandler] Warning: Electronic board not connected" << std::endl;
      m_status.boardConnected = false;
    }
    else
    {
      m_status.boardConnected = true;
    }
  }

  // Initialize signal generator with current decimation
  m_status.currentDecimation = m_hardware->getDecimation();
  double samplingFreq = ServerConfig::ADC_SAMPLE_RATE_HZ / m_status.currentDecimation;
  m_signalGen = std::make_unique<SignalGenerator>(samplingFreq, m_status.currentDecimation);

  // The board keeps gains and routing across re-initialization: read them back
  if (m_status.boardConnected)
  {
    _refreshBoardCacheFromBoard();
  }

  m_status.hardwareInitialized = true;

  std::ostringstream oss;
  if (m_status.mode == OperatingMode::RP_ONLY)
  {
    oss << "Red Pitaya OK, Board DISABLED (RP-only mode)";
  }
  else
  {
    oss << "Red Pitaya OK, Board " << (m_status.boardConnected ? "OK" : "NOT CONNECTED");
  }
  return buildOkResponse(oss.str());
}

std::string CommandHandler::_handleSystDeinit(const ParsedCommand& cmd)
{
  (void)cmd;

  if (!m_status.hardwareInitialized)
  {
    return buildOkResponse("Not initialized");
  }

  if (m_hardware)
  {
    m_hardware->cleanup();
    m_hardware.reset();
  }

  if (m_board)
  {
    m_board->close();
    m_board.reset();
  }

  m_signalGen.reset();
  m_status.hardwareInitialized = false;
  m_status.boardConnected = false;
  m_status.adcOverdrive = false;
  m_status.adcSaturated = false;
  m_status.adcSaturationRatio = 0.0f;

  return buildOkResponse("Hardware deinitialized");
}

// ---- BOARD subsystem ----

std::string CommandHandler::_handleBoardMuxRoute(const ParsedCommand& cmd)
{
  std::string error;
  if (!_requireBoard(error))
    return error;

  int output, input;
  if (!cmd.getArgInt(0, output) || !cmd.getArgInt(1, input))
  {
    return buildErrorResponse(ResponseStatus::ERR_SYNTAX, "Usage: BOARD:MUX:ROUTE <out>,<in>");
  }

  // Accept 1-based channel numbers matching board connector labels (1-4)
  if (!_validateChannel1Based(output, "out", error) ||
      !_validateChannel1Based(input, "in", error))
  {
    return error;
  }

  // Convert to 0-indexed for internal use
  if (!m_board->setMuxRoute(static_cast<uint8_t>(output - 1), static_cast<uint8_t>(input - 1)))
  {
    return buildErrorResponse(ResponseStatus::ERR_HARDWARE, m_board->getLastError());
  }

  m_muxRouting[output - 1] = static_cast<uint8_t>(input - 1);
  m_muxRoutingKnown[output - 1] = true;
  return _buildOkWithOverdriveWarning();
}

std::string CommandHandler::_handleBoardMuxDisconnect(const ParsedCommand& cmd)
{
  std::string error;
  if (!_requireBoard(error))
    return error;

  int output;
  if (!cmd.getArgInt(0, output))
  {
    return buildErrorResponse(ResponseStatus::ERR_SYNTAX, "Usage: BOARD:MUX:DISCONNECT <out>");
  }

  // Accept 1-based channel number matching board connector label (1-4)
  if (!_validateChannel1Based(output, "out", error))
    return error;

  // Convert to 0-indexed for internal use
  if (!m_board->disconnectMux(static_cast<uint8_t>(output - 1)))
  {
    return buildErrorResponse(ResponseStatus::ERR_HARDWARE, m_board->getLastError());
  }

  m_muxRouting[output - 1] = IElectronicBoard::DISCONNECTED;
  m_muxRoutingKnown[output - 1] = true;
  return _buildOkWithOverdriveWarning();
}

std::string CommandHandler::_handleBoardGain(const ParsedCommand& cmd)
{
  std::string error;
  if (!_requireBoard(error))
    return error;

  int channel, gainIndex;
  if (!cmd.getArgInt(0, channel) || !cmd.getArgInt(1, gainIndex))
  {
    return buildErrorResponse(ResponseStatus::ERR_SYNTAX, "Usage: BOARD:GAIN <ch>,<gain_index>");
  }

  // Accept 1-based channel number matching board connector label (1-4)
  if (!_validateChannel1Based(channel, "channel", error))
    return error;

  if (gainIndex < 0 || gainIndex > IElectronicBoard::MAX_GAIN_INDEX)
  {
    return buildErrorResponse(ResponseStatus::ERR_PARAM, "gain_index must be 0-7");
  }

  // Convert to 0-indexed channel for internal use
  GainSetting gain = IElectronicBoard::gainFromIndex(static_cast<uint8_t>(gainIndex));
  if (!m_board->setGain(static_cast<uint8_t>(channel - 1), gain))
  {
    return buildErrorResponse(ResponseStatus::ERR_HARDWARE, m_board->getLastError());
  }

  m_channelGains[static_cast<size_t>(channel - 1)] = gain;
  const std::string warn = _buildOverdriveSuffix();
  return buildOkResponse(IElectronicBoard::gainToString(gain) + warn);
}

std::string CommandHandler::_handleBoardAdcLoop(const ParsedCommand& cmd)
{
  static const char* const c_Usage =
      "Usage: BOARD:ADC:LOOP AUTO | <excite_ch>,<return_ch> | OFF";

  std::string error;
  if (!_requireBoard(error))
    return error;

  // Query: report the configured loop mode
  if (cmd.args.empty())
  {
    if (!cmd.isQuery)
    {
      return buildErrorResponse(ResponseStatus::ERR_SYNTAX, c_Usage);
    }
    switch (m_status.loopMode)
    {
    case AdcLoopMode::MANUAL:
      // Reply with 1-based channel numbers matching board connector labels
      return buildOkResponse(std::to_string(m_status.loopExciteChannel + 1) + "," +
                             std::to_string(m_status.loopReturnChannel + 1));
    case AdcLoopMode::OFF:
      return buildOkResponse("OFF");
    case AdcLoopMode::AUTO:
    default:
      return buildOkResponse("AUTO");
    }
  }

  // AUTO | OFF
  if (cmd.args.size() == 1)
  {
    std::string arg0;
    cmd.getArgString(0, arg0);
    std::transform(arg0.begin(), arg0.end(), arg0.begin(), ::toupper);
    if (arg0 == "OFF")
    {
      m_status.loopMode = AdcLoopMode::OFF;
      m_status.adcOverdrive = false;
      return buildOkResponse("ADC loop monitor off");
    }
    if (arg0 == "AUTO")
    {
      m_status.loopMode = AdcLoopMode::AUTO;
      return buildOkResponse("AUTO" + _buildOverdriveSuffix());
    }
    return buildErrorResponse(ResponseStatus::ERR_SYNTAX, c_Usage);
  }

  int excite, ret;
  if (!cmd.getArgInt(0, excite) || !cmd.getArgInt(1, ret))
  {
    return buildErrorResponse(ResponseStatus::ERR_SYNTAX, c_Usage);
  }

  // Accept 1-based channel numbers matching board connector labels (1-4)
  if (!_validateChannel1Based(excite, "excite_ch", error) ||
      !_validateChannel1Based(ret, "return_ch", error))
  {
    return error;
  }

  m_status.loopExciteChannel = static_cast<uint8_t>(excite - 1);
  m_status.loopReturnChannel = static_cast<uint8_t>(ret - 1);
  m_status.loopMode = AdcLoopMode::MANUAL;

  std::string response =
      std::to_string(excite) + "," + std::to_string(ret) + _buildOverdriveSuffix();
  return buildOkResponse(response);
}

std::string CommandHandler::_handleBoardAdcProtect(const ParsedCommand& cmd)
{
  static const char* const c_Usage = "Usage: BOARD:ADC:PROTECT ON | OFF";

  std::string error;
  if (!_requireBoard(error))
    return error;

  if (cmd.args.empty())
  {
    if (!cmd.isQuery)
    {
      return buildErrorResponse(ResponseStatus::ERR_SYNTAX, c_Usage);
    }
    return buildOkResponse(m_status.adcProtectEnabled ? "ON" : "OFF");
  }

  std::string arg0;
  if (cmd.args.size() != 1 || !cmd.getArgString(0, arg0))
  {
    return buildErrorResponse(ResponseStatus::ERR_SYNTAX, c_Usage);
  }
  std::transform(arg0.begin(), arg0.end(), arg0.begin(), ::toupper);

  if (arg0 == "ON")
  {
    m_status.adcProtectEnabled = true;
  }
  else if (arg0 == "OFF")
  {
    m_status.adcProtectEnabled = false;
  }
  else
  {
    return buildErrorResponse(ResponseStatus::ERR_SYNTAX, c_Usage);
  }

  return buildOkResponse(arg0 + _buildOverdriveSuffix());
}

std::string CommandHandler::_handleBoardAdcSampleGain(const ParsedCommand& cmd)
{
  static const char* const c_Usage = "Usage: BOARD:ADC:SAMPLEGAIN <factor>";

  std::string error;
  if (!_requireBoard(error))
    return error;

  if (cmd.args.empty())
  {
    if (!cmd.isQuery)
    {
      return buildErrorResponse(ResponseStatus::ERR_SYNTAX, c_Usage);
    }
    std::ostringstream oss;
    oss << m_status.adcSampleGain;
    return buildOkResponse(oss.str());
  }

  float sampleGain;
  if (cmd.args.size() != 1 || !cmd.getArgFloat(0, sampleGain))
  {
    return buildErrorResponse(ResponseStatus::ERR_SYNTAX, c_Usage);
  }
  if (sampleGain < InputSafety::MIN_SAMPLE_GAIN || sampleGain > InputSafety::MAX_SAMPLE_GAIN)
  {
    std::ostringstream oss;
    oss << "factor must be " << InputSafety::MIN_SAMPLE_GAIN << " to "
        << InputSafety::MAX_SAMPLE_GAIN;
    return buildErrorResponse(ResponseStatus::ERR_PARAM, oss.str());
  }

  m_status.adcSampleGain = sampleGain;

  std::ostringstream oss;
  oss << sampleGain << _buildOverdriveSuffix();
  return buildOkResponse(oss.str());
}

std::string CommandHandler::_handleBoardReset(const ParsedCommand& cmd)
{
  (void)cmd;

  std::string error;
  if (!_requireBoard(error))
    return error;

  if (!m_board->reset())
  {
    return buildErrorResponse(ResponseStatus::ERR_HARDWARE, m_board->getLastError());
  }

  // The board firmware sets all gains back to x1 and disconnects the mux
  _resetBoardCache();
  const std::string warn = _buildOverdriveSuffix();
  return buildOkResponse(std::string("Board reset complete") + warn);
}

std::string CommandHandler::_handleBoardStatus(const ParsedCommand& cmd)
{
  (void)cmd;

  std::string error;
  if (!_requireBoard(error))
    return error;

  std::string boardStatus;
  if (!m_board->getStatus(boardStatus))
  {
    return buildErrorResponse(ResponseStatus::ERR_HARDWARE, m_board->getLastError());
  }

  // Resync the cache with the real board state
  _refreshBoardCacheFromStatus(boardStatus);

  return buildTextResponse(boardStatus);
}

// ---- MEASURE subsystem ----

std::string CommandHandler::_handleMeasSinc(const ParsedCommand& cmd)
{
  std::string error;
  if (!_checkInitialized(error))
  {
    return error;
  }

  // Parse arguments: MEASURE:SINC
  // <center_kHz>,<bandwidth_kHz>[,<num_samples>,<decimation>,<amplitude>]
  float centerKHz, bandwidthKHz;
  if (!cmd.getArgFloat(0, centerKHz) || !cmd.getArgFloat(1, bandwidthKHz))
  {
    return buildErrorResponse(
        ResponseStatus::ERR_SYNTAX,
        "Usage: MEASURE:SINC "
        "<center_kHz>,<bandwidth_kHz>[,<num_samples>,<decimation>,<amplitude>]");
  }

  // Optional parameters default values
  int numSamples = 8192;
  int decimation = 64;
  float amplitude = 1.0f;

  if (cmd.args.size() > 2 && !cmd.getArgInt(2, numSamples))
  {
    return buildErrorResponse(ResponseStatus::ERR_SYNTAX, "num_samples must be an integer");
  }
  if (!_parseMeasTailArgs(cmd, decimation, amplitude, error))
    return error;

  if (!_validateSampleCount(numSamples, error))
    return error;
  if (!_validateDecimation(decimation, error))
    return error;
  if (centerKHz <= 0.0f)
  {
    return buildErrorResponse(ResponseStatus::ERR_PARAM, "center_kHz must be > 0");
  }
  if (bandwidthKHz <= 0.0f)
  {
    return buildErrorResponse(ResponseStatus::ERR_PARAM, "bandwidth_kHz must be > 0");
  }
  if (!_validateAmplitude(amplitude, error))
    return error;

  uint16_t dec = static_cast<uint16_t>(decimation);
  double centerHz = centerKHz * 1000.0;
  double bandwidthHz = bandwidthKHz * 1000.0;

  if (bandwidthHz < 1.0)
  {
    return buildErrorResponse(ResponseStatus::ERR_PARAM, "bandwidth must be at least 1 Hz");
  }

  if (!_validateNyquist(centerHz + bandwidthHz / 2.0, dec, error))
    return error;

  if (!_checkAdcProtection(amplitude, error))
    return error;

  if (_applyDecimation(dec, error) <= 0.0)
    return error;

  // Track only after all validation passed: a rejected command must not
  // clear the saturation evidence of the previous acquisition
  _trackExcitationAmplitude(amplitude);

  auto signal = m_signalGen->generateSincSignal(static_cast<uint32_t>(numSamples),
                                                static_cast<uint32_t>(centerHz),
                                                static_cast<uint32_t>(bandwidthHz), amplitude);

  if (!m_hardware->loadGenerationSignal(signal))
  {
    return buildErrorResponse(ResponseStatus::ERR_HARDWARE, "Failed to load signal to FPGA");
  }

  ScopedMeasurement measurement(m_status, *m_hardware);
  if (!m_hardware->startMeasurement(static_cast<uint32_t>(numSamples), 0))
  {
    return buildErrorResponse(ResponseStatus::ERR_HARDWARE, "Failed to start measurement");
  }

  // Wait for completion (blocking)
  if (!_waitForMeasurement())
  {
    return buildErrorResponse(ResponseStatus::ERR_HARDWARE, "Measurement timeout");
  }

  std::vector<float> acquired(static_cast<size_t>(numSamples));
  if (!m_hardware->getAcquiredSignal(acquired))
  {
    return buildErrorResponse(ResponseStatus::ERR_HARDWARE, "Failed to read acquired data");
  }

  SaturationDetector::Report saturation = SaturationDetector::analyze(acquired);
  m_status.adcSaturated = saturation.saturated;
  m_status.adcSaturationRatio = saturation.saturatedRatio;

  // Compute FFT
  m_fftProcessor->applyWindow(acquired);
  auto fftResult = m_fftProcessor->computeFFT(acquired);
  auto magnitude = m_fftProcessor->computeMagnitudeSpectrum(fftResult);
  auto phase = m_fftProcessor->computePhaseSpectrum(fftResult);
  auto freqAxis = m_fftProcessor->getFrequencyAxis(static_cast<uint32_t>(numSamples));

  // Filter to bandwidth range
  double lowFreq = centerHz - bandwidthHz / 2.0;
  double highFreq = centerHz + bandwidthHz / 2.0;
  if (lowFreq < 0.0)
  {
    lowFreq = 0.0;
  }

  std::vector<SpectrumPoint> spectrum;
  for (size_t i = 0; i < freqAxis.size() && i < magnitude.size(); ++i)
  {
    if (freqAxis[i] >= lowFreq && freqAxis[i] <= highFreq)
    {
      spectrum.push_back({static_cast<float>(freqAxis[i] / 1000.0), magnitude[i], phase[i]});
    }
  }

  if (spectrum.empty())
  {
    return buildErrorResponse(ResponseStatus::ERR_PARAM, "No frequency bins in specified range");
  }

  return buildSpectrumResponse(spectrum);
}

std::string CommandHandler::_handleMeasSweep(const ParsedCommand& cmd)
{
  std::string error;
  if (!_checkInitialized(error))
    return error;

  // Parse arguments: MEASURE:SWEEP <center_kHz>,<range_kHz>[,<step_kHz>,<decimation>,<amplitude>]
  float centerKHz, rangeKHz;
  if (!cmd.getArgFloat(0, centerKHz) || !cmd.getArgFloat(1, rangeKHz))
  {
    return buildErrorResponse(
        ResponseStatus::ERR_SYNTAX,
        "Usage: MEASURE:SWEEP <center_kHz>,<range_kHz>[,<step_kHz>,<decimation>,<amplitude>]");
  }

  // Optional parameters with defaults
  float stepKHz = 1.0f;
  int decimation = 64;
  float amplitude = 1.0f;

  if (cmd.args.size() > 2 && !cmd.getArgFloat(2, stepKHz))
  {
    return buildErrorResponse(ResponseStatus::ERR_SYNTAX, "step_kHz must be a finite number");
  }
  if (!_parseMeasTailArgs(cmd, decimation, amplitude, error))
    return error;

  // Validate
  if (!_validateDecimation(decimation, error))
    return error;
  if (centerKHz <= 0.0f)
  {
    return buildErrorResponse(ResponseStatus::ERR_PARAM, "center_kHz must be > 0");
  }
  if (rangeKHz <= 0.0f)
  {
    return buildErrorResponse(ResponseStatus::ERR_PARAM, "range_kHz must be > 0");
  }
  if (stepKHz <= 0.0f)
  {
    return buildErrorResponse(ResponseStatus::ERR_PARAM, "step_kHz must be > 0");
  }
  if (!_validateAmplitude(amplitude, error))
    return error;

  uint16_t dec = static_cast<uint16_t>(decimation);

  // Compute frequency range
  double startKHz = centerKHz - rangeKHz / 2.0;
  double stopKHz = centerKHz + rangeKHz / 2.0;
  if (startKHz < 0.0)
    startKHz = 0.0;

  if (!_validateNyquist(stopKHz * 1000.0, dec, error))
    return error;

  // Relative tolerance: a float step such as 0.1 must not drop the stop frequency
  double pointsEstimate =
      std::floor((stopKHz - startKHz) / stepKHz * (1.0 + c_SweepStepTolerance)) + 1.0;
  if (pointsEstimate > static_cast<double>(ServerConfig::MAX_SWEEP_POINTS))
  {
    std::ostringstream oss;
    oss << "sweep would take ";
    if (pointsEstimate > 1.0e9)
    {
      oss << "more than 1e9";
    }
    else
    {
      oss << std::fixed << std::setprecision(0) << pointsEstimate;
    }
    oss << " points (max " << ServerConfig::MAX_SWEEP_POINTS << ")";
    return buildErrorResponse(ResponseStatus::ERR_PARAM, oss.str());
  }
  size_t numPoints = static_cast<size_t>(pointsEstimate);

  if (!_checkAdcProtection(amplitude, error))
    return error;

  const double samplingFreq = _applyDecimation(dec, error);
  if (samplingFreq <= 0.0)
    return error;

  // Track only after all validation passed: a rejected command must not
  // clear the saturation evidence of the previous acquisition
  _trackExcitationAmplitude(amplitude);

  // Use a fixed number of samples for each single-frequency measurement
  const uint32_t sweepSamples = 8192;
  ScopedMeasurement measurement(m_status, *m_hardware);

  std::vector<SpectrumPoint> spectrum;
  spectrum.reserve(numPoints);

  for (size_t point = 0; point < numPoints; ++point)
  {
    double freqKHz = std::min(startKHz + static_cast<double>(point) * stepKHz, stopKHz);
    double freqHz = freqKHz * 1000.0;

    auto signal = m_signalGen->generateSineWave(sweepSamples, freqHz, amplitude);

    if (!m_hardware->loadGenerationSignal(signal))
    {
      return buildErrorResponse(ResponseStatus::ERR_HARDWARE,
                                "Failed to load signal at " + std::to_string(freqKHz) + " kHz");
    }

    if (!m_hardware->startMeasurement(sweepSamples, 0))
    {
      return buildErrorResponse(ResponseStatus::ERR_HARDWARE, "Failed to start measurement at " +
                                                                  std::to_string(freqKHz) + " kHz");
    }

    if (!_waitForMeasurement())
    {
      return buildErrorResponse(ResponseStatus::ERR_HARDWARE,
                                "Measurement timeout at " + std::to_string(freqKHz) + " kHz");
    }

    std::vector<float> acquired(sweepSamples);
    if (!m_hardware->getAcquiredSignal(acquired))
    {
      return buildErrorResponse(ResponseStatus::ERR_HARDWARE,
                                "Failed to read data at " + std::to_string(freqKHz) + " kHz");
    }

    SaturationDetector::Report saturation = SaturationDetector::analyze(acquired);
    if (saturation.saturated)
    {
      m_status.adcSaturated = true;
    }
    m_status.adcSaturationRatio =
        std::max(m_status.adcSaturationRatio, saturation.saturatedRatio);

    // Compute amplitude and phase via sin/cos correlation
    double omega = 2.0 * M_PI * freqHz / samplingFreq;
    double sumSin = 0.0;
    double sumCos = 0.0;

    for (uint32_t i = 0; i < sweepSamples; ++i)
    {
      double t = static_cast<double>(i);
      sumSin += acquired[i] * std::sin(omega * t);
      sumCos += acquired[i] * std::cos(omega * t);
    }
    sumSin *= 2.0 / sweepSamples;
    sumCos *= 2.0 / sweepSamples;

    float magnitude = static_cast<float>(std::sqrt(sumSin * sumSin + sumCos * sumCos));
    // Phase of the response relative to the sine excitation (negative = lag)
    float phaseRad = static_cast<float>(std::atan2(sumCos, sumSin));

    spectrum.push_back({static_cast<float>(freqKHz), magnitude, phaseRad});

    m_hardware->resetMeasurement();
  }

  return buildSpectrumResponse(spectrum);
}

std::string CommandHandler::_handleUnknown(const ParsedCommand& cmd)
{
  return buildErrorResponse(ResponseStatus::ERR_SYNTAX, "Unknown command: " + cmd.rawLine);
}

std::string CommandHandler::_boardUnavailableError() const
{
  if (m_status.mode == OperatingMode::RP_ONLY)
  {
    return buildErrorResponse(ResponseStatus::ERR_HARDWARE,
                              "Electronic board disabled (RP-only mode)");
  }
  return buildErrorResponse(ResponseStatus::ERR_HARDWARE, "Board not connected");
}

bool CommandHandler::_checkInitialized(std::string& errorResponse)
{
  if (!m_status.hardwareInitialized)
  {
    errorResponse = buildErrorResponse(ResponseStatus::ERR_NOT_INIT,
                                       "Hardware not initialized. Use SYSTEM:INIT first");
    return false;
  }
  return true;
}

bool CommandHandler::_requireBoard(std::string& errorResponse)
{
  if (!_checkInitialized(errorResponse))
  {
    return false;
  }
  if (!m_status.boardConnected)
  {
    errorResponse = _boardUnavailableError();
    return false;
  }
  return true;
}

bool CommandHandler::_validateChannel1Based(int channel, const char* name,
                                            std::string& errorResponse)
{
  if (channel < 1 || channel > IElectronicBoard::NUM_CHANNELS)
  {
    errorResponse = buildErrorResponse(ResponseStatus::ERR_PARAM,
                                       std::string(name) + " must be 1-" +
                                           std::to_string(IElectronicBoard::NUM_CHANNELS));
    return false;
  }
  return true;
}

bool CommandHandler::_validateSampleCount(int numSamples, std::string& errorResponse)
{
  if (numSamples <= 0 || numSamples > static_cast<int>(IRedPitayaHardware::MAX_SAMPLES))
  {
    errorResponse = buildErrorResponse(ResponseStatus::ERR_PARAM,
                                       "num_samples must be 1-" +
                                           std::to_string(IRedPitayaHardware::MAX_SAMPLES));
    return false;
  }
  return true;
}

bool CommandHandler::_validateDecimation(int decimation, std::string& errorResponse)
{
  if (!HardwareLimits::isValidDecimation(decimation))
  {
    errorResponse =
        buildErrorResponse(ResponseStatus::ERR_PARAM,
                           "decimation must be a power of 2 (16, 32, 64, 128, 256, 512, 1024)");
    return false;
  }
  return true;
}

bool CommandHandler::_validateAmplitude(float amplitude, std::string& errorResponse)
{
  if (amplitude <= 0.0f || amplitude > 1.0f)
  {
    errorResponse = buildErrorResponse(ResponseStatus::ERR_PARAM, "amplitude must be in ]0, 1]");
    return false;
  }
  return true;
}

bool CommandHandler::_validateNyquist(double stopHz, uint16_t dec, std::string& errorResponse)
{
  double nyquistHz = ServerConfig::ADC_SAMPLE_RATE_HZ / (2.0 * dec);
  if (stopHz >= nyquistHz)
  {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(3) << "stop frequency exceeds Nyquist ("
        << nyquistHz / 1000.0 << " kHz) at decimation " << dec;
    errorResponse = buildErrorResponse(ResponseStatus::ERR_PARAM, oss.str());
    return false;
  }
  return true;
}

bool CommandHandler::_parseMeasTailArgs(const ParsedCommand& cmd, int& decimation,
                                        float& amplitude, std::string& errorResponse)
{
  if (cmd.args.size() > 3 && !cmd.getArgInt(3, decimation))
  {
    errorResponse =
        buildErrorResponse(ResponseStatus::ERR_SYNTAX, "decimation must be an integer");
    return false;
  }
  if (cmd.args.size() > 4 && !cmd.getArgFloat(4, amplitude))
  {
    errorResponse =
        buildErrorResponse(ResponseStatus::ERR_SYNTAX, "amplitude must be a finite number");
    return false;
  }
  return true;
}

void CommandHandler::_trackExcitationAmplitude(float amplitude)
{
  // Track the excitation for the ADC overdrive estimate; the estimate is
  // recomputed lazily by SYSTEM:STATUS because the DATA reply format must
  // stay unchanged for byte-count based clients.
  m_lastExcitationAmplitude = amplitude;
  m_status.adcSaturated = false;
  m_status.adcSaturationRatio = 0.0f;
}

double CommandHandler::_applyDecimation(uint16_t dec, std::string& errorResponse)
{
  if (!m_hardware->setDecimation(dec))
  {
    errorResponse = buildErrorResponse(ResponseStatus::ERR_HARDWARE, "Failed to set decimation");
    return 0.0;
  }
  m_status.currentDecimation = dec;

  // Recreate signal processing chain with updated sampling frequency
  double samplingFreq = ServerConfig::ADC_SAMPLE_RATE_HZ / dec;
  m_signalGen = std::make_unique<SignalGenerator>(samplingFreq, dec);
  m_fftProcessor = std::make_unique<FFTProcessor>(samplingFreq);
  m_resonanceAnalyzer = std::make_unique<ResonanceAnalyzer>(samplingFreq);
  return samplingFreq;
}

float CommandHandler::_loopPeak(float amplitude, uint8_t exciteChannel,
                                uint8_t returnChannel) const
{
  // Excited input routed straight to the ADC: one gain stage
  if (exciteChannel == returnChannel)
  {
    return InputSafety::boardDrivePeak(amplitude, m_channelGains[exciteChannel]);
  }
  return InputSafety::estimateAdcPeak(amplitude, m_channelGains[exciteChannel],
                                      m_channelGains[returnChannel], m_status.adcSampleGain);
}

CommandHandler::AdcLoopEstimate CommandHandler::_estimateAdcLoop(float amplitude) const
{
  AdcLoopEstimate estimate{false, false, true, 0, 0, 0.0f};

  if (m_status.loopMode == AdcLoopMode::OFF || !m_status.boardConnected)
  {
    return estimate;
  }
  estimate.active = true;

  if (m_status.loopMode == AdcLoopMode::MANUAL)
  {
    estimate.exciteChannel = m_status.loopExciteChannel;
    estimate.returnChannel = m_status.loopReturnChannel;
    estimate.peak = _loopPeak(amplitude, estimate.exciteChannel, estimate.returnChannel);
    return estimate;
  }

  // AUTO: excitation on the DAC link input, return = input routed to the ADC link output
  estimate.exciteChannel = InputSafety::DAC_LINK_INPUT;

  if (!m_muxRoutingKnown[InputSafety::ADC_LINK_OUTPUT])
  {
    // Unknown routing: assume the worst input
    estimate.returnKnown = false;
    for (uint8_t channel = 0; channel < IElectronicBoard::NUM_CHANNELS; ++channel)
    {
      const float peak = _loopPeak(amplitude, estimate.exciteChannel, channel);
      if (peak >= estimate.peak)
      {
        estimate.peak = peak;
        estimate.returnChannel = channel;
      }
    }
    return estimate;
  }

  const uint8_t routedInput = m_muxRouting[InputSafety::ADC_LINK_OUTPUT];
  if (routedInput >= IElectronicBoard::NUM_CHANNELS)
  {
    estimate.returnOpen = true;
    return estimate;
  }

  estimate.returnChannel = routedInput;
  estimate.peak = _loopPeak(amplitude, estimate.exciteChannel, estimate.returnChannel);
  return estimate;
}

std::string CommandHandler::_describeOverdrive(const AdcLoopEstimate& estimate,
                                               float amplitude) const
{
  const GainSetting exciteGain = m_channelGains[estimate.exciteChannel];
  const GainSetting returnGain = m_channelGains[estimate.returnChannel];
  const bool direct = estimate.exciteChannel == estimate.returnChannel;
  const int exciteLabel = static_cast<int>(estimate.exciteChannel + 1);
  const int returnLabel = static_cast<int>(estimate.returnChannel + 1);
  const float sampleGain = m_status.adcSampleGain;

  std::ostringstream oss;
  oss << std::fixed << std::setprecision(2) << "expected ADC peak ~" << estimate.peak
      << " V exceeds " << InputSafety::ADC_FULL_SCALE << " V full scale";
  if (!estimate.returnKnown)
  {
    oss << " (board routing unknown, worst case assumed)";
  }
  if (!direct && sampleGain != 1.0f)
  {
    oss << std::defaultfloat << std::setprecision(6) << " at sample gain " << sampleGain
        << std::fixed;
  }

  // Rounded down so the advice never exceeds the limit
  float gainProduct = InputSafety::DAC_FULL_SCALE * IElectronicBoard::gainFactor(exciteGain);
  if (!direct)
  {
    gainProduct *= sampleGain * IElectronicBoard::gainFactor(returnGain);
  }
  const float maxAmplitude =
      std::floor(InputSafety::ADC_FULL_SCALE / gainProduct * 10000.0f) / 10000.0f;
  oss << std::setprecision(4);

  GainSetting safeGain = GainSetting::GAIN_1;
  if (direct)
  {
    if (InputSafety::maxSafeDirectGain(amplitude, safeGain))
    {
      oss << "; suggested: BOARD:GAIN " << exciteLabel << ","
          << static_cast<int>(IElectronicBoard::gainToIndex(safeGain))
          << " or excitation amplitude <= " << maxAmplitude;
    }
    else
    {
      oss << "; reduce excitation amplitude to <= " << maxAmplitude;
    }
  }
  else if (InputSafety::maxSafeReturnGain(amplitude, exciteGain, safeGain, sampleGain))
  {
    oss << "; suggested: BOARD:GAIN " << returnLabel << ","
        << static_cast<int>(IElectronicBoard::gainToIndex(safeGain));
  }
  else if (InputSafety::maxSafeExciteGain(amplitude, returnGain, safeGain, sampleGain))
  {
    oss << "; suggested: BOARD:GAIN " << exciteLabel << ","
        << static_cast<int>(IElectronicBoard::gainToIndex(safeGain))
        << " or excitation amplitude <= " << maxAmplitude;
  }
  else
  {
    oss << "; reduce excitation amplitude to <= " << maxAmplitude;
  }
  return oss.str();
}

bool CommandHandler::_checkAdcProtection(float amplitude, std::string& errorResponse) const
{
  if (!m_status.adcProtectEnabled)
  {
    return true;
  }

  const AdcLoopEstimate estimate = _estimateAdcLoop(amplitude);
  if (!estimate.active || estimate.peak <= InputSafety::ADC_FULL_SCALE)
  {
    return true;
  }

  errorResponse = buildErrorResponse(ResponseStatus::ERR_PARAM,
                                     "ADC overdrive: " + _describeOverdrive(estimate, amplitude) +
                                         "; BOARD:ADC:PROTECT OFF overrides");
  return false;
}

bool CommandHandler::_updateAdcOverdriveEstimate()
{
  const AdcLoopEstimate estimate = _estimateAdcLoop(m_lastExcitationAmplitude);
  m_status.adcOverdrive = estimate.active && estimate.peak > InputSafety::ADC_FULL_SCALE;
  return m_status.adcOverdrive;
}

std::string CommandHandler::_buildOverdriveSuffix()
{
  if (!_updateAdcOverdriveEstimate())
  {
    return "";
  }
  const AdcLoopEstimate estimate = _estimateAdcLoop(m_lastExcitationAmplitude);
  return " WARN: " + _describeOverdrive(estimate, m_lastExcitationAmplitude);
}

std::string CommandHandler::_buildOkWithOverdriveWarning()
{
  const std::string warn = _buildOverdriveSuffix();
  return buildOkResponse(warn.empty() ? "" : warn.substr(1));
}

void CommandHandler::_refreshBoardCacheFromBoard()
{
  // Fail-safe seed: unknown gain = x16, unknown routing = worst input
  for (size_t i = 0; i < IElectronicBoard::NUM_CHANNELS; ++i)
  {
    m_channelGains[i] = GainSetting::GAIN_16;
    m_muxRoutingKnown[i] = false;
  }

  if (!m_board)
  {
    return;
  }

  std::array<GainSetting, IElectronicBoard::NUM_CHANNELS> gains;
  std::array<bool, IElectronicBoard::NUM_CHANNELS> gainValid;
  // Value-initialize before the call: mocks (and gtest argument printing)
  // may observe the buffers even when the query fails
  gains.fill(GainSetting::GAIN_1);
  gainValid.fill(false);
  if (m_board->queryGains(gains, gainValid))
  {
    for (size_t i = 0; i < IElectronicBoard::NUM_CHANNELS; ++i)
    {
      m_channelGains[i] = gainValid[i] ? gains[i] : GainSetting::GAIN_16;
    }
  }

  std::array<uint8_t, IElectronicBoard::NUM_CHANNELS> routing;
  std::array<bool, IElectronicBoard::NUM_CHANNELS> routingValid;
  routing.fill(IElectronicBoard::DISCONNECTED);
  routingValid.fill(false);
  if (m_board->queryRouting(routing, routingValid))
  {
    for (size_t i = 0; i < IElectronicBoard::NUM_CHANNELS; ++i)
    {
      m_muxRouting[i] = routing[i];
      m_muxRoutingKnown[i] = routingValid[i];
    }
  }
}

void CommandHandler::_refreshBoardCacheFromStatus(const std::string& statusText)
{
  // Take over only what parses; the rest keeps its cached value
  GainSetting gains[IElectronicBoard::NUM_CHANNELS];
  bool gainValid[IElectronicBoard::NUM_CHANNELS];
  for (auto& gain : gains)
  {
    gain = GainSetting::GAIN_16;
  }
  if (BoardProtocol::parseGainStatus(statusText, gains, gainValid,
                                     IElectronicBoard::NUM_CHANNELS))
  {
    for (size_t i = 0; i < IElectronicBoard::NUM_CHANNELS; ++i)
    {
      if (gainValid[i])
      {
        m_channelGains[i] = gains[i];
      }
    }
  }

  uint8_t routing[IElectronicBoard::NUM_CHANNELS];
  bool routingValid[IElectronicBoard::NUM_CHANNELS];
  for (auto& route : routing)
  {
    route = IElectronicBoard::DISCONNECTED;
  }
  if (BoardProtocol::parseRoutingStatus(statusText, routing, routingValid,
                                        IElectronicBoard::NUM_CHANNELS))
  {
    for (size_t i = 0; i < IElectronicBoard::NUM_CHANNELS; ++i)
    {
      if (routingValid[i])
      {
        m_muxRouting[i] = routing[i];
        m_muxRoutingKnown[i] = true;
      }
    }
  }
}

void CommandHandler::_resetBoardCache()
{
  // Board defaults: unity gains, outputs disconnected
  for (size_t i = 0; i < IElectronicBoard::NUM_CHANNELS; ++i)
  {
    m_channelGains[i] = GainSetting::GAIN_1;
    m_muxRouting[i] = IElectronicBoard::DISCONNECTED;
    m_muxRoutingKnown[i] = true;
  }
}

bool CommandHandler::_waitForMeasurement()
{
  auto start = std::chrono::steady_clock::now();
  while (!m_hardware->isMeasurementComplete())
  {
    auto elapsed = std::chrono::steady_clock::now() - start;
    if (std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() >=
        m_measurementTimeoutMs)
    {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return true;
}

} // namespace AFM
