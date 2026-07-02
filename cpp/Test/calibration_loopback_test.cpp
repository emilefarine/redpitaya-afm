#include "CalibrationManager.h"
#include "FFTProcessor.h"
#include "RedPitayaHardware.h"
#include "SignalGenerator.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <string>
#include <thread>
#include <vector>

/**
 * @brief Calibration loopback comparison test (DAC OUT1 -> board -> ADC IN1).
 *
 * Exercises CalibrationManager against real hardware without the server/TCP
 * layer.  The flow mirrors what the server does in _handleCalRun and
 * _handleMeasSinc:
 *
 *   Step A  Run a loopback sinc measurement, store its spectrum as the
 *           calibration reference H_ref(f).
 *   Step B  Run a second loopback sinc measurement (separate acquisition) and
 *           keep its raw spectrum -- this is the "uncalibrated" result.
 *   Step C  Apply the stored calibration to a copy of the raw spectrum.
 *   Step D  Compute flatness/phase metrics on both and print a comparison.
 *
 * In an ideal loopback the calibrated transfer function should be flat
 * (magnitude ~ 1.0, phase ~ 0).  The raw spectrum carries the DAC/ADC
 * reconstruction-filter ripple, cable delay, and board (PGA/MUX) response;
 * dividing by H_ref removes them, so the calibrated spectrum should be
 * measurably flatter than the raw one.
 *
 * Hardware setup:
 *   - Connect OUT1 (DAC) -> electronic board -> IN1 (ADC).
 *   - Ensure the FPGA is programmed with the correct bitfile.
 *
 * Note: CalibrationManager operates entirely in kHz.  The FFT frequency axis
 * is in Hz, so we convert to kHz when building the spectrum (same as the
 * server's SpectrumPoint construction).
 */

static constexpr float PI_F = 3.14159265358979323846f;

// --- Configuration -----------------------------------------------------------
static constexpr double RP_MAX_FREQ = 125000000.0; // 125 MHz ADC/DAC clock
static constexpr uint16_t DECIMATION = 64;
static constexpr uint32_t NUM_SAMPLES = 8192;
static constexpr uint32_t CENTER_FREQ = 500000; // 500 kHz
static constexpr uint32_t BANDWIDTH = 200000;   // 200 kHz
static constexpr float AMPLITUDE = 1.0f;
static constexpr long MEAS_TIMEOUT_MS = 1000;

struct SpectrumData
{
  std::vector<float> freqKHz;
  std::vector<float> magnitude;
  std::vector<float> phaseRad;

  size_t size() const
  {
    return freqKHz.size();
  }
  bool empty() const
  {
    return freqKHz.empty();
  }
};

struct Metrics
{
  float magMean;
  float magStd;
  float magFlatnessDb; // 20*log10(max/min) across the band
  float phaseStd;      // rad
  float phaseMaxDev;   // rad, worst-case deviation from mean
};

/** @brief Wrap an angle into (-pi, pi].  Used for phase-difference statistics. */
static float wrapToPi(float x)
{
  while (x > PI_F)
  {
    x -= 2.0f * PI_F;
  }
  while (x < -PI_F)
  {
    x += 2.0f * PI_F;
  }
  return x;
}

/**
 * @brief Run one loopback measurement: load -> start -> wait -> read -> reset.
 *
 * resetMeasurement() is called at the end (and on every error path) so the
 * next measurement starts from a clean FSM state -- the same discipline the
 * server uses between runs.
 */
static bool doMeasurement(RedPitayaHardware& hw,
                          const std::vector<float>& signal,
                          uint32_t numSamples,
                          std::vector<float>& acquired)
{
  if (!hw.loadGenerationSignal(signal))
  {
    std::cerr << "  ! Failed to load generation signal" << std::endl;
    return false;
  }

  if (!hw.startMeasurement(numSamples, 0))
  {
    std::cerr << "  ! Failed to start measurement" << std::endl;
    hw.resetMeasurement();
    return false;
  }

  auto startTime = std::chrono::steady_clock::now();
  while (!hw.isMeasurementComplete())
  {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - startTime)
                       .count();
    if (elapsed > MEAS_TIMEOUT_MS)
    {
      std::cerr << "  ! Measurement timeout" << std::endl;
      hw.resetMeasurement();
      return false;
    }
  }

  acquired.assign(numSamples, 0.0f);
  if (!hw.getAcquiredSignal(acquired))
  {
    std::cerr << "  ! Failed to read acquired signal" << std::endl;
    hw.resetMeasurement();
    return false;
  }

  hw.resetMeasurement();
  return true;
}

