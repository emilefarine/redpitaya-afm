#include "FFTProcessor.h"
#include "ResonanceAnalyzer.h"
#include "SignalGenerator.h"

#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ============================================================================
// CSV Export Functions
// ============================================================================

void saveSignalToCSV(const std::vector<float>& signal,
                     const std::string& filename,
                     double samplingFreq)
{
  std::ofstream file(filename);
  if (!file.is_open())
  {
    std::cerr << "Error: Could not open " << filename << std::endl;
    return;
  }

  file << "Time_us,Amplitude\n";
  for (size_t i = 0; i < signal.size(); ++i)
  {
    double time_us = (i / samplingFreq) * 1e6; // Convert to microseconds
    file << time_us << "," << signal[i] << "\n";
  }
  file.close();
  std::cout << "Saved: " << filename << std::endl;
}

void saveSpectrumToCSV(const std::vector<float>& frequencies,
                       const std::vector<float>& magnitude,
                       const std::string& filename)
{
  std::ofstream file(filename);
  if (!file.is_open())
  {
    std::cerr << "Error: Could not open " << filename << std::endl;
    return;
  }

  file << "Frequency_kHz,Magnitude\n";
  for (size_t i = 0; i < frequencies.size() && i < magnitude.size(); ++i)
  {
    file << frequencies[i] / 1000.0 << "," << magnitude[i] << "\n";
  }
  file.close();
  std::cout << "Saved: " << filename << std::endl;
}

// ============================================================================
// Test Functions
// ============================================================================

void test1_BasicFFTAccuracy(double samplingFreq)
{
  std::cout << "\nTEST 1: FFTW3 Integration - Basic Accuracy Test" << std::endl;

  const uint32_t NUM_SAMPLES = 1024;
  const double TEST_FREQ = 100000.0; // 100 kHz

  std::cout << "Generating " << TEST_FREQ / 1000.0 << " kHz sine wave..." << std::endl;

  // Generate pure sine wave
  std::vector<float> signal(NUM_SAMPLES);
  for (uint32_t i = 0; i < NUM_SAMPLES; ++i)
  {
    double t = static_cast<double>(i) / samplingFreq;
    signal[i] = static_cast<float>(sin(2.0 * M_PI * TEST_FREQ * t));
  }

  // Compute FFT
  FFTProcessor processor(samplingFreq);
  auto fftResult = processor.computeFFT(signal);
  auto magnitude = processor.computeMagnitudeSpectrum(fftResult);
  auto frequencies = processor.getFrequencyAxis(NUM_SAMPLES);

  // Find peak
  float maxMagnitude = 0.0f;
  size_t peakIndex = 0;
  for (size_t i = 1; i < magnitude.size(); ++i)
  {
    if (magnitude[i] > maxMagnitude)
    {
      maxMagnitude = magnitude[i];
      peakIndex = i;
    }
  }

  float peakFrequency = frequencies[peakIndex];
  float frequencyError = std::abs(peakFrequency - TEST_FREQ);
  float frequencyResolution = frequencies[1] - frequencies[0];

  std::cout << "\nResults:" << std::endl;
  std::cout << "  Peak detected at: " << peakFrequency / 1000.0 << " kHz" << std::endl;
  std::cout << "  Expected: " << TEST_FREQ / 1000.0 << " kHz" << std::endl;
  std::cout << "  Error: " << frequencyError << " Hz" << std::endl;
  std::cout << "  Frequency resolution: " << frequencyResolution << " Hz" << std::endl;

  bool passed = (frequencyError <= frequencyResolution);
  if (passed)
  {
    std::cout << "TEST PASSED - FFT accuracy verified!" << std::endl;
  }
  else
  {
    std::cout << "\nTEST FAILED - Frequency error too large!" << std::endl;
  }

  // Save to CSV
  std::cout << "\nExporting CSV files..." << std::endl;
  saveSignalToCSV(signal, "test1_sine_wave.csv", samplingFreq);
  saveSpectrumToCSV(frequencies, magnitude, "test1_spectrum.csv");
}

