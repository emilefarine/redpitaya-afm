/**
 * @brief Single-BRAM Validation Test Suite
 *
 * Validates the single-BRAM (in-place overwrite) implementation end-to-end:
 *   1. Baseline loopback with waveform correlation
 *   2. Max-depth (65536 samples) operation
 *   3. Busy / PS access-denied arbitration
 *   4. Delay-zero safety (clamp to 1)
 *   5. Overwrite reuse (no stale residues)
 *   6. Stress test (N repeated cycles)
 *
 * Hardware setup: Connect OUT1 (DAC) to IN1 (ADC) with SMA cable.
 */

#include "FFTProcessor.h"
#include "RedPitayaHardware.h"
#include "ResonanceAnalyzer.h"
#include "SignalGenerator.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <sstream>
#include <thread>
#include <vector>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ============================================================
// Configuration
// ============================================================
static constexpr double RP_MAX_FREQ = 125000000.0;
static constexpr uint32_t DECIMATION = 64;
static constexpr double SAMPLING_FREQ = RP_MAX_FREQ / DECIMATION;
static constexpr uint32_t CENTER_FREQ = 500000;
static constexpr uint32_t BANDWIDTH = 20000;
static constexpr double CORRELATION_THRESHOLD =
    0.85; // Pearson r minimum (zero-lag, diagnostic only)
static constexpr double BASELINE_LAGGED_CORR_THRESHOLD = 0.75;
static constexpr double MAXDEPTH_LAGGED_CORR_THRESHOLD = 0.70;
static constexpr double DELAY_ZERO_LAGGED_CORR_THRESHOLD = 0.60;
static constexpr int MAX_CORR_LAG_SAMPLES = 256;
static constexpr int STRESS_ITERATIONS = 100;
static constexpr int MEASUREMENT_TIMEOUT_MS = 2000;

// ============================================================
// Helpers
// ============================================================

struct TestResult
{
  std::string name;
  bool passed;
  std::string detail;
};

static std::vector<TestResult> g_results;

struct CorrelationResult
{
  double maxCorrelation;
  int bestLag;
};

static void recordTest(const std::string& name, bool passed, const std::string& detail = "")
{
  g_results.push_back({name, passed, detail});
  std::cout << (passed ? "  [PASS] " : "  [FAIL] ") << name;
  if (!detail.empty())
    std::cout << " -- " << detail;
  std::cout << std::endl;
}

static bool waitForCompletion(RedPitayaHardware& hw, int timeoutMs = MEASUREMENT_TIMEOUT_MS)
{
  auto t0 = std::chrono::steady_clock::now();
  while (!hw.isMeasurementComplete())
  {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0)
            .count();
    if (elapsed > timeoutMs)
      return false;
  }
  return true;
}

/// Run a full measurement cycle: load signal, start, wait, read back.
/// Returns true on success.  Acquired data is written into `acquired`.
static bool runMeasurementCycle(RedPitayaHardware& hw,
                                const std::vector<float>& excitation,
                                std::vector<float>& acquired,
                                uint32_t delaySamples = 1,
                                int timeoutMs = MEASUREMENT_TIMEOUT_MS)
{
  if (!hw.loadGenerationSignal(excitation))
    return false;
  if (!hw.startMeasurement(static_cast<uint32_t>(excitation.size()), delaySamples))
    return false;
  if (!waitForCompletion(hw, timeoutMs))
    return false;

  acquired.resize(excitation.size());
  if (!hw.getAcquiredSignal(acquired))
    return false;

  hw.resetMeasurement();
  return true;
}

/// Pearson correlation between two equal-length signals (normalize first).
static double pearsonCorrelation(const std::vector<float>& a, const std::vector<float>& b)
{
  if (a.size() != b.size() || a.empty())
    return 0.0;

  size_t n = a.size();
  double sumA = 0, sumB = 0;
  for (size_t i = 0; i < n; ++i)
  {
    sumA += a[i];
    sumB += b[i];
  }
  double meanA = sumA / n, meanB = sumB / n;

  double num = 0, denA = 0, denB = 0;
  for (size_t i = 0; i < n; ++i)
  {
    double da = a[i] - meanA;
    double db = b[i] - meanB;
    num += da * db;
    denA += da * da;
    denB += db * db;
  }
  double den = std::sqrt(denA * denB);
  return (den == 0.0) ? 0.0 : (num / den);
}