/**
 * @brief FFT an acquired signal and return the spectrum restricted to the
 *        measurement band, expressed in kHz (mirrors the server pipeline).
 *
 * @param acquired   Taken by value: the Hann window is applied to the copy so
 *                   the caller's buffer is left untouched.
 */
static SpectrumData computeSpectrum(FFTProcessor& fft,
                                    std::vector<float> acquired,
                                    uint32_t numSamples,
                                    double centerHz,
                                    double bandwidthHz)
{
  fft.applyWindow(acquired); // WindowType::Hann, same default as the server
  auto fftResult = fft.computeFFT(acquired);
  auto magnitude = fft.computeMagnitudeSpectrum(fftResult);
  auto phase = fft.computePhaseSpectrum(fftResult);
  auto freqAxis = fft.getFrequencyAxis(numSamples); // Hz

  double lowFreq = centerHz - bandwidthHz / 2.0;
  double highFreq = centerHz + bandwidthHz / 2.0;
  if (lowFreq < 0.0)
  {
    lowFreq = 0.0;
  }

  SpectrumData out;
  for (size_t i = 0; i < freqAxis.size() && i < magnitude.size(); ++i)
  {
    if (freqAxis[i] >= lowFreq && freqAxis[i] <= highFreq)
    {
      out.freqKHz.push_back(static_cast<float>(freqAxis[i] / 1000.0));
      out.magnitude.push_back(magnitude[i]);
      out.phaseRad.push_back(phase[i]);
    }
  }
  return out;
}

/** @brief Flatness/spread metrics over a spectrum's band. */
static Metrics computeMetrics(const std::vector<float>& mag, const std::vector<float>& phase)
{
  Metrics m{};
  const size_t n = mag.size();
  if (n == 0)
  {
    return m;
  }

  // Magnitude mean, min, max.
  double sum = 0.0;
  float maxMag = mag[0];
  float minMag = mag[0];
  for (float v : mag)
  {
    sum += v;
    maxMag = std::max(maxMag, v);
    minMag = std::min(minMag, v);
  }
  m.magMean = static_cast<float>(sum / n);

  // Magnitude standard deviation.
  double var = 0.0;
  for (float v : mag)
  {
    double d = static_cast<double>(v) - m.magMean;
    var += d * d;
  }
  m.magStd = static_cast<float>(std::sqrt(var / n));

  // Flatness as peak-to-trough ratio in dB.
  m.magFlatnessDb =
      (minMag > 0.0f) ? 20.0f * std::log10(maxMag / minMag) : std::numeric_limits<float>::infinity();

  // Phase spread.  Deviations are wrapped into (-pi, pi] so a bin sitting on
  // the +/-pi wrap boundary does not masquerade as a huge error -- this keeps
  // the raw-vs-calibrated comparison fair (the calibration subtracts phase
  // without re-wrapping, so a near-zero residual can land near +/-2pi).
  double psum = 0.0;
  for (float v : phase)
  {
    psum += v;
  }
  const float pmean = static_cast<float>(psum / n);

  double pvar = 0.0;
  float pmax = 0.0f;
  for (float v : phase)
  {
    float d = wrapToPi(v - pmean);
    pvar += static_cast<double>(d) * d;
    pmax = std::max(pmax, std::fabs(d));
  }
  m.phaseStd = static_cast<float>(std::sqrt(pvar / n));
  m.phaseMaxDev = pmax;

  return m;
}

