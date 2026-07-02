#pragma once

#include <complex>
#include <cstdint>
#include <fftw3.h>
#include <vector>

enum class WindowType
{
  Rectangle, // No windowing (rectangular window)
  Hann,      // Hann window (raised cosine)
  Hamming,   // Hamming window (less side lobe than Hann)
  Blackman   // Blackman window (best side lobe suppression)
};

class FFTProcessor
{
public:
  explicit FFTProcessor(double samplingFrequency);
  ~FFTProcessor();

  std::vector<std::complex<float>> computeFFT(const std::vector<float>& inputSignal);

  std::vector<float> computeMagnitudeSpectrum(
      const std::vector<std::complex<float>>& fftResult) const;

  std::vector<float> computePhaseSpectrum(const std::vector<std::complex<float>>& fftResult) const;

  std::vector<float> getFrequencyAxis(uint32_t numSamples) const;

  void applyWindow(std::vector<float>& signal, WindowType windowType = WindowType::Hann) const;

  double getSamplingFrequency() const
  {
    return m_samplingFrequency;
  }

private:
  double m_samplingFrequency;

  mutable fftwf_plan m_forwardPlan;
  mutable float* m_inputBuffer;
  mutable fftwf_complex* m_outputBuffer;
  mutable size_t m_planSize;

  void _initializePlan(size_t size) const;
  void _cleanupPlan() const;
};