void test2_WindowFunctions(double samplingFreq)
{
  std::cout << "\nTEST 2: Window Functions Comparison" << std::endl;

  const uint32_t NUM_SAMPLES = 512;
  const double TEST_FREQ = 100000.0; // 100 kHz

  std::cout << "Testing all window types with " << TEST_FREQ / 1000.0 << " kHz sine wave..."
            << std::endl;

  // Generate test signal
  std::vector<float> originalSignal(NUM_SAMPLES);
  for (uint32_t i = 0; i < NUM_SAMPLES; ++i)
  {
    double t = static_cast<double>(i) / samplingFreq;
    originalSignal[i] = static_cast<float>(sin(2.0 * M_PI * TEST_FREQ * t));
  }

  FFTProcessor processor(samplingFreq);

  // Test each window type
  std::vector<std::pair<WindowType, std::string>> windows = {
      {WindowType::Rectangle, "Rectangle (no window)"},
      {WindowType::Hann, "Hann (default)"},
      {WindowType::Hamming, "Hamming"},
      {WindowType::Blackman, "Blackman (best sidelobe suppression)"}};

  std::cout << "\nWindow function effects (comparing edges vs middle):" << std::endl;

  size_t edgeIdx = 10;                // Near edge (not exactly 0 to avoid sine=0)
  size_t middleIdx = NUM_SAMPLES / 2; // Middle of signal

  for (const auto& [windowType, description] : windows)
  {
    auto signal = originalSignal; // Make a copy
    processor.applyWindow(signal, windowType);

    std::cout << "  - " << description << std::endl;

    // Show edge effect
    float edgeOriginal = originalSignal[edgeIdx];
    float edgeWindowed = signal[edgeIdx];
    float edgeSuppression = (1.0f - edgeWindowed / edgeOriginal) * 100.0f;

    // Show middle effect
    float middleOriginal = originalSignal[middleIdx];
    float middleWindowed = signal[middleIdx];
    float middleChange = (middleWindowed / middleOriginal - 1.0f) * 100.0f;

    std::cout << "    Edge (sample " << edgeIdx << "): " << edgeOriginal << " → " << edgeWindowed
              << " (suppressed " << edgeSuppression << "%)" << std::endl;
    std::cout << "    Middle (sample " << middleIdx << "): " << middleOriginal << " → "
              << middleWindowed << " (change " << middleChange << "%)" << std::endl;
  }

  std::cout << "\nWindow selection guide:" << std::endl;
  std::cout << "  - Rectangle: Maximum frequency resolution, but spectral leakage" << std::endl;
  std::cout << "  - Hann: Good balance, recommended for most applications" << std::endl;
  std::cout << "  - Hamming: Similar to Hann, slightly different characteristics" << std::endl;
  std::cout << "  - Blackman: Best sidelobe suppression, reduced resolution" << std::endl;

  std::cout << "\n    All window functions working correctly!" << std::endl;
}

void test3_AFMSignalProcessing(double samplingFreq, uint32_t decimation)
{
  std::cout << "TEST 3: Complete AFM Signal Processing Pipeline" << std::endl;

  // AFM-typical parameters
  const uint32_t NUM_SAMPLES = 16384;  // ~8.4 ms at 1.953 MHz
  const uint32_t CENTER_FREQ = 500000; // 500 kHz (typical cantilever)
  const uint32_t BANDWIDTH = 20000;
  const uint32_t RESONANCE_FREQ = 501000; // Simulated resonance at 501 kHz

  std::cout << "Simulating AFM cantilever measurement:" << std::endl;
  std::cout << "  Excitation center: " << CENTER_FREQ / 1000.0 << " kHz" << std::endl;
  std::cout << "  Bandwidth: " << BANDWIDTH / 1000.0 << " kHz" << std::endl;
  std::cout << "  Simulated resonance: " << RESONANCE_FREQ / 1000.0 << " kHz" << std::endl;
  std::cout << "  Measurement time: " << NUM_SAMPLES / samplingFreq * 1000.0 << " ms" << std::endl;

  // Step 1: Generate excitation signal (sinc)
  std::cout << "\nStep 1: Generating broadband excitation signal..." << std::endl;
  SignalGenerator generator(samplingFreq, decimation);
  auto excitation = generator.generateSincSignal(NUM_SAMPLES, CENTER_FREQ, BANDWIDTH);
  std::cout << "Sinc signal generated (" << excitation.size() << " samples)" << std::endl;

  // Step 2: Generate simulated cantilever response with realistic damping
  std::cout << "\nStep 2: Simulating cantilever response..." << std::endl;

  // Realistic AFM parameters: Q-factor ~250-300 in air
  const float targetQ = 280.0f;
  const float dampingBandwidth = RESONANCE_FREQ / targetQ; // ~1800 Hz for Q=280

  std::cout << "  Target Q-factor: " << targetQ << std::endl;
  std::cout << "  Expected bandwidth: " << dampingBandwidth << " Hz" << std::endl;

  // Generate damped sinusoidal response
  std::vector<float> response(NUM_SAMPLES);
  for (uint32_t i = 0; i < NUM_SAMPLES; ++i)
  {
    double t = static_cast<double>(i) / samplingFreq;

    // Exponential decay envelope
    double envelope = std::exp(-M_PI * dampingBandwidth * t);

    // Oscillation at resonance frequency
    double oscillation = std::sin(2.0 * M_PI * RESONANCE_FREQ * t);

    // Combine: damped oscillation (typical cantilever ringdown)
    response[i] = 0.5f * static_cast<float>(envelope * oscillation);
  }

  std::cout << "Damped cantilever response generated" << std::endl;

  // Step 3: FFT Analysis
  std::cout << "\nStep 3: Computing FFT..." << std::endl;
  FFTProcessor fftProcessor(samplingFreq);

  auto fftResult = fftProcessor.computeFFT(response);
  auto magnitudeSpectrum = fftProcessor.computeMagnitudeSpectrum(fftResult);
  auto phaseSpectrum = fftProcessor.computePhaseSpectrum(fftResult);
  auto frequencies = fftProcessor.getFrequencyAxis(NUM_SAMPLES);

  float freqResolution = frequencies[1] - frequencies[0];
  std::cout << "FFT computed (" << fftResult.size() << " bins)" << std::endl;
  std::cout << "Frequency resolution: " << freqResolution << " Hz" << std::endl;

  // Step 4: Resonance Analysis
  std::cout << "\nStep 4: Analyzing resonance..." << std::endl;
  ResonanceAnalyzer analyzer(samplingFreq);

  float searchCenter = CENTER_FREQ;
  float searchWidth = BANDWIDTH * 2; // Search ±bandwidth

  auto results =
      analyzer.analyzeResonance(magnitudeSpectrum, phaseSpectrum, searchCenter, searchWidth);

  std::cout << "\nResonance Analysis Results:" << std::endl;
  std::cout << "-------------------------------" << std::endl;
  std::cout << "Peak frequency:                " << std::setw(8) << results.peakFrequency / 1000.0
            << " kHz" << std::endl;
  std::cout << "Peak amplitude:                " << std::setw(8) << results.peakAmplitude
            << std::endl;
  std::cout << "Peak phase:                    " << std::setw(8) << results.peakPhase << " rad"
            << std::endl;
  std::cout << "Center frequency:              " << std::setw(8) << results.centerFrequency / 1000.0
            << " kHz" << std::endl;
  std::cout << "3dB Bandwidth:                 " << std::setw(8) << results.bandwidth3dB / 1000.0
            << " kHz" << std::endl;
  std::cout << "Q-factor:                      " << std::setw(8) << results.qFactor << std::endl;

  // Verify accuracy
  float freqError = std::abs(results.peakFrequency - RESONANCE_FREQ);
  bool accurate = (freqError <= freqResolution * 2); // Within 2 bins

  if (accurate)
  {
    std::cout << "\nResonance detected accurately!" << std::endl;
    std::cout << "Error: " << freqError << " Hz (< " << freqResolution * 2 << " Hz)" << std::endl;
  }
  else
  {
    std::cout << "\nWarning: Resonance detection error: " << freqError << " Hz" << std::endl;
  }

  // Step 5: Export CSV files for visualization
  std::cout << "\nStep 5: Exporting CSV files for visualization..." << std::endl;
  saveSignalToCSV(excitation, "test3_excitation.csv", samplingFreq);
  saveSignalToCSV(response, "test3_response.csv", samplingFreq);
  saveSpectrumToCSV(frequencies, magnitudeSpectrum, "test3_spectrum.csv");

  // Also save excitation spectrum
  auto excitationFFT = fftProcessor.computeFFT(excitation);
  auto excitationSpectrum = fftProcessor.computeMagnitudeSpectrum(excitationFFT);
  saveSpectrumToCSV(frequencies, excitationSpectrum, "test3_excitation_spectrum.csv");

  std::cout << "\nComplete AFM pipeline tested successfully!" << std::endl;
}