static void saveSpectrumCSV(const std::string& filename, const SpectrumData& s)
{
  std::ofstream file(filename);
  if (!file.is_open())
  {
    std::cerr << "  ! Could not open " << filename << std::endl;
    return;
  }
  file << "Frequency_kHz,Magnitude,Phase_rad\n";
  for (size_t i = 0; i < s.size(); ++i)
  {
    file << s.freqKHz[i] << "," << s.magnitude[i] << "," << s.phaseRad[i] << "\n";
  }
  std::cout << "  Saved: " << filename << " (" << s.size() << " points)" << std::endl;
}

static void printComparison(const Metrics& raw, const Metrics& cal)
{
  std::cout << "\n--- Results Comparison ---" << std::endl;
  std::cout << std::left << std::setw(26) << "" << std::right << std::setw(14) << "Uncalibrated"
            << std::setw(16) << "Calibrated" << std::endl;

  std::cout << std::setprecision(6) << std::defaultfloat;
  auto row = [](const std::string& label, float u, float c)
  {
    std::cout << std::left << std::setw(26) << label << std::right << std::setw(14) << u
              << std::setw(16) << c << std::endl;
  };

  row("Magnitude mean:", raw.magMean, cal.magMean);
  row("Magnitude std:", raw.magStd, cal.magStd);
  row("Magnitude flatness (dB):", raw.magFlatnessDb, cal.magFlatnessDb);
  row("Phase std (rad):", raw.phaseStd, cal.phaseStd);
  row("Phase max deviation:", raw.phaseMaxDev, cal.phaseMaxDev);
}

