/**
 * @file CommandHandler.cpp
 * @brief Command handler implementation for AFM measurement server
 *
 * @date December 2025
 */

#include "CommandHandler.h"

#include "Protocol.h"

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
                               int measurementTimeoutMs)
    : m_hardwareFactory(std::move(hardwareFactory))
    , m_boardFactory(std::move(boardFactory))
    , m_measurementTimeoutMs(measurementTimeoutMs)
    , m_hardware(nullptr)
    , m_board(nullptr)
    , m_signalGen(nullptr)
    , m_fftProcessor(nullptr)
    , m_resonanceAnalyzer(nullptr)
{
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

  // BOARD subsystem
  case Command::BOARD_MUX_ROUTE:
    return _handleBoardMuxRoute(cmd);
  case Command::BOARD_MUX_DISCONNECT:
    return _handleBoardMuxDisconnect(cmd);
  case Command::BOARD_GAIN:
    return _handleBoardGain(cmd);
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
  m_status = SystemStatus();
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

std::string CommandHandler::_handleSystStatus(const ParsedCommand& cmd)
{
  (void)cmd;

  std::ostringstream oss;
  oss << "HW_INIT=" << (m_status.hardwareInitialized ? "1" : "0")
      << " BOARD=" << (m_status.boardConnected ? "1" : "0")
      << " BUSY=" << (m_status.measurementInProgress ? "1" : "0")
      << " DEC=" << m_status.currentDecimation;

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

  // Initialize electronic board
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

  // Initialize signal generator with current decimation
  m_status.currentDecimation = m_hardware->getDecimation();
  double samplingFreq = ServerConfig::ADC_SAMPLE_RATE_HZ / m_status.currentDecimation;
  m_signalGen = std::make_unique<SignalGenerator>(samplingFreq, m_status.currentDecimation);

  m_status.hardwareInitialized = true;

  std::ostringstream oss;
  oss << "Red Pitaya OK, Board " << (m_status.boardConnected ? "OK" : "NOT CONNECTED");
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

  return buildOkResponse("Hardware deinitialized");
}

// ---- BOARD subsystem ----

std::string CommandHandler::_handleBoardMuxRoute(const ParsedCommand& cmd)
{
  std::string error;
  if (!_checkInitialized(error))
    return error;
  if (!m_status.boardConnected)
  {
    return buildErrorResponse(ResponseStatus::ERR_HARDWARE, "Board not connected");
  }

  int output, input;
  if (!cmd.getArgInt(0, output) || !cmd.getArgInt(1, input))
  {
    return buildErrorResponse(ResponseStatus::ERR_SYNTAX, "Usage: BOARD:MUX:ROUTE <out>,<in>");
  }

  // Accept 1-based channel numbers matching board connector labels (1-4)
  if (output < 1 || output > IElectronicBoard::NUM_CHANNELS || input < 1 ||
      input > IElectronicBoard::NUM_CHANNELS)
  {
    return buildErrorResponse(ResponseStatus::ERR_PARAM,
                              "out and in must be 1-" +
                                  std::to_string(IElectronicBoard::NUM_CHANNELS));
  }

  // Convert to 0-indexed for internal use
  if (!m_board->setMuxRoute(static_cast<uint8_t>(output - 1), static_cast<uint8_t>(input - 1)))
  {
    return buildErrorResponse(ResponseStatus::ERR_HARDWARE, m_board->getLastError());
  }

  return buildOkResponse();
}

std::string CommandHandler::_handleBoardMuxDisconnect(const ParsedCommand& cmd)
{
  std::string error;
  if (!_checkInitialized(error))
    return error;
  if (!m_status.boardConnected)
  {
    return buildErrorResponse(ResponseStatus::ERR_HARDWARE, "Board not connected");
  }

  int output;
  if (!cmd.getArgInt(0, output))
  {
    return buildErrorResponse(ResponseStatus::ERR_SYNTAX, "Usage: BOARD:MUX:DISCONNECT <out>");
  }

  // Accept 1-based channel number matching board connector label (1-4)
  if (output < 1 || output > IElectronicBoard::NUM_CHANNELS)
  {
    return buildErrorResponse(ResponseStatus::ERR_PARAM,
                              "out must be 1-" +
                                  std::to_string(IElectronicBoard::NUM_CHANNELS));
  }

  // Convert to 0-indexed for internal use
  if (!m_board->disconnectMux(static_cast<uint8_t>(output - 1)))
  {
    return buildErrorResponse(ResponseStatus::ERR_HARDWARE, m_board->getLastError());
  }

  return buildOkResponse();
}

std::string CommandHandler::_handleBoardGain(const ParsedCommand& cmd)
{
  std::string error;
  if (!_checkInitialized(error))
    return error;
  if (!m_status.boardConnected)
  {
    return buildErrorResponse(ResponseStatus::ERR_HARDWARE, "Board not connected");
  }

  int channel, gainIndex;
  if (!cmd.getArgInt(0, channel) || !cmd.getArgInt(1, gainIndex))
  {
    return buildErrorResponse(ResponseStatus::ERR_SYNTAX, "Usage: BOARD:GAIN <ch>,<gain_index>");
  }

  // Accept 1-based channel number matching board connector label (1-4)
  if (channel < 1 || channel > IElectronicBoard::NUM_CHANNELS)
  {
    return buildErrorResponse(ResponseStatus::ERR_PARAM,
                              "channel must be 1-" +
                                  std::to_string(IElectronicBoard::NUM_CHANNELS));
  }

  if (gainIndex < 0 || gainIndex > 7)
  {
    return buildErrorResponse(ResponseStatus::ERR_PARAM, "gain_index must be 0-7");
  }

  // Convert to 0-indexed for internal use
  GainSetting gain = static_cast<GainSetting>(gainIndex);
  if (!m_board->setGain(static_cast<uint8_t>(channel - 1), gain))
  {
    return buildErrorResponse(ResponseStatus::ERR_HARDWARE, m_board->getLastError());
  }

  return buildOkResponse(IElectronicBoard::gainToString(gain));
}

std::string CommandHandler::_handleBoardReset(const ParsedCommand& cmd)
{
  (void)cmd;

  std::string error;
  if (!_checkInitialized(error))
    return error;
  if (!m_status.boardConnected)
  {
    return buildErrorResponse(ResponseStatus::ERR_HARDWARE, "Board not connected");
  }

  if (!m_board->reset())
  {
    return buildErrorResponse(ResponseStatus::ERR_HARDWARE, m_board->getLastError());
  }

  return buildOkResponse("Board reset complete");
}

std::string CommandHandler::_handleBoardStatus(const ParsedCommand& cmd)
{
  (void)cmd;

  std::string error;
  if (!_checkInitialized(error))
    return error;
  if (!m_status.boardConnected)
  {
    return buildErrorResponse(ResponseStatus::ERR_HARDWARE, "Board not connected");
  }

  std::string boardStatus;
  if (!m_board->getStatus(boardStatus))
  {
    return buildErrorResponse(ResponseStatus::ERR_HARDWARE, m_board->getLastError());
  }

  return buildOkResponse(boardStatus);
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
  if (cmd.args.size() > 3 && !cmd.getArgInt(3, decimation))
  {
    return buildErrorResponse(ResponseStatus::ERR_SYNTAX, "decimation must be an integer");
  }
  if (cmd.args.size() > 4 && !cmd.getArgFloat(4, amplitude))
  {
    return buildErrorResponse(ResponseStatus::ERR_SYNTAX, "amplitude must be a finite number");
  }

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
  if (amplitude <= 0.0f || amplitude > 1.0f)
  {
    return buildErrorResponse(ResponseStatus::ERR_PARAM, "amplitude must be in ]0, 1]");
  }

  uint16_t dec = static_cast<uint16_t>(decimation);
  double centerHz = centerKHz * 1000.0;
  double bandwidthHz = bandwidthKHz * 1000.0;
  double nyquistHz = ServerConfig::ADC_SAMPLE_RATE_HZ / (2.0 * dec);

  if (bandwidthHz < 1.0)
  {
    return buildErrorResponse(ResponseStatus::ERR_PARAM, "bandwidth must be at least 1 Hz");
  }

  if (centerHz + bandwidthHz / 2.0 >= nyquistHz)
  {
    std::ostringstream oss;
    oss << "excitation band exceeds Nyquist (" << std::fixed << std::setprecision(3)
        << nyquistHz / 1000.0 << " kHz) at decimation " << dec;
    return buildErrorResponse(ResponseStatus::ERR_PARAM, oss.str());
  }

  if (!m_hardware->setDecimation(dec))
  {
    return buildErrorResponse(ResponseStatus::ERR_HARDWARE, "Failed to set decimation");
  }
  m_status.currentDecimation = dec;

  // Recreate signal processing chain with updated sampling frequency
  double samplingFreq = ServerConfig::ADC_SAMPLE_RATE_HZ / dec;
  m_signalGen = std::make_unique<SignalGenerator>(samplingFreq, dec);
  m_fftProcessor = std::make_unique<FFTProcessor>(samplingFreq);
  m_resonanceAnalyzer = std::make_unique<ResonanceAnalyzer>(samplingFreq);

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
  if (cmd.args.size() > 3 && !cmd.getArgInt(3, decimation))
  {
    return buildErrorResponse(ResponseStatus::ERR_SYNTAX, "decimation must be an integer");
  }
  if (cmd.args.size() > 4 && !cmd.getArgFloat(4, amplitude))
  {
    return buildErrorResponse(ResponseStatus::ERR_SYNTAX, "amplitude must be a finite number");
  }

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
  if (amplitude <= 0.0f || amplitude > 1.0f)
  {
    return buildErrorResponse(ResponseStatus::ERR_PARAM, "amplitude must be in ]0, 1]");
  }

  uint16_t dec = static_cast<uint16_t>(decimation);
  double nyquistKHz = ServerConfig::ADC_SAMPLE_RATE_HZ / (2.0 * dec) / 1000.0;

  // Compute frequency range
  double startKHz = centerKHz - rangeKHz / 2.0;
  double stopKHz = centerKHz + rangeKHz / 2.0;
  if (startKHz < 0.0)
    startKHz = 0.0;

  if (stopKHz >= nyquistKHz)
  {
    std::ostringstream oss;
    oss << "sweep stop frequency exceeds Nyquist (" << std::fixed << std::setprecision(3)
        << nyquistKHz << " kHz) at decimation " << dec;
    return buildErrorResponse(ResponseStatus::ERR_PARAM, oss.str());
  }

  double pointsEstimate = std::floor((stopKHz - startKHz) / stepKHz) + 1.0;
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

  if (!m_hardware->setDecimation(dec))
  {
    return buildErrorResponse(ResponseStatus::ERR_HARDWARE, "Failed to set decimation");
  }
  m_status.currentDecimation = dec;

  // Recreate signal generator with updated sampling frequency
  double samplingFreq = ServerConfig::ADC_SAMPLE_RATE_HZ / dec;
  m_signalGen = std::make_unique<SignalGenerator>(samplingFreq, dec);

  // Use a fixed number of samples for each single-frequency measurement
  const uint32_t sweepSamples = 8192;
  ScopedMeasurement measurement(m_status, *m_hardware);

  std::vector<SpectrumPoint> spectrum;
  spectrum.reserve(numPoints);

  for (size_t point = 0; point < numPoints; ++point)
  {
    double freqKHz = startKHz + static_cast<double>(point) * stepKHz;
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
    float phaseRad = static_cast<float>(std::atan2(sumSin, sumCos));

    spectrum.push_back({static_cast<float>(freqKHz), magnitude, phaseRad});

    m_hardware->resetMeasurement();
  }

  return buildSpectrumResponse(spectrum);
}

std::string CommandHandler::_handleUnknown(const ParsedCommand& cmd)
{
  return buildErrorResponse(ResponseStatus::ERR_SYNTAX, "Unknown command: " + cmd.rawLine);
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
  if (decimation < 16 || decimation > 1024)
  {
    errorResponse =
        buildErrorResponse(ResponseStatus::ERR_PARAM, "decimation must be 16-1024 (power of 2)");
    return false;
  }
  if ((decimation & (decimation - 1)) != 0)
  {
    errorResponse =
        buildErrorResponse(ResponseStatus::ERR_PARAM,
                           "decimation must be a power of 2 (16, 32, 64, 128, 256, 512, 1024)");
    return false;
  }
  return true;
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