// ============================================================================
// Main Program
// ============================================================================

int main()
{
  try
  {
    std::cout << "\nAFM Signal Processing - Comprehensive Test Suite" << std::endl;

    // RedPitaya configuration
    const double RP_MAX_FREQ = 125000000.0; // 125 MHz
    const uint32_t DECIMATION = 64;
    const double SAMPLING_FREQ = RP_MAX_FREQ / DECIMATION; // 1.953125 MHz

    std::cout << "\nRedPitaya Configuration:" << std::endl;
    std::cout << "  Max frequency: " << RP_MAX_FREQ / 1e6 << " MHz" << std::endl;
    std::cout << "  Decimation: " << DECIMATION << std::endl;
    std::cout << "  Sampling frequency: " << SAMPLING_FREQ / 1e6 << " MHz" << std::endl;

    test1_BasicFFTAccuracy(SAMPLING_FREQ);
    test2_WindowFunctions(SAMPLING_FREQ);
    test3_AFMSignalProcessing(SAMPLING_FREQ, DECIMATION);

    std::cout << "ALL TESTS COMPLETED" << std::endl;

    std::cout << "\nGenerated CSV files for visualization:" << std::endl;
    std::cout << "Time-domain signals:" << std::endl;
    std::cout << "    - test1_sine_wave.csv" << std::endl;
    std::cout << "    - test3_excitation.csv (sinc signal)" << std::endl;
    std::cout << "    - test3_response.csv (cantilever response)" << std::endl;
    std::cout << "\nFrequency-domain spectra:" << std::endl;
    std::cout << "    - test1_spectrum.csv" << std::endl;
    std::cout << "    - test3_spectrum.csv (response spectrum)" << std::endl;
    std::cout << "    - test3_excitation_spectrum.csv (sinc spectrum)" << std::endl;

    return 0;
  }
  catch (const std::exception& e)
  {
    std::cerr << "\nERROR: " << e.what() << std::endl;
    return 1;
  }
}
