#include "FFTProcessor.h"
#include "RedPitayaHardware.h"
#include "ResonanceAnalyzer.h"
#include "SignalGenerator.h"

#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <thread>

/**
 * @brief Hardware loopback test: DAC -> Cable -> ADC -> Signal Processing
 *
 * Test procedure:
 * 1. Generate sinc excitation signal (software)
 * 2. Load to FPGA generation buffer (BRAM_GEN)
 * 3. Start FPGA: Generate on DAC, acquire on ADC
 * 4. Read acquisition buffer (BRAM_ACQ)
 * 5. Process with FFT and resonance analysis
 * 6. Export results to CSV
 *
 * Hardware setup:
 * - Connect OUT1 (DAC) to IN1 (ADC) with SMA cable
 * - Make sure FPGA is programmed with correct bitfile
 */

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

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
    double time_us = (i / samplingFreq) * 1e6;
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

int main()
{
  try
  {

    std::cout << "RedPitaya Hardware Loopback Test (DAC -> ADC)" << std::endl;

    // RedPitaya configuration
    const double RP_MAX_FREQ = 125000000.0; // 125 MHz
    const uint32_t DECIMATION = 64;
    const double SAMPLING_FREQ = RP_MAX_FREQ / DECIMATION; // 1.953125 MHz

    // Signal parameters (matching old C code)
    const uint32_t NUM_SAMPLES = 16384;  // ~8.4 ms
    const uint32_t CENTER_FREQ = 500000; // 500 kHz
    const uint32_t BANDWIDTH = 20000;    // 20 kHz
    const uint32_t DELAY_SAMPLES = 0;    // No delay between gen and acq

    std::cout << "\nConfiguration:" << std::endl;
    std::cout << "  Max frequency: " << RP_MAX_FREQ / 1e6 << " MHz" << std::endl;
    std::cout << "  Decimation: " << DECIMATION << std::endl;
    std::cout << "  Sampling frequency: " << SAMPLING_FREQ / 1e6 << " MHz" << std::endl;
    std::cout << "  Number of samples: " << NUM_SAMPLES << std::endl;
    std::cout << "  Measurement time: " << NUM_SAMPLES / SAMPLING_FREQ * 1000.0 << " ms"
              << std::endl;
    std::cout << "  Center frequency: " << CENTER_FREQ / 1000.0 << " kHz" << std::endl;
    std::cout << "  Bandwidth: " << BANDWIDTH / 1000.0 << " kHz" << std::endl;

    // Hardware setup warning
    std::cout << "\nHARDWARE SETUP REQUIRED:" << std::endl;
    std::cout << "  1. Connect OUT1 (DAC) to IN1 (ADC) with SMA cable" << std::endl;
    std::cout << "  2. Ensure FPGA is programmed with correct bitfile" << std::endl;
    std::cout << "\nPress Enter to continue..." << std::endl;
    std::cin.get();

    // Step 1: Initialize Hardware
    std::cout << "\n[Step 1] Initializing hardware..." << std::endl;
    RedPitayaHardware hardware;

    if (!hardware.initialize())
    {
      std::cerr << "Hardware initialization failed!" << std::endl;
      return 1;
    }

    // Step 2: Generate Excitation Signal
    std::cout << "\n[Step 2] Generating sinc excitation signal..." << std::endl;
    SignalGenerator generator(SAMPLING_FREQ, DECIMATION);
    auto excitation = generator.generateSincSignal(NUM_SAMPLES, CENTER_FREQ, BANDWIDTH);

    std::cout << "Generated " << excitation.size() << " samples" << std::endl;
    saveSignalToCSV(excitation, "hw_test_excitation.csv", SAMPLING_FREQ);

    // Step 3: Load to FPGA and Start Measurement
    std::cout << "\n[Step 3] Loading signal to FPGA..." << std::endl;
    if (!hardware.loadGenerationSignal(excitation))
    {
      std::cerr << "Failed to load signal!" << std::endl;
      return 1;
    }

    std::cout << "\n[Step 4] Starting measurement..." << std::endl;
    if (!hardware.startMeasurement(NUM_SAMPLES, DELAY_SAMPLES))
    {
      std::cerr << "Failed to start measurement!" << std::endl;
      return 1;
    }

    // Step 4: Wait for Completion
    std::cout << "\n[Step 5] Waiting for acquisition to complete..." << std::endl;

    auto startTime = std::chrono::steady_clock::now();
    while (!hardware.isMeasurementComplete())
    {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));

      // Timeout after 1 second
      auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now() - startTime)
                         .count();

      if (elapsed > 1000)
      {
        std::cerr << "Timeout waiting for measurement!" << std::endl;
        return 1;
      }
    }

    auto endTime = std::chrono::steady_clock::now();
    auto duration =
        std::chrono::duration_cast<std::chrono::microseconds>(endTime - startTime).count();

    std::cout << "Measurement complete in " << duration / 1000.0 << " ms" << std::endl;

    // Step 5: Read Acquired Signal
    std::cout << "\n[Step 6] Reading acquired signal from FPGA..." << std::endl;
    std::vector<float> acquired(NUM_SAMPLES);

    if (!hardware.getAcquiredSignal(acquired))
    {
      std::cerr << "Failed to read acquired signal!" << std::endl;
      return 1;
    }

    saveSignalToCSV(acquired, "hw_test_acquired.csv", SAMPLING_FREQ);

    // Step 6: Signal Processing - FFT
    std::cout << "\n[Step 7] Computing FFT..." << std::endl;
    FFTProcessor fftProcessor(SAMPLING_FREQ);

    auto fftResult = fftProcessor.computeFFT(acquired);
    auto magnitudeSpectrum = fftProcessor.computeMagnitudeSpectrum(fftResult);
    auto phaseSpectrum = fftProcessor.computePhaseSpectrum(fftResult);
    auto frequencies = fftProcessor.getFrequencyAxis(NUM_SAMPLES);

    float freqResolution = frequencies[1] - frequencies[0];
    std::cout << "  FFT computed (" << fftResult.size() << " bins)" << std::endl;
    std::cout << "  Frequency resolution: " << freqResolution << " Hz" << std::endl;

    saveSpectrumToCSV(frequencies, magnitudeSpectrum, "hw_test_spectrum.csv");

    // Step 7: Resonance Analysis
    std::cout << "\n[Step 8] Analyzing resonance..." << std::endl;
    ResonanceAnalyzer analyzer(SAMPLING_FREQ);

    float searchCenter = CENTER_FREQ;
    float searchWidth = BANDWIDTH * 3; // Search wider range

    auto results =
        analyzer.analyzeResonance(magnitudeSpectrum, phaseSpectrum, searchCenter, searchWidth);

    std::cout << "Hardware Test Results" << std::endl;

    std::cout << "\n  Peak frequency:  " << std::setw(10) << results.peakFrequency / 1000.0
              << " kHz" << std::endl;
    std::cout << "  Peak amplitude:  " << std::setw(10) << results.peakAmplitude << std::endl;
    std::cout << "  Peak phase:      " << std::setw(10) << results.peakPhase << " rad" << std::endl;
    std::cout << "  Center frequency:  " << std::setw(10) << results.centerFrequency / 1000.0
              << " kHz" << std::endl;
    std::cout << "  3dB Bandwidth:     " << std::setw(10) << results.bandwidth3dB / 1000.0 << " kHz"
              << std::endl;
    std::cout << "  Q-factor:          " << std::setw(10) << results.qFactor << std::endl;

    // Step 8: Validation
    std::cout << "\n[Validation]" << std::endl;

    // Check if we see the excitation bandwidth
    float expectedRange = BANDWIDTH * 2; // Sinc should cover this range
    bool bandwidthOk = (results.peakFrequency > CENTER_FREQ - expectedRange / 2) &&
                       (results.peakFrequency < CENTER_FREQ + expectedRange / 2);

    if (bandwidthOk)
    {
      std::cout << "Peak within expected range (" << (CENTER_FREQ - expectedRange / 2) / 1000.0
                << " - " << (CENTER_FREQ + expectedRange / 2) / 1000.0 << " kHz)" << std::endl;
    }
    else
    {
      std::cout << "Peak outside expected range!" << std::endl;
    }

    // Check signal amplitude
    // Note: Sinc signal with Hann window typically gives ~0.002 magnitude
    // This is EXPECTED due to energy spreading across bandwidth
    if (results.peakAmplitude > 0.001f)
    {
      std::cout << "  Signal detected (amplitude = " << results.peakAmplitude << ")" << std::endl;
      std::cout << "  (Low amplitude is expected for sinc signal - energy spread across bandwidth)"
                << std::endl;
    }
    else
    {
      std::cout << "!! Very low signal amplitude - check cable connection!" << std::endl;
    }

    // Cleanup
    hardware.cleanup();

    std::cout << "Hardware Test Complete!" << std::endl;

    std::cout << "\nGenerated files:" << std::endl;
    std::cout << "  - hw_test_excitation.csv (sinc signal sent to DAC)" << std::endl;
    std::cout << "  - hw_test_acquired.csv (signal received from ADC)" << std::endl;
    std::cout << "  - hw_test_spectrum.csv (FFT of acquired signal)" << std::endl;

    std::cout << "\nNext steps:" << std::endl;
    std::cout << "  1. Visualize CSV files with Python" << std::endl;
    std::cout << "  2. Compare excitation vs acquired signal" << std::endl;
    std::cout << "  3. Verify spectrum shows broadband content" << std::endl;

    return 0;
  }
  catch (const std::exception& e)
  {
    std::cerr << "\n   ERROR: " << e.what() << std::endl;
    return 1;
  }
}
