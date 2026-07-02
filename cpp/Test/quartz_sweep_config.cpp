#include "RedPitayaHardware.h"
#include "SignalGenerator.h"

#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <thread>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

void print_usage()
{
  std::cout << "Usage: ./quartz_sweep_config [center_freq_kHz] [range_kHz] [step_kHz]" << std::endl;
  std::cout << "\nExamples:" << std::endl;
  std::cout << "  ./quartz_sweep_config 1000 100 1     # 1 MHz quartz, ±50 kHz, 1 kHz steps"
            << std::endl;
  std::cout << "  ./quartz_sweep_config 2000 100 2     # 2 MHz quartz, ±50 kHz, 2 kHz steps"
            << std::endl;
  std::cout << "  ./quartz_sweep_config 4000 200 5     # 4 MHz quartz, ±100 kHz, 5 kHz steps"
            << std::endl;
  std::cout << "\nDefault: 1000 kHz center, ±50 kHz range, 1 kHz steps" << std::endl;
}

int main(int argc, char* argv[])
{
  std::cout << "=== Configurable Quartz Frequency Sweep Test ===" << std::endl;

  // Default parameters (1 MHz quartz)
  uint32_t CENTER_FREQ = 1000; // kHz
  uint32_t RANGE = 100;        // kHz (±50 kHz)
  uint32_t FREQ_STEP = 1;      // kHz

  // Parse command line arguments
  if (argc > 1)
  {
    if (std::string(argv[1]) == "-h" || std::string(argv[1]) == "--help")
    {
      print_usage();
      return 0;
    }

    CENTER_FREQ = std::atoi(argv[1]);
    if (argc > 2)
      RANGE = std::atoi(argv[2]);
    if (argc > 3)
      FREQ_STEP = std::atoi(argv[3]);
  }

  const double RP_MAX_FREQ = 125000000.0;
  const uint32_t DECIMATION = 64;
  const double SAMPLING_FREQ = RP_MAX_FREQ / DECIMATION;
  const uint32_t NUM_SAMPLES = 8192;

  // Calculate sweep range
  const uint32_t START_FREQ = (CENTER_FREQ - RANGE / 2) * 1000;
  const uint32_t STOP_FREQ = (CENTER_FREQ + RANGE / 2) * 1000;
  const uint32_t STEP = FREQ_STEP * 1000;
  const float AMPLITUDE = 0.5f;

  const double NYQUIST = SAMPLING_FREQ / 2;
  if (STOP_FREQ >= NYQUIST)
  {
    std::cerr << "Error: Stop frequency (" << STOP_FREQ / 1000.0 << " kHz) exceeds Nyquist limit ("
              << NYQUIST / 1000.0 << " kHz)" << std::endl;
    std::cerr << "Reduce decimation or lower frequency range" << std::endl;
    return 1;
  }

  std::cout << "\nConfiguration:" << std::endl;
  std::cout << "  Sampling frequency: " << SAMPLING_FREQ / 1e6 << " MHz" << std::endl;
  std::cout << "  Nyquist frequency:  " << NYQUIST / 1e6 << " MHz" << std::endl;
  std::cout << "  Center frequency:   " << CENTER_FREQ << " kHz" << std::endl;
  std::cout << "  Sweep range:        " << START_FREQ / 1000.0 << " - " << STOP_FREQ / 1000.0
            << " kHz" << std::endl;
  std::cout << "  Step size:          " << FREQ_STEP << " kHz" << std::endl;
  std::cout << "  Number of points:   " << (STOP_FREQ - START_FREQ) / STEP + 1 << std::endl;
  std::cout << "  Measurement time:   " << NUM_SAMPLES / SAMPLING_FREQ * 1000.0 << " ms per point"
            << std::endl;

  RedPitayaHardware hardware;

  if (!hardware.initialize())
  {
    std::cerr << "Hardware init failed!" << std::endl;
    return 1;
  }

  SignalGenerator gen(SAMPLING_FREQ, DECIMATION);

  // Log in csv file
  std::string filename = "quartz_sweep_" + std::to_string(CENTER_FREQ) + "kHz.csv";
  std::ofstream csvFile(filename);
  if (!csvFile.is_open())
  {
    std::cerr << "Failed to open output file!" << std::endl;
    return 1;
  }

  csvFile << "Frequency_kHz,RMS_Amplitude,Phase_deg\n";

  std::cout << "\nStarting frequency sweep..." << std::endl;
  std::cout << "Hardware setup: OUT1 -> Quartz -> IN1" << std::endl;
  std::cout << "\nPress Enter to start..." << std::endl;
  std::cin.get();

  float maxAmplitude = 0.0f;
  uint32_t maxFreq = 0;
  float minAmplitude = 1e6;

  auto sweepStart = std::chrono::steady_clock::now();

  for (uint32_t freq = START_FREQ; freq <= STOP_FREQ; freq += STEP)
  {
    // Generate sine wave at this frequency
    auto testSignal = gen.generateSineWave(NUM_SAMPLES, freq, AMPLITUDE);

    // Load and measure
    hardware.loadGenerationSignal(testSignal);
    hardware.startMeasurement(NUM_SAMPLES, 0);

    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    std::vector<float> acquired(NUM_SAMPLES);
    hardware.getAcquiredSignal(acquired);

    // Calculate RMS amplitude and phase using correlation with reference sine/cosine
    float rms = 0.0f;
    float sumSin = 0.0f;
    float sumCos = 0.0f;

    const double omega = 2.0 * M_PI * freq / SAMPLING_FREQ;

    for (size_t i = 0; i < acquired.size(); ++i)
    {
      rms += acquired[i] * acquired[i];
      sumSin += acquired[i] * sin(omega * i);
      sumCos += acquired[i] * cos(omega * i);
    }
    rms = sqrt(rms / acquired.size());

    // Phase calculation: atan2(sin_component, cos_component)
    float amplitude = sqrt(sumSin * sumSin + sumCos * sumCos);
    float phase_rad = atan2(sumSin, sumCos);
    float phase_deg = phase_rad * 180.0 / M_PI;

    // Save to CSV
    csvFile << std::fixed << std::setprecision(3) << freq / 1000.0 << "," << std::scientific
            << std::setprecision(6) << amplitude << "," << std::fixed << std::setprecision(2)
            << phase_deg << "\n";

    // Track maximum and minimum
    if (rms > maxAmplitude)
    {
      maxAmplitude = rms;
      maxFreq = freq;
    }
    if (rms < minAmplitude)
    {
      minAmplitude = rms;
    }

    // Progress display every 10 points
    if ((freq - START_FREQ) % (STEP * 10) == 0)
    {
      float progress = (float)(freq - START_FREQ) / (STOP_FREQ - START_FREQ) * 100.0f;
      std::cout << "  Progress: " << std::fixed << std::setprecision(1) << progress << "% - "
                << freq / 1000.0 << " kHz: RMS = " << std::scientific << std::setprecision(3) << rms
                << std::endl;
    }

    hardware.resetMeasurement();
  }

  auto sweepEnd = std::chrono::steady_clock::now();
  auto sweepDuration =
      std::chrono::duration_cast<std::chrono::seconds>(sweepEnd - sweepStart).count();

  csvFile.close();

  // Calculate contrast ratio
  float contrastRatio = maxAmplitude / minAmplitude;

  std::cout << "\n=== SWEEP RESULTS ===" << std::endl;
  std::cout << "Total sweep time: " << sweepDuration << " seconds" << std::endl;
  std::cout << "\nPeak found at:     " << maxFreq / 1000.0 << " kHz" << std::endl;
  std::cout << "Peak amplitude:    " << std::scientific << maxAmplitude << std::endl;
  std::cout << "Minimum amplitude: " << std::scientific << minAmplitude << std::endl;
  std::cout << "Contrast ratio:    " << std::fixed << std::setprecision(2) << contrastRatio << "×"
            << std::endl;

  std::cout << "\n=== DIAGNOSIS ===" << std::endl;
  if (contrastRatio > 10.0)
  {
    std::cout << " STRONG RESONANCE: Contrast ratio > 10x" << std::endl;
    std::cout << "  -> Clear quartz resonance detected!" << std::endl;
  }
  else if (contrastRatio > 3.0)
  {
    std::cout << "!! WEAK RESONANCE: Contrast ratio 3-10x" << std::endl;
    std::cout << "  -> Possible resonance but weak coupling" << std::endl;
    std::cout << "  -> Try adding load capacitors or checking connections" << std::endl;
  }
  else if (contrastRatio > 1.5)
  {
    std::cout << "!! VERY WEAK: Contrast ratio 1.5-3x" << std::endl;
    std::cout << "  -> Barely visible response" << std::endl;
    std::cout << "  -> Check connections" << std::endl;
  }
  else
  {
    std::cout << "X NO RESONANCE: Contrast ratio < 1.5x" << std::endl;
    std::cout << "  -> Quartz is not resonating" << std::endl;
  }

  std::cout << "\nSaved: " << filename << std::endl;
  std::cout << "\nNext steps:" << std::endl;
  std::cout << "  1. Plot the CSV file with: python plot_quartz_sweep.py" << std::endl;
  std::cout << "  2. Look for sharp peak in amplitude" << std::endl;
  std::cout << "  3. If no peak, try different frequency range" << std::endl;

  hardware.cleanup();

  return 0;
}
