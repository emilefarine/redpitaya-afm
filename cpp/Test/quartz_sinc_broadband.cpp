#include "FFTProcessor.h"
#include "RedPitayaHardware.h"
#include "ResonanceAnalyzer.h"
#include "SignalGenerator.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <thread>
#include <vector>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

void print_usage()
{
  std::cout << "Usage: ./quartz_sinc_broadband [center_freq_kHz] [bandwidth_kHz] [num_samples] "
               "[decimation]"
            << std::endl;
  std::cout << "\nThis simulates AFM-style broadband excitation using sinc pulse" << std::endl;
  std::cout << "\nExamples:" << std::endl;
  std::cout << "  ./quartz_sinc_broadband 1000 100           # 1 MHz quartz, ±50 kHz bandwidth, "
               "8192 samples, decimation 64"
            << std::endl;
  std::cout << "  ./quartz_sinc_broadband 33 2 65536         # 32 kHz quartz, ±1 kHz, 65536 "
               "samples (better resolution)"
            << std::endl;
  std::cout << "  ./quartz_sinc_broadband 33 2 16384 256     # 32 kHz quartz, decimation 256 for "
               "best freq resolution"
            << std::endl;
  std::cout << "  ./quartz_sinc_broadband 950 100 16384 64   # 950 kHz center, 16384 samples, "
               "decimation 64"
            << std::endl;
  std::cout << "\nDefault: 1000 kHz center, 100 kHz bandwidth, 8192 samples, decimation 64"
            << std::endl;
  std::cout << "\nNum_samples: 8192, 16384, 32768, or 65536 (max = 65536 due to FPGA BRAM limit)"
            << std::endl;
  std::cout << "             Higher samples = better resolution, but slower" << std::endl;
  std::cout << "\nDecimation: 16, 32, 64, 128, 256, 512, or 1024 (power of 2)" << std::endl;
  std::cout << "            Higher decimation = better frequency resolution, but slower"
            << std::endl;
  std::cout << "            decimation 64   -> 238 Hz/bin with 8192 samples (default)" << std::endl;
  std::cout << "            decimation 256  -> 59.6 Hz/bin with 8192 samples (low freq)"
            << std::endl;
  std::cout << "            decimation 1024 -> 14.9 Hz/bin with 8192 samples (ultra low freq)"
            << std::endl;
}