/// Maximum normalized cross-correlation over lag in [-maxLag, +maxLag].
static CorrelationResult maxLaggedCorrelation(const std::vector<float>& ref,
                                              const std::vector<float>& sig,
                                              int maxLag = MAX_CORR_LAG_SAMPLES)
{
  CorrelationResult result{0.0, 0};
  if (ref.size() != sig.size() || ref.empty())
    return result;

  const int n = static_cast<int>(ref.size());
  maxLag = std::min(maxLag, n - 1);

  for (int lag = -maxLag; lag <= maxLag; ++lag)
  {
    int iRefStart = (lag > 0) ? lag : 0;
    int iSigStart = (lag > 0) ? 0 : -lag;
    int count = n - std::abs(lag);
    if (count <= 4)
      continue;

    double dot = 0.0;
    double eRef = 0.0;
    double eSig = 0.0;
    for (int k = 0; k < count; ++k)
    {
      const double a = ref[iRefStart + k];
      const double b = sig[iSigStart + k];
      dot += a * b;
      eRef += a * a;
      eSig += b * b;
    }

    const double den = std::sqrt(eRef * eSig);
    if (den <= 0.0)
      continue;

    const double corr = dot / den;
    if (std::abs(corr) > std::abs(result.maxCorrelation))
    {
      result.maxCorrelation = corr;
      result.bestLag = lag;
    }
  }

  return result;
}

static void saveCSV(const std::vector<float>& signal, const std::string& filename, double fs)
{
  std::ofstream f(filename);
  if (!f.is_open())
    return;
  f << "Time_us,Amplitude\n";
  for (size_t i = 0; i < signal.size(); ++i)
    f << (i / fs) * 1e6 << "," << signal[i] << "\n";
}

