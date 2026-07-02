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
#include <iostream>
#include <sstream>
#include <thread>

namespace AFM
{

CommandHandler::CommandHandler()
    : m_hardware(nullptr)
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

  // CALIBRATE subsystem
  case Command::CAL_RUN:
    return _handleCalRun(cmd);
  case Command::CAL_STATUS:
    return _handleCalStatus(cmd);
  case Command::CAL_CLEAR:
    return _handleCalClear(cmd);

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
  m_calibration.reset();
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
      << " CAL=" << (m_status.calibrationActive ? "1" : "0")
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
  m_hardware = std::make_unique<RedPitayaHardware>();
  if (!m_hardware->initialize())
  {
    m_hardware.reset();
    return buildErrorResponse(ResponseStatus::ERR_HARDWARE, "Failed to initialize Red Pitaya");
  }

  // Initialize electronic board
  m_board = std::make_unique<ElectronicBoardUART>();
  if (!m_board->initialize())
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
  double samplingFreq = 125000000.0 / m_status.currentDecimation;
  m_signalGen = std::make_unique<SignalGenerator>(samplingFreq, m_status.currentDecimation);

  // Initialize calibration manager
  m_calibration = std::make_unique<CalibrationManager>();

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
  m_calibration.reset();
  m_status.hardwareInitialized = false;
  m_status.boardConnected = false;
  m_status.calibrationActive = false;

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
  if (output < 1 || output > ElectronicBoardUART::NUM_CHANNELS || input < 1 ||
      input > ElectronicBoardUART::NUM_CHANNELS)
  {
    return buildErrorResponse(ResponseStatus::ERR_PARAM,
                              "out and in must be 1-" +
                                  std::to_string(ElectronicBoardUART::NUM_CHANNELS));
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
  if (output < 1 || output > ElectronicBoardUART::NUM_CHANNELS)
  {
    return buildErrorResponse(ResponseStatus::ERR_PARAM,
                              "out must be 1-" +
                                  std::to_string(ElectronicBoardUART::NUM_CHANNELS));
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
  if (channel < 1 || channel > ElectronicBoardUART::NUM_CHANNELS)
  {
    return buildErrorResponse(ResponseStatus::ERR_PARAM,
                              "channel must be 1-" +
                                  std::to_string(ElectronicBoardUART::NUM_CHANNELS));
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

  return buildOkResponse(ElectronicBoardUART::gainToString(gain));
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

  cmd.getArgInt(2, numSamples);
  cmd.getArgInt(3, decimation);
  cmd.getArgFloat(4, amplitude);

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
  if (!m_hardware->setDecimation(dec))
  {
    return buildErrorResponse(ResponseStatus::ERR_HARDWARE, "Failed to set decimation");
  }
  m_status.currentDecimation = dec;

  // Recreate signal processing chain with updated sampling frequency
  double samplingFreq = 125000000.0 / dec;
  m_signalGen = std::make_unique<SignalGenerator>(samplingFreq, dec);
  m_fftProcessor = std::make_unique<FFTProcessor>(samplingFreq);
  m_resonanceAnalyzer = std::make_unique<ResonanceAnalyzer>(samplingFreq);

  double centerHz = centerKHz * 1000.0;
  double bandwidthHz = bandwidthKHz * 1000.0;

  auto signal = m_signalGen->generateSincSignal(static_cast<uint32_t>(numSamples),
                                                static_cast<uint32_t>(centerHz),
                                                static_cast<uint32_t>(bandwidthHz), amplitude);

  if (!m_hardware->loadGenerationSignal(signal))
  {
    return buildErrorResponse(ResponseStatus::ERR_HARDWARE, "Failed to load signal to FPGA");
  }

  m_status.measurementInProgress = true;
  if (!m_hardware->startMeasurement(static_cast<uint32_t>(numSamples), 0))
  {
    m_status.measurementInProgress = false;
    return buildErrorResponse(ResponseStatus::ERR_HARDWARE, "Failed to start measurement");
  }

  // Wait for completion (blocking)
  if (!_waitForMeasurement())
  {
    m_status.measurementInProgress = false;
    m_hardware->resetMeasurement();
    return buildErrorResponse(ResponseStatus::ERR_HARDWARE, "Measurement timeout");
  }

  std::vector<float> acquired(static_cast<size_t>(numSamples));
  if (!m_hardware->getAcquiredSignal(acquired))
  {
    m_status.measurementInProgress = false;
    m_hardware->resetMeasurement();
    return buildErrorResponse(ResponseStatus::ERR_HARDWARE, "Failed to read acquired data");
  }

  m_status.measurementInProgress = false;
  m_hardware->resetMeasurement();

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

  // Apply calibration if available and valid
  if (m_calibration && m_calibration->isCalibrated())
  {
    CalibrationParams currentParams;
    currentParams.decimation = dec;
    currentParams.numSamples = static_cast<uint32_t>(numSamples);
    currentParams.centerKHz = centerKHz;
    currentParams.bandwidthKHz = bandwidthKHz;
    currentParams.amplitude = amplitude;

    if (m_calibration->isValidFor(currentParams))
    {
      std::vector<float> freqs, mags, phases;
      freqs.reserve(spectrum.size());
      mags.reserve(spectrum.size());
      phases.reserve(spectrum.size());

      for (const auto& pt : spectrum)
      {
        freqs.push_back(pt.freqKHz);
        mags.push_back(pt.magnitude);
        phases.push_back(pt.phaseRad);
      }

      m_calibration->applyCalibration(mags, phases, freqs);

      for (size_t i = 0; i < spectrum.size(); ++i)
      {
        spectrum[i].magnitude = mags[i];
        spectrum[i].phaseRad = phases[i];
      }
    }
    else
    {
      std::cout << "[CommandHandler] Warning: Calibration parameters mismatch, "
                << "skipping calibration. Re-calibrate with matching settings." << std::endl;
    }
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

  cmd.getArgFloat(2, stepKHz);
  cmd.getArgInt(3, decimation);
  cmd.getArgFloat(4, amplitude);

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
  if (!m_hardware->setDecimation(dec))
  {
    return buildErrorResponse(ResponseStatus::ERR_HARDWARE, "Failed to set decimation");
  }
  m_status.currentDecimation = dec;

  // Recreate signal generator with updated sampling frequency
  double samplingFreq = 125000000.0 / dec;
  m_signalGen = std::make_unique<SignalGenerator>(samplingFreq, dec);

  // Compute frequency range
  double startKHz = centerKHz - rangeKHz / 2.0;
  double stopKHz = centerKHz + rangeKHz / 2.0;
  if (startKHz < 0.0)
    startKHz = 0.0;

  // Use a fixed number of samples for each single-frequency measurement
  const uint32_t sweepSamples = 8192;
  m_status.measurementInProgress = true;

  std::vector<SpectrumPoint> spectrum;

  for (double freqKHz = startKHz; freqKHz <= stopKHz; freqKHz += stepKHz)
  {
    double freqHz = freqKHz * 1000.0;

    auto signal = m_signalGen->generateSineWave(sweepSamples, freqHz, amplitude);

    if (!m_hardware->loadGenerationSignal(signal))
    {
      m_status.measurementInProgress = false;
      return buildErrorResponse(ResponseStatus::ERR_HARDWARE,
                                "Failed to load signal at " + std::to_string(freqKHz) + " kHz");
    }

    if (!m_hardware->startMeasurement(sweepSamples, 0))
    {
      m_status.measurementInProgress = false;
      return buildErrorResponse(ResponseStatus::ERR_HARDWARE, "Failed to start measurement at " +
                                                                  std::to_string(freqKHz) + " kHz");
    }

    if (!_waitForMeasurement())
    {
      m_status.measurementInProgress = false;
      return buildErrorResponse(ResponseStatus::ERR_HARDWARE,
                                "Measurement timeout at " + std::to_string(freqKHz) + " kHz");
    }

    std::vector<float> acquired(sweepSamples);
    if (!m_hardware->getAcquiredSignal(acquired))
    {
      m_status.measurementInProgress = false;
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

  m_status.measurementInProgress = false;

  return buildSpectrumResponse(spectrum);
}

// ---- CALIBRATE subsystem ----

std::string CommandHandler::_handleCalRun(const ParsedCommand& cmd)
{
  std::string error;
  if (!_checkInitialized(error))
  {
    return error;
  }

  // Parse arguments: CALIBRATE:RUN
  // <center_kHz>,<bandwidth_kHz>[,<num_samples>,<decimation>,<amplitude>]
  float centerKHz, bandwidthKHz;
  if (!cmd.getArgFloat(0, centerKHz) || !cmd.getArgFloat(1, bandwidthKHz))
  {
    return buildErrorResponse(
        ResponseStatus::ERR_SYNTAX,
        "Usage: CALIBRATE:RUN "
        "<center_kHz>,<bandwidth_kHz>[,<num_samples>,<decimation>,<amplitude>]");
  }

  // Optional parameters with defaults
  int numSamples = 8192;
  int decimation = 64;
  float amplitude = 1.0f;

  cmd.getArgInt(2, numSamples);
  cmd.getArgInt(3, decimation);
  cmd.getArgFloat(4, amplitude);

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
  if (!m_hardware->setDecimation(dec))
  {
    return buildErrorResponse(ResponseStatus::ERR_HARDWARE, "Failed to set decimation");
  }
  m_status.currentDecimation = dec;

  // Recreate signal processing chain with updated sampling frequency
  double samplingFreq = 125000000.0 / dec;
  m_signalGen = std::make_unique<SignalGenerator>(samplingFreq, dec);
  m_fftProcessor = std::make_unique<FFTProcessor>(samplingFreq);

  double centerHz = centerKHz * 1000.0;
  double bandwidthHz = bandwidthKHz * 1000.0;

  auto signal = m_signalGen->generateSincSignal(static_cast<uint32_t>(numSamples),
                                                static_cast<uint32_t>(centerHz),
                                                static_cast<uint32_t>(bandwidthHz), amplitude);

  if (!m_hardware->loadGenerationSignal(signal))
  {
    return buildErrorResponse(ResponseStatus::ERR_HARDWARE, "Failed to load calibration signal");
  }

  m_status.measurementInProgress = true;
  if (!m_hardware->startMeasurement(static_cast<uint32_t>(numSamples), 0))
  {
    m_status.measurementInProgress = false;
    return buildErrorResponse(ResponseStatus::ERR_HARDWARE,
                              "Failed to start calibration measurement");
  }

  if (!_waitForMeasurement())
  {
    m_status.measurementInProgress = false;
    m_hardware->resetMeasurement();
    return buildErrorResponse(ResponseStatus::ERR_HARDWARE, "Calibration measurement timeout");
  }

  std::vector<float> acquired(static_cast<size_t>(numSamples));
  if (!m_hardware->getAcquiredSignal(acquired))
  {
    m_status.measurementInProgress = false;
    m_hardware->resetMeasurement();
    return buildErrorResponse(ResponseStatus::ERR_HARDWARE, "Failed to read calibration data");
  }

  m_status.measurementInProgress = false;
  m_hardware->resetMeasurement();

  // Compute FFT of the loopback response
  m_fftProcessor->applyWindow(acquired);
  auto fftResult = m_fftProcessor->computeFFT(acquired);
  auto magnitude = m_fftProcessor->computeMagnitudeSpectrum(fftResult);
  auto phase = m_fftProcessor->computePhaseSpectrum(fftResult);
  auto freqAxis = m_fftProcessor->getFrequencyAxis(static_cast<uint32_t>(numSamples));

  // Filter to bandwidth range (same logic as MEASURE:SINC)
  double lowFreq = centerHz - bandwidthHz / 2.0;
  double highFreq = centerHz + bandwidthHz / 2.0;
  if (lowFreq < 0.0)
  {
    lowFreq = 0.0;
  }

  std::vector<float> calFreqKHz;
  std::vector<float> calMagnitude;
  std::vector<float> calPhaseRad;

  for (size_t i = 0; i < freqAxis.size() && i < magnitude.size(); ++i)
  {
    if (freqAxis[i] >= lowFreq && freqAxis[i] <= highFreq)
    {
      calFreqKHz.push_back(static_cast<float>(freqAxis[i] / 1000.0));
      calMagnitude.push_back(magnitude[i]);
      calPhaseRad.push_back(phase[i]);
    }
  }

  if (calFreqKHz.empty())
  {
    return buildErrorResponse(ResponseStatus::ERR_PARAM,
                              "No frequency bins in specified calibration range");
  }

  // Store the calibration reference
  CalibrationParams params;
  params.decimation = dec;
  params.numSamples = static_cast<uint32_t>(numSamples);
  params.centerKHz = centerKHz;
  params.bandwidthKHz = bandwidthKHz;
  params.amplitude = amplitude;

  m_calibration->setCalibration(calFreqKHz, calMagnitude, calPhaseRad, params);
  m_status.calibrationActive = true;

  std::ostringstream oss;
  oss << "CALIBRATED " << calFreqKHz.size() << " points";
  return buildOkResponse(oss.str());
}

std::string CommandHandler::_handleCalStatus(const ParsedCommand& cmd)
{
  (void)cmd;

  if (!m_calibration)
  {
    return buildOkResponse("NOT_CALIBRATED");
  }

  return buildOkResponse(m_calibration->statusString());
}

std::string CommandHandler::_handleCalClear(const ParsedCommand& cmd)
{
  (void)cmd;

  if (m_calibration)
  {
    m_calibration->clearCalibration();
  }
  m_status.calibrationActive = false;

  return buildOkResponse("Calibration cleared");
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
  if (numSamples <= 0 || numSamples > static_cast<int>(RedPitayaHardware::MAX_SAMPLES))
  {
    errorResponse = buildErrorResponse(ResponseStatus::ERR_PARAM,
                                       "num_samples must be 1-" +
                                           std::to_string(RedPitayaHardware::MAX_SAMPLES));
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

bool CommandHandler::_waitForMeasurement(int timeoutMs)
{
  auto start = std::chrono::steady_clock::now();
  while (!m_hardware->isMeasurementComplete())
  {
    auto elapsed = std::chrono::steady_clock::now() - start;
    if (std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() >= timeoutMs)
    {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return true;
}

} // namespace AFM