int main(int argc, char* argv[])
{
  std::cout << "Quartz Broadband Sinc Excitation (AFM Simulation)" << std::endl;

  // Default parameters (1 MHz quartz)
  uint32_t CENTER_FREQ = 1000;
  uint32_t BANDWIDTH = 100;
  uint32_t NUM_SAMPLES = 8192;
  uint16_t DECIMATION = 64; // Default decimation

  if (argc > 1)
  {
    if (std::string(argv[1]) == "-h" || std::string(argv[1]) == "--help")
    {
      print_usage();
      return 0;
    }

    CENTER_FREQ = std::atoi(argv[1]);
    if (argc > 2)
      BANDWIDTH = std::atoi(argv[2]);
    if (argc > 3)
    {
      NUM_SAMPLES = std::atoi(argv[3]);

      if (NUM_SAMPLES < 1024 || NUM_SAMPLES > 65536 || (NUM_SAMPLES & (NUM_SAMPLES - 1)) != 0)
      {
        std::cerr << "Error: NUM_SAMPLES must be a power of 2 between 1024 and 65536" << std::endl;
        std::cerr << "Common values: 8192, 16384, 32768, 65536" << std::endl;
        std::cerr << "Limit: 65536 samples (FPGA shared BRAM size = 2^16)" << std::endl;
        return 1;
      }
    }
    if (argc > 4)
    {
      DECIMATION = std::atoi(argv[4]);

      if (DECIMATION < 16 || DECIMATION > 1024 || (DECIMATION & (DECIMATION - 1)) != 0)
      {
        std::cerr << "Error: DECIMATION must be a power of 2 between 16 and 1024" << std::endl;
        std::cerr << "Valid values: 16, 32, 64, 128, 256, 512, 1024" << std::endl;
        return 1;
      }
    }
  }

  const double RP_MAX_FREQ = 125000000.0;
  const double SAMPLING_FREQ = RP_MAX_FREQ / DECIMATION;
  const float AMPLITUDE = 0.5f;

  // FPGA BRAM limits (from red_pitaya_top.sv)
  const uint32_t MAX_ACQ_SAMPLES = 65536; // 2^16 (shared BRAM) - LIMITING FACTOR!

  // Validate NUM_SAMPLES against FPGA BRAM limits
  if (NUM_SAMPLES > MAX_ACQ_SAMPLES)
  {
    std::cerr << "Error: NUM_SAMPLES (" << NUM_SAMPLES << ") exceeds FPGA shared BRAM limit ("
              << MAX_ACQ_SAMPLES << ")" << std::endl;
    std::cerr << "Maximum samples: " << MAX_ACQ_SAMPLES << " (limited by shared BRAM = 2^16)"
              << std::endl;
    std::cerr << "This gives maximum measurement time: " << MAX_ACQ_SAMPLES / SAMPLING_FREQ * 1000.0
              << " ms" << std::endl;
    return 1;
  }

  const double NYQUIST = SAMPLING_FREQ / 2;
  const uint32_t CENTER_FREQ_HZ = CENTER_FREQ * 1000;
  const uint32_t BANDWIDTH_HZ = BANDWIDTH * 1000;
  const uint32_t LOW_FREQ = CENTER_FREQ_HZ - BANDWIDTH_HZ / 2;
  const uint32_t HIGH_FREQ = CENTER_FREQ_HZ + BANDWIDTH_HZ / 2;
  const double FREQ_RESOLUTION = SAMPLING_FREQ / NUM_SAMPLES;

  if (HIGH_FREQ >= NYQUIST)
  {
    std::cerr << "Error: High frequency (" << HIGH_FREQ / 1000.0 << " kHz) exceeds Nyquist limit ("
              << NYQUIST / 1000.0 << " kHz)" << std::endl;
    std::cerr << "Reduce decimation or lower bandwidth" << std::endl;
    return 1;
  }

  std::cout << "\n=== CONFIGURATION ===" << std::endl;
  std::cout << "Decimation (FPGA):    " << DECIMATION << std::endl;
  std::cout << "Sampling frequency:   " << SAMPLING_FREQ / 1e6 << " MHz" << std::endl;
  std::cout << "Nyquist frequency:    " << NYQUIST / 1e6 << " MHz" << std::endl;
  std::cout << "Number of samples:    " << NUM_SAMPLES << std::endl;
  std::cout << "Center frequency:     " << CENTER_FREQ << " kHz" << std::endl;
  std::cout << "Bandwidth:            " << BANDWIDTH << " kHz (±" << BANDWIDTH / 2 << " kHz)"
            << std::endl;
  std::cout << "Search range:         " << LOW_FREQ / 1000.0 << " - " << HIGH_FREQ / 1000.0
            << " kHz" << std::endl;
  std::cout << "Measurement time:     " << NUM_SAMPLES / SAMPLING_FREQ * 1000.0 << " ms"
            << std::endl;
  std::cout << "FFT resolution:       " << FREQ_RESOLUTION << " Hz/bin" << std::endl;
  std::cout << "FFT bins in range:    " << BANDWIDTH_HZ / FREQ_RESOLUTION << " bins" << std::endl;

  std::cout << "\n=== MEASUREMENT PRINCIPLE ===" << std::endl;
  std::cout << "1. Generate cardinal sinc pulse (broadband excitation)" << std::endl;
  std::cout << "2. Excite quartz with single pulse" << std::endl;
  std::cout << "3. Capture response (one measurement)" << std::endl;
  std::cout << "4. FFT to analyze frequency spectrum" << std::endl;
  std::cout << "5. Find resonance peak and calculate Q-factor" << std::endl;
  std::cout << "\nThis simulates AFM cantilever excitation technique!" << std::endl;

  RedPitayaHardware hardware;

  if (!hardware.initialize())
  {
    std::cerr << "Hardware init failed!" << std::endl;
    return 1;
  }

  // Set decimation factor
  std::cout << "\nSetting decimation factor to " << DECIMATION << "..." << std::endl;
  if (!hardware.setDecimation(DECIMATION))
  {
    std::cerr << "Failed to set decimation!" << std::endl;
    return 1;
  }
  std::cout << "Decimation set successfully. Current value: " << hardware.getDecimation()
            << std::endl;

  SignalGenerator gen(SAMPLING_FREQ, DECIMATION);
  FFTProcessor fft(SAMPLING_FREQ);
  ResonanceAnalyzer analyzer(SAMPLING_FREQ);

  std::cout << "\nHardware setup: OUT1 -> Quartz -> IN1" << std::endl;
  std::cout << "\nPress Enter to start broadband measurement..." << std::endl;
  std::cin.get();

  auto measurementStart = std::chrono::steady_clock::now();

  std::cout << "Creating cardinal sinc with " << BANDWIDTH << " kHz bandwidth..." << std::endl;

  auto sincSignal = gen.generateSincSignal(NUM_SAMPLES, CENTER_FREQ_HZ, BANDWIDTH_HZ);

  // Scale to desired amplitude
  for (auto& sample : sincSignal)
  {
    sample *= AMPLITUDE;
  }

  std::cout << "Sinc pulse generated: " << sincSignal.size() << " samples" << std::endl;

  std::cout << "\n=== MEASURING RESPONSE ===" << std::endl;
  hardware.loadGenerationSignal(sincSignal);
  hardware.startMeasurement(NUM_SAMPLES, 0);

  // Wait for measurement to complete
  auto startTime = std::chrono::steady_clock::now();
  const auto timeout = std::chrono::milliseconds(500);
  while (!hardware.isMeasurementComplete())
  {
    std::this_thread::sleep_for(std::chrono::microseconds(100));
    if (std::chrono::steady_clock::now() - startTime > timeout)
    {
      std::cerr << "ERROR: Measurement timeout!" << std::endl;
      return 1;
    }
  }

  std::vector<float> response(NUM_SAMPLES);
  hardware.getAcquiredSignal(response);

  auto measurementEnd = std::chrono::steady_clock::now();
  auto measurementTime =
      std::chrono::duration_cast<std::chrono::milliseconds>(measurementEnd - measurementStart)
          .count();

  std::cout << "Response captured: " << response.size() << " samples" << std::endl;
  std::cout << "Total measurement time: " << measurementTime << " ms (FAST!)" << std::endl;

  // Save time domain data for precision analysis
  std::string timeFilename = "quartz_sinc_time_domain.csv";
  std::ofstream timeFile(timeFilename);
  if (timeFile.is_open())
  {
    timeFile << "Time_ms,Voltage_V\n";
    for (size_t i = 0; i < response.size(); ++i)
    {
      double timeMs = static_cast<double>(i) / SAMPLING_FREQ * 1000.0;
      timeFile << std::fixed << std::setprecision(6) << timeMs << "," << std::scientific
               << std::setprecision(9) << response[i] << "\n";
    }
    timeFile.close();
    std::cout << "Time domain data saved: " << timeFilename << std::endl;
  }

  float maxResponse = *std::max_element(response.begin(), response.end());
  float minResponse = *std::min_element(response.begin(), response.end());
  float peakToPeak = maxResponse - minResponse;

  // RMS of response
  float rms = 0.0f;
  for (const auto& val : response)
  {
    rms += val * val;
  }
  rms = sqrt(rms / response.size());

  std::cout << "\nRaw response statistics:" << std::endl;
  std::cout << "  Peak-to-peak:  " << std::scientific << peakToPeak << " V" << std::endl;
  std::cout << "  RMS amplitude: " << std::scientific << rms << " V" << std::endl;

  std::cout << "\n=== FFT ANALYSIS ===" << std::endl;
  std::cout << "Applying Hann window..." << std::endl;
  // fft.applyWindow(response, WindowType::Hann); Check if necessary afterward (probably not)

  std::cout << "Computing FFT..." << std::endl;
  auto fftResult = fft.computeFFT(response);
  auto magnitude = fft.computeMagnitudeSpectrum(fftResult);
  auto phase_spectrum = fft.computePhaseSpectrum(fftResult);

  std::cout << "FFT complete: " << magnitude.size() << " frequency bins" << std::endl;

  std::cout << "\n=== RESONANCE ANALYSIS ===" << std::endl;
  std::cout << "Searching for resonance peak in " << LOW_FREQ / 1000.0 << " - "
            << HIGH_FREQ / 1000.0 << " kHz..." << std::endl;

  float searchBandwidth = BANDWIDTH_HZ * 1.2f; // Search slightly wider
  auto resonance =
      analyzer.analyzeResonance(magnitude, phase_spectrum, CENTER_FREQ_HZ, searchBandwidth);

  std::string filename = "quartz_sinc_spectrum_" + std::to_string(CENTER_FREQ) + "kHz.csv";
  std::ofstream csvFile(filename);
  if (csvFile.is_open())
  {
    csvFile << "Frequency_kHz,Magnitude,Phase_rad\n";

    std::cout << "Saving spectrum in frequency range..." << std::endl;
    std::cout << "Magnitude size: " << magnitude.size() << std::endl;
    std::cout << "Frequency resolution: " << FREQ_RESOLUTION << " Hz/bin" << std::endl;

    int pointsWritten = 0;

    // Only save frequencies within the excitation range (where sinc has energy)
    std::cout << "Low frequ: " << LOW_FREQ << " Hz, High freq: " << HIGH_FREQ << " Hz" << std::endl;
    for (size_t i = 0; i < magnitude.size(); ++i)
    {
      double freq = static_cast<double>(i) * FREQ_RESOLUTION;

      // Only save data within LOW_FREQ to HIGH_FREQ range
      if (freq >= LOW_FREQ && freq <= HIGH_FREQ)
      {
        csvFile << std::fixed << std::setprecision(3) << freq / 1000.0 << "," << std::scientific
                << std::setprecision(6) << magnitude[i] << "," << std::fixed << std::setprecision(6)
                << phase_spectrum[i] << "\n";
        pointsWritten++;
      }
    }

    csvFile.close();
    std::cout << "Data points written: " << pointsWritten << std::endl;
    std::cout << "Frequency range: " << std::fixed << std::setprecision(1) << LOW_FREQ / 1000.0
              << " - " << HIGH_FREQ / 1000.0 << " kHz" << std::endl;
    std::cout << "\nSpectrum saved: " << filename << std::endl;
  }

  std::cout << "\n" << std::string(70, '=') << std::endl;
  std::cout << "=== BROADBAND MEASUREMENT RESULTS ===" << std::endl;
  std::cout << std::string(70, '=') << std::endl;

  std::cout << "\n--- MEASUREMENT EFFICIENCY ---" << std::endl;
  std::cout << "  Measurement time:     " << measurementTime << " ms" << std::endl;
  std::cout << "  Frequency points:     " << (HIGH_FREQ - LOW_FREQ) / (FREQ_RESOLUTION) << " bins"
            << std::endl;
  std::cout << "  Equivalent sweep:     ~"
            << (HIGH_FREQ - LOW_FREQ) / 1000 * (NUM_SAMPLES / SAMPLING_FREQ * 1000.0) << " ms"
            << std::endl;
  std::cout << "  Speed advantage:      "
            << ((HIGH_FREQ - LOW_FREQ) / 1000 * (NUM_SAMPLES / SAMPLING_FREQ * 1000.0)) /
                   measurementTime
            << "x FASTER!" << std::endl;

  std::cout << "\n--- RESONANCE PEAK ---" << std::endl;
  std::cout << "  Peak frequency:       " << std::fixed << std::setprecision(3)
            << resonance.peakFrequency / 1000.0 << " kHz" << std::endl;
  std::cout << "  Peak amplitude:       " << std::scientific << resonance.peakAmplitude
            << std::endl;
  std::cout << "  Peak phase:           " << std::fixed << std::setprecision(1)
            << resonance.peakPhase << "°" << std::endl;
  std::cout << "  Frequency offset:     " << std::fixed << std::setprecision(3)
            << (resonance.peakFrequency - CENTER_FREQ_HZ) / 1000.0 << " kHz from center"
            << std::endl;

  std::cout << "\n--- Q-FACTOR ANALYSIS ---" << std::endl;
  std::cout << "  Q-factor:             " << std::fixed << std::setprecision(1) << resonance.qFactor
            << std::endl;
  std::cout << "  3dB Bandwidth:        " << std::fixed << std::setprecision(1)
            << resonance.bandwidth3dB / 1000.0 << " kHz (" << resonance.bandwidth3dB << " Hz)"
            << std::endl;
  std::cout << "  Center frequency:     " << std::fixed << std::setprecision(3)
            << resonance.centerFrequency / 1000.0 << " kHz" << std::endl;

  std::cout << "\n--- RESONANCE QUALITY ---" << std::endl;
  if (resonance.qFactor > 10000)
  {
    std::cout << "EXCELLENT: Q > 10,000 - High-quality quartz!" << std::endl;
  }
  else if (resonance.qFactor > 5000)
  {
    std::cout << "GOOD: Q > 5,000 - Good quartz quality" << std::endl;
  }
  else if (resonance.qFactor > 1000)
  {
    std::cout << "FAIR: Q > 1,000 - Acceptable resonance" << std::endl;
  }
  else if (resonance.qFactor > 100)
  {
    std::cout << "LOW Q: Q > 100 - Weak resonance detected" << std::endl;
    std::cout << " -> This is EXPECTED for broadband sinc excitation" << std::endl;
    std::cout << " -> High-Q systems need sustained excitation (use sweep instead)" << std::endl;
  }
  else if (resonance.qFactor > 10)
  {
    std::cout << "VERY LOW Q: Q < 100 - Very weak resonance" << std::endl;
    std::cout << " -> Sinc pulse may not provide enough energy for high-Q resonance" << std::endl;
    std::cout << " -> Try increasing amplitude or use sine sweep" << std::endl;
  }
  else
  {
    std::cout << "X NO RESONANCE: Q < 10 - No clear resonance detected" << std::endl;
    std::cout << " -> Check frequency range and connections" << std::endl;
    std::cout << " -> High-Q quartz may need sustained excitation (sine sweep)" << std::endl;
  }

  std::cout << "\n--- AFM SIMULATION NOTES ---" << std::endl;
  std::cout << "Single-shot measurement (like AFM)" << std::endl;
  std::cout << "Broadband excitation covers entire range" << std::endl;
  std::cout << "Fast frequency detection (" << measurementTime << " ms)" << std::endl;
  if (resonance.qFactor < 1000)
  {
    std::cout << "\nNote: High-Q quartz (Q > 10,000) may show lower Q with sinc excitation"
              << std::endl;
    std::cout << " -> Sinc is optimized for moderate-Q systems (AFM cantilevers, Q ~ 100-500)"
              << std::endl;
    std::cout << " -> For accurate high-Q measurement, use sine sweep method" << std::endl;
  }
  else
  {
    std::cout << "\nResonance well-characterized with broadband excitation!" << std::endl;
  }

  std::cout << "\n" << std::string(70, '=') << std::endl;
  std::cout << "\nVisualize spectrum with: python plot_quartz_sinc_spectrum.py" << std::endl;

  hardware.cleanup();

  return 0;
}