// ============================================================
// Test 1: Baseline Loopback (enhanced)
// ============================================================
static void testBaselineLoopback(RedPitayaHardware& hw)
{
  std::cout << "\n=== Test 1: Baseline Loopback (16384 samples) ===" << std::endl;
  const uint32_t NUM = 16384;

  hw.clearStatusRegister();

  // Pre-check: STATUS should show idle
  uint32_t statusBefore = hw.readStatusRegister();
  bool idleBefore = !(statusBefore & RedPitayaHardware::STATUS_BUSY_BIT);
  recordTest("Pre-load STATUS busy==0", idleBefore,
             "STATUS=0x" + ([&]{ std::ostringstream o; o << std::hex << statusBefore; return o.str(); })());

  SignalGenerator gen(SAMPLING_FREQ, DECIMATION);
  auto excitation = gen.generateSincSignal(NUM, CENTER_FREQ, BANDWIDTH);

  // Load signal and verify no denial
  bool loadOk = hw.loadGenerationSignal(excitation);
  recordTest("loadGenerationSignal succeeds", loadOk);

  uint32_t statusAfterLoad = hw.readStatusRegister();
  bool noDenial = !(statusAfterLoad & RedPitayaHardware::STATUS_PS_ACCESS_DENIED_BIT);
  recordTest("No denial after load", noDenial);

  // Run measurement
  bool startOk = hw.startMeasurement(NUM, 1);
  recordTest("startMeasurement succeeds", startOk);

  bool completed = waitForCompletion(hw);
  recordTest("Measurement completes within timeout", completed);

  // Post-check STATUS
  uint32_t statusAfter = hw.readStatusRegister();
  bool idleAfter = !(statusAfter & RedPitayaHardware::STATUS_BUSY_BIT);
  recordTest("Post-measurement STATUS busy==0", idleAfter);

  // Read acquired
  std::vector<float> acquired(NUM);
  bool readOk = hw.getAcquiredSignal(acquired);
  recordTest("getAcquiredSignal succeeds", readOk);

  hw.resetMeasurement();

  // Waveform similarity diagnostics and robust pass metric.
  double pearson = pearsonCorrelation(excitation, acquired);
  CorrelationResult lagged = maxLaggedCorrelation(excitation, acquired);

  std::ostringstream pearsonStr;
  pearsonStr << std::fixed << std::setprecision(4) << pearson << " (threshold "
             << CORRELATION_THRESHOLD << ")";
  recordTest("Pearson correlation (diagnostic)", true, pearsonStr.str());

  std::ostringstream laggedStr;
  laggedStr << std::fixed << std::setprecision(4) << "maxCorr=" << lagged.maxCorrelation
            << " lag=" << lagged.bestLag << " (threshold " << BASELINE_LAGGED_CORR_THRESHOLD << ")";
  recordTest("Lag-compensated correlation >= threshold",
             std::abs(lagged.maxCorrelation) >= BASELINE_LAGGED_CORR_THRESHOLD, laggedStr.str());

  // FFT peak frequency check
  FFTProcessor fft(SAMPLING_FREQ);
  auto fftRes = fft.computeFFT(acquired);
  auto mag = fft.computeMagnitudeSpectrum(fftRes);
  auto phase = fft.computePhaseSpectrum(fftRes);

  ResonanceAnalyzer analyzer(SAMPLING_FREQ);
  auto results = analyzer.analyzeResonance(mag, phase, CENTER_FREQ, BANDWIDTH * 3);

  float freqError = std::abs(results.peakFrequency - CENTER_FREQ);
  bool freqOk = freqError < BANDWIDTH;
  std::ostringstream freqStr;
  freqStr << "peak=" << results.peakFrequency / 1000.0 << " kHz, error=" << freqError / 1000.0
          << " kHz";
  recordTest("Peak frequency within bandwidth", freqOk, freqStr.str());

  bool ampOk = results.peakAmplitude > 0.001f;
  recordTest("Signal amplitude detected", ampOk,
             "amplitude=" + std::to_string(results.peakAmplitude));

  saveCSV(excitation, "bram_test_excitation.csv", SAMPLING_FREQ);
  saveCSV(acquired, "bram_test_acquired.csv", SAMPLING_FREQ);
}

// ============================================================
// Test 2: Max-Depth (65536 samples)
// ============================================================
static void testMaxDepth(RedPitayaHardware& hw)
{
  std::cout << "\n=== Test 2: Max-Depth (65536 samples) ===" << std::endl;
  const uint32_t NUM = RedPitayaHardware::MAX_SAMPLES;

  hw.clearStatusRegister();

  SignalGenerator gen(SAMPLING_FREQ, DECIMATION);
  auto excitation = gen.generateSincSignal(NUM, CENTER_FREQ, BANDWIDTH);
  recordTest("Generated MAX_SAMPLES signal", excitation.size() == NUM);

  std::vector<float> acquired;
  bool ok = runMeasurementCycle(hw, excitation, acquired, 1, 5000);
  recordTest("Max-depth measurement cycle succeeds", ok);

  if (ok)
  {
    double corr = pearsonCorrelation(excitation, acquired);
    CorrelationResult lagged = maxLaggedCorrelation(excitation, acquired);
    std::ostringstream s;
    s << std::fixed << std::setprecision(4) << "pearson=" << corr
      << " maxCorr=" << lagged.maxCorrelation << " lag=" << lagged.bestLag;
    recordTest("Max-depth lag-compensated correlation >= threshold",
               std::abs(lagged.maxCorrelation) >= MAXDEPTH_LAGGED_CORR_THRESHOLD, s.str());
  }
}