int main()
{
  try
  {
    std::cout << "=== Calibration Loopback Comparison Test ===\n" << std::endl;

    const double samplingFreq = RP_MAX_FREQ / DECIMATION; // 1.953125 MHz
    const double centerHz = static_cast<double>(CENTER_FREQ);
    const double bandwidthHz = static_cast<double>(BANDWIDTH);

    std::cout << "Configuration:" << std::endl;
    std::cout << "  Decimation:        " << DECIMATION << std::endl;
    std::cout << "  Sampling freq:     " << samplingFreq / 1e6 << " MHz" << std::endl;
    std::cout << "  Samples:           " << NUM_SAMPLES << std::endl;
    std::cout << "  Freq resolution:   " << samplingFreq / NUM_SAMPLES << " Hz" << std::endl;
    std::cout << "  Center frequency:  " << CENTER_FREQ / 1000.0 << " kHz" << std::endl;
    std::cout << "  Bandwidth:         " << BANDWIDTH / 1000.0 << " kHz" << std::endl;
    std::cout << "  Amplitude:         " << AMPLITUDE << std::endl;

    std::cout << "\nHARDWARE SETUP REQUIRED:" << std::endl;
    std::cout << "  1. Connect OUT1 (DAC) -> electronic board -> IN1 (ADC)" << std::endl;
    std::cout << "  2. Ensure the FPGA is programmed with the correct bitfile" << std::endl;
    std::cout << "\nPress Enter to continue..." << std::endl;
    std::cin.get();

    // --- Step 1: hardware init ------------------------------------------------
    RedPitayaHardware hardware;
    if (!hardware.initialize())
    {
      std::cerr << "Hardware initialization failed!" << std::endl;
      return 1;
    }
    if (!hardware.setDecimation(DECIMATION))
    {
      std::cerr << "Failed to set decimation!" << std::endl;
      hardware.cleanup();
      return 1;
    }
    std::cout << "\n[Step 1] Hardware initialized (decimation=" << hardware.getDecimation()
              << ", version=0x" << std::hex << hardware.getVersion() << std::dec << ")" << std::endl;

    SignalGenerator generator(samplingFreq, DECIMATION);
    FFTProcessor fft(samplingFreq);
    CalibrationManager calibration;

    auto sinc = generator.generateSincSignal(NUM_SAMPLES, CENTER_FREQ, BANDWIDTH, AMPLITUDE);

    // --- Step A: calibration run ---------------------------------------------
    std::vector<float> acquiredCal;
    if (!doMeasurement(hardware, sinc, NUM_SAMPLES, acquiredCal))
    {
      hardware.cleanup();
      return 1;
    }
    SpectrumData specRef = computeSpectrum(fft, acquiredCal, NUM_SAMPLES, centerHz, bandwidthHz);
    if (specRef.empty())
    {
      std::cerr << "No frequency bins in the specified band -- check parameters." << std::endl;
      hardware.cleanup();
      return 1;
    }

    CalibrationParams params;
    params.decimation = DECIMATION;
    params.numSamples = NUM_SAMPLES;
    params.centerKHz = CENTER_FREQ / 1000.0f;
    params.bandwidthKHz = BANDWIDTH / 1000.0f;
    params.amplitude = AMPLITUDE;

    calibration.setCalibration(specRef.freqKHz, specRef.magnitude, specRef.phaseRad, params);
    saveSpectrumCSV("cal_test_reference.csv", specRef);
    std::cout << "[Step 2] Calibration run complete (" << specRef.size() << " points stored)"
              << std::endl;

    // Sanity check: the params-matching gate the server relies on must accept
    // an identical parameter set.
    if (!calibration.isValidFor(params))
    {
      std::cerr << "  ! WARNING: isValidFor() rejected identical params" << std::endl;
    }

    // --- Step B: uncalibrated measurement ------------------------------------
    std::vector<float> acquiredRaw;
    if (!doMeasurement(hardware, sinc, NUM_SAMPLES, acquiredRaw))
    {
      hardware.cleanup();
      return 1;
    }
    SpectrumData specRaw = computeSpectrum(fft, acquiredRaw, NUM_SAMPLES, centerHz, bandwidthHz);
    saveSpectrumCSV("cal_test_uncalibrated.csv", specRaw);
    Metrics metricsRaw = computeMetrics(specRaw.magnitude, specRaw.phaseRad);
    std::cout << "[Step 3] Uncalibrated measurement complete" << std::endl;

    // --- Step C: apply calibration -------------------------------------------
    SpectrumData specCal = specRaw; // copy freq/mag/phase, then correct in place
    bool applied = calibration.applyCalibration(specCal.magnitude, specCal.phaseRad, specCal.freqKHz);
    if (!applied)
    {
      std::cerr << "  ! WARNING: calibration was not applied (0 bins corrected)" << std::endl;
    }
    saveSpectrumCSV("cal_test_calibrated.csv", specCal);
    Metrics metricsCal = computeMetrics(specCal.magnitude, specCal.phaseRad);
    std::cout << "[Step 4] Calibration applied" << std::endl;

    // --- Step D: comparison ---------------------------------------------------
    printComparison(metricsRaw, metricsCal);

    bool magImproved = metricsCal.magStd < metricsRaw.magStd;
    bool phaseImproved = metricsCal.phaseStd < metricsRaw.phaseStd;
    bool pass = magImproved && phaseImproved;

    std::cout << "\nVERDICT: " << (pass ? "PASS" : "FAIL") << std::endl;
    std::cout << "  Magnitude std " << (magImproved ? "improved" : "did NOT improve") << " ("
              << std::setprecision(4) << std::defaultfloat << metricsRaw.magStd << " -> "
              << metricsCal.magStd << ")" << std::endl;
    std::cout << "  Phase std     " << (phaseImproved ? "improved" : "did NOT improve") << " ("
              << metricsRaw.phaseStd << " -> " << metricsCal.phaseStd << ")" << std::endl;

    std::cout << "\nGenerated files:" << std::endl;
    std::cout << "  - cal_test_reference.csv     (stored calibration H_ref)" << std::endl;
    std::cout << "  - cal_test_uncalibrated.csv  (raw loopback spectrum)" << std::endl;
    std::cout << "  - cal_test_calibrated.csv    (raw spectrum after calibration)" << std::endl;

    hardware.cleanup();
    return pass ? 0 : 1;
  }
  catch (const std::exception& e)
  {
    std::cerr << "\n   ERROR: " << e.what() << std::endl;
    return 1;
  }
}
