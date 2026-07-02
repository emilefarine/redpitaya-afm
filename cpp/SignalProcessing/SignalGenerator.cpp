#include "SignalGenerator.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

// Mathematical constants
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

SignalGenerator::SignalGenerator(double samplingFrequency, uint16_t decimationFactor)
    : m_samplingFrequency(samplingFrequency)
    , m_decimationFactor(decimationFactor)
{
  if (samplingFrequency <= 0)
  {
    throw std::invalid_argument("Sampling frequency must be positive");
  }
  if (decimationFactor == 0)
  {
    throw std::invalid_argument("Decimation factor must be non-zero");
  }
}

std::vector<float> SignalGenerator::generateSincSignal(uint32_t numSamples,
                                                       uint32_t centralFreq,
                                                       uint32_t bandwidth,
                                                       float amplitude) const
{
  if (numSamples == 0)
  {
    throw std::invalid_argument("Number of samples must be positive");
  }

  if (centralFreq >= m_samplingFrequency / 2)
  {
    throw std::invalid_argument("Central frequency must be less than Nyquist frequency");
  }

  if (amplitude < 0.0f || amplitude > 1.0f)
  {
    throw std::invalid_argument("Amplitude must be between 0.0 and 1.0");
  }

  std::vector<float> signal(numSamples);

  for (uint32_t i = 0; i < numSamples; ++i)
  {
    double t = static_cast<double>(static_cast<int>(i) - static_cast<int>(numSamples / 2)) *
               m_decimationFactor / (m_samplingFrequency * m_decimationFactor);

    if (t == 0.0)
    {
      signal[i] = amplitude;
    }
    else
    {
      // Sinc function: sinc(t) = sin(π*t*BW) / (π*t*BW)
      // Modulated by carrier: cos(2π*fc*t)
      double sincArg = M_PI * t * bandwidth;
      double carrierArg = 2.0 * M_PI * centralFreq * t;

      signal[i] = amplitude * static_cast<float>((sin(sincArg) / sincArg) * cos(carrierArg));
    }
  }

  _applyHannWindow(signal);

  return signal;
}

std::vector<float> SignalGenerator::generateSineWave(uint32_t numSamples,
                                                     double frequency,
                                                     float amplitude) const
{
  if (numSamples == 0)
  {
    throw std::invalid_argument("Number of samples must be positive");
  }
  if (frequency >= m_samplingFrequency / 2)
  {
    throw std::invalid_argument("Frequency must be less than Nyquist frequency");
  }
  if (amplitude < 0.0f || amplitude > 1.0f)
  {
    throw std::invalid_argument("Amplitude must be between 0.0 and 1.0");
  }

  std::vector<float> signal(numSamples);

  for (uint32_t i = 0; i < numSamples; ++i)
  {
    double t = static_cast<double>(i) / m_samplingFrequency;
    signal[i] = amplitude * static_cast<float>(sin(2.0 * M_PI * frequency * t));
  }

  return signal;
}

void SignalGenerator::_applyHannWindow(std::vector<float>& signal) const
{
  const size_t N = signal.size();

  for (size_t i = 0; i < N; ++i)
  {
    // Hann window: w(n) = 0.5 * (1 - cos(2π*n/(N-1)))
    double window = 0.5 * (1.0 - cos(2.0 * M_PI * i / (N - 1)));
    signal[i] *= static_cast<float>(window);
  }
}