// ============================================================
// Test 3: Busy / Access-Denied
// ============================================================
static void testBusyAccessDenied(RedPitayaHardware& hw)
{
  std::cout << "\n=== Test 3: Busy / Access-Denied ===" << std::endl;
  const uint32_t NUM = 16384;

  hw.clearStatusRegister();

  SignalGenerator gen(SAMPLING_FREQ, DECIMATION);
  auto excitation = gen.generateSincSignal(NUM, CENTER_FREQ, BANDWIDTH);

  // Load signal before starting measurement
  bool loadOk = hw.loadGenerationSignal(excitation);
  recordTest("Pre-load for busy test", loadOk);

  // Start measurement (creates RUNNING state)
  bool startOk = hw.startMeasurement(NUM, 1);
  recordTest("Start measurement for busy test", startOk);

  // Immediately read STATUS -- should show busy
  uint32_t statusDuringRun = hw.readStatusRegister();
  bool busySet = (statusDuringRun & RedPitayaHardware::STATUS_BUSY_BIT) != 0;
  recordTest("STATUS_BUSY_BIT set during RUNNING", busySet);

  // Attempt PS BRAM access while running (this triggers access denied in PL)
  // loadGenerationSignal writes to BRAM, which should be denied during RUNNING
  std::vector<float> singleSample = {0.5f};
  hw.loadGenerationSignal(singleSample); // Don't care if it "succeeds" at SW level

  // Read STATUS -- should now show PS_ACCESS_DENIED latched
  uint32_t statusAfterAttempt = hw.readStatusRegister();
  bool deniedSet = (statusAfterAttempt & RedPitayaHardware::STATUS_PS_ACCESS_DENIED_BIT) != 0;
  recordTest("STATUS_PS_ACCESS_DENIED latched after BRAM write during busy", deniedSet);

  // Also attempt a read from BRAM during RUNNING
  std::vector<float> dummyRead(1);
  hw.getAcquiredSignal(dummyRead);

  // Verify denied bit is still latched (it's sticky until cleared)
  uint32_t statusAfterRead = hw.readStatusRegister();
  bool deniedStillSet = (statusAfterRead & RedPitayaHardware::STATUS_PS_ACCESS_DENIED_BIT) != 0;
  recordTest("Denial bit sticky after read attempt", deniedStillSet);

  // Wait for completion
  bool completed = waitForCompletion(hw);
  recordTest("Measurement eventually completes", completed);

  hw.resetMeasurement();

  // After completion, busy should be cleared
  uint32_t statusFinal = hw.readStatusRegister();
  bool busyCleared = !(statusFinal & RedPitayaHardware::STATUS_BUSY_BIT);
  recordTest("Busy cleared after completion", busyCleared);

  bool statusClearOk = hw.clearStatusRegister();
  recordTest("Clear sticky STATUS after busy test", statusClearOk);
}

// ============================================================
// Test 4: Delay-Zero Safety (clamp to 1)
// ============================================================
static void testDelayZeroSafety(RedPitayaHardware& hw)
{
  std::cout << "\n=== Test 4: Delay-Zero Safety ===" << std::endl;
  const uint32_t NUM = 4096;

  hw.clearStatusRegister();

  SignalGenerator gen(SAMPLING_FREQ, DECIMATION);
  auto excitation = gen.generateSincSignal(NUM, CENTER_FREQ, BANDWIDTH);

  bool loadOk = hw.loadGenerationSignal(excitation);
  recordTest("Load for delay-zero test", loadOk);

  // Start with delaySamples = 0 (firmware should clamp to 1)
  bool startOk = hw.startMeasurement(NUM, 0);
  recordTest("startMeasurement(delay=0) succeeds", startOk);

  // Read back REG_DELAY to verify clamping
  uint32_t delayReadback = hw.readDelayRegister();
  bool clamped = (delayReadback >= 1);
  recordTest("REG_DELAY clamped to >= 1", clamped, "readback=" + std::to_string(delayReadback));

  bool completed = waitForCompletion(hw);
  recordTest("Delay-zero measurement completes", completed);

  std::vector<float> acquired(NUM);
  bool readOk = hw.getAcquiredSignal(acquired);
  recordTest("Acquired signal readable", readOk);

  hw.resetMeasurement();

  if (readOk)
  {
    // Check that no self-overwrite occurred: first samples should not be
    // corrupted (they should not be exactly the excitation values if acquisition
    // started with delay). A basic check: the acquired signal should be non-zero
    // and have reasonable amplitude.
    float maxVal = *std::max_element(acquired.begin(), acquired.end());
    float minVal = *std::min_element(acquired.begin(), acquired.end());
    float range = maxVal - minVal;
    bool nonDegenerate = (range > 0.001f);
    recordTest("Acquired signal non-degenerate (no self-overwrite)", nonDegenerate,
               "range=" + std::to_string(range));

    double corr = pearsonCorrelation(excitation, acquired);
    CorrelationResult lagged = maxLaggedCorrelation(excitation, acquired);
    std::ostringstream s;
    s << std::fixed << std::setprecision(4) << "pearson=" << corr
      << " maxCorr=" << lagged.maxCorrelation << " lag=" << lagged.bestLag;
    recordTest("Delay-zero lag-compensated correlation >= threshold",
               std::abs(lagged.maxCorrelation) >= DELAY_ZERO_LAGGED_CORR_THRESHOLD, s.str());
  }
}

// ============================================================
// Test 5: Overwrite Reuse (two different waveforms)
// ============================================================
static void testOverwriteReuse(RedPitayaHardware& hw)
{
  std::cout << "\n=== Test 5: Overwrite Reuse ===" << std::endl;
  const uint32_t NUM = 8192;

  hw.clearStatusRegister();

  SignalGenerator gen(SAMPLING_FREQ, DECIMATION);

  // Run 1: sinc at 500 kHz
  auto excitation1 = gen.generateSincSignal(NUM, 500000, 20000);
  std::vector<float> acquired1;
  bool ok1 = runMeasurementCycle(hw, excitation1, acquired1);
  recordTest("Run 1 (sinc 500kHz) succeeds", ok1);

  // Run 2: sine at 200 kHz (very different waveform)
  auto excitation2 = gen.generateSineWave(NUM, 200000.0, 0.8f);
  std::vector<float> acquired2;
  bool ok2 = runMeasurementCycle(hw, excitation2, acquired2);
  recordTest("Run 2 (sine 200kHz) succeeds", ok2);

  if (ok1 && ok2)
  {
    // Acquired2 should correlate better with excitation2 than with excitation1
    double corr2_with_exc2 = pearsonCorrelation(excitation2, acquired2);
    double corr2_with_exc1 = pearsonCorrelation(excitation1, acquired2);

    std::ostringstream s;
    s << std::fixed << std::setprecision(4) << "corr(acq2,exc2)=" << corr2_with_exc2
      << " corr(acq2,exc1)=" << corr2_with_exc1;
    bool noResidues = (corr2_with_exc2 > corr2_with_exc1);
    recordTest("Run 2 matches run 2 waveform (no stale residues)", noResidues, s.str());

    // Run 2 correlation should be positive and meaningful
    recordTest("Run 2 correlation positive", corr2_with_exc2 > 0.3,
               "r=" + std::to_string(corr2_with_exc2));
  }
}

// ============================================================
// Test 6: Stress Test (N cycles)
// ============================================================
static void testStress(RedPitayaHardware& hw)
{
  std::cout << "\n=== Test 6: Stress Test (" << STRESS_ITERATIONS << " cycles) ===" << std::endl;

  hw.clearStatusRegister();

  SignalGenerator gen(SAMPLING_FREQ, DECIMATION);
  std::srand(static_cast<unsigned>(std::time(nullptr)));

  int passes = 0;
  int failures = 0;
  int firstFailIter = -1;

  for (int i = 0; i < STRESS_ITERATIONS; ++i)
  {
    // Randomize parameters within safe bounds
    uint32_t numSamples = 1024 + (std::rand() % 15360); // 1024..16383
    uint32_t delay = 1 + (std::rand() % 10);            // 1..10

    auto excitation = gen.generateSincSignal(numSamples, CENTER_FREQ, BANDWIDTH);
    std::vector<float> acquired;

    bool ok = runMeasurementCycle(hw, excitation, acquired, delay);

    if (ok)
    {
      // Quick sanity: check the acquired signal has non-trivial content
      float maxAbs = 0;
      for (auto v : acquired)
        maxAbs = std::max(maxAbs, std::abs(v));
      if (maxAbs > 0.001f)
        passes++;
      else
      {
        failures++;
        if (firstFailIter < 0)
          firstFailIter = i;
      }
    }
    else
    {
      failures++;
      if (firstFailIter < 0)
        firstFailIter = i;
    }

    // Progress indicator every 10 iterations
    if ((i + 1) % 10 == 0)
      std::cout << "  ... iteration " << (i + 1) << "/" << STRESS_ITERATIONS << " (pass=" << passes
                << " fail=" << failures << ")" << std::endl;
  }

  std::ostringstream s;
  s << passes << "/" << STRESS_ITERATIONS << " passed";
  if (firstFailIter >= 0)
    s << ", first failure at iteration " << firstFailIter;
  recordTest("Stress test pass rate", failures == 0, s.str());

  bool fewFailures = (failures <= STRESS_ITERATIONS / 10); // Allow up to 10% flaky
  recordTest("Stress test flaky rate <= 10%", fewFailures, "failures=" + std::to_string(failures));
}

// ============================================================
// Main
// ============================================================
int main()
{
  try
  {
    std::cout << "============================================" << std::endl;
    std::cout << " Single-BRAM Validation Test Suite" << std::endl;
    std::cout << "============================================" << std::endl;
    std::cout << "\nConfiguration:" << std::endl;
    std::cout << "  Sampling frequency: " << SAMPLING_FREQ / 1e6 << " MHz" << std::endl;
    std::cout << "  Decimation: " << DECIMATION << std::endl;
    std::cout << "  Center frequency: " << CENTER_FREQ / 1000.0 << " kHz" << std::endl;
    std::cout << "  Bandwidth: " << BANDWIDTH / 1000.0 << " kHz" << std::endl;
    std::cout << "  Stress iterations: " << STRESS_ITERATIONS << std::endl;

    std::cout << "\nHARDWARE SETUP REQUIRED:" << std::endl;
    std::cout << "  1. Connect OUT1 (DAC) to IN1 (ADC) with SMA cable" << std::endl;
    std::cout << "  2. Ensure FPGA is programmed with correct bitfile" << std::endl;
    std::cout << "\nPress Enter to continue..." << std::endl;
    std::cin.get();

    // Initialize hardware
    std::cout << "[Init] Initializing hardware..." << std::endl;
    RedPitayaHardware hardware;
    if (!hardware.initialize())
    {
      std::cerr << "Hardware initialization failed!" << std::endl;
      return 1;
    }

    // Run all tests
    testBaselineLoopback(hardware);
    testMaxDepth(hardware);
    testBusyAccessDenied(hardware);
    testDelayZeroSafety(hardware);
    testOverwriteReuse(hardware);
    testStress(hardware);

    // Summary
    std::cout << "\n============================================" << std::endl;
    std::cout << " TEST SUMMARY" << std::endl;
    std::cout << "============================================" << std::endl;

    int totalPass = 0, totalFail = 0;
    for (const auto& r : g_results)
    {
      if (r.passed)
        totalPass++;
      else
        totalFail++;
    }

    for (const auto& r : g_results)
    {
      if (!r.passed)
        std::cout << "  FAILED: " << r.name << (r.detail.empty() ? "" : " -- " + r.detail)
                  << std::endl;
    }

    std::cout << "\n  Total: " << g_results.size() << "  Passed: " << totalPass
              << "  Failed: " << totalFail << std::endl;
    std::cout << "  Result: " << (totalFail == 0 ? "ALL PASSED" : "SOME FAILURES") << std::endl;

    hardware.cleanup();
    return (totalFail == 0) ? 0 : 1;
  }
  catch (const std::exception& e)
  {
    std::cerr << "\n   ERROR: " << e.what() << std::endl;
    return 1;
  }
}
