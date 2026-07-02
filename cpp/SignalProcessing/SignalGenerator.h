#pragma once

#include <cstdint>
#include <vector>

class SignalGenerator
{
public:
  SignalGenerator(double samplingFrequency = 125000000.0 /
                                             64.0, // 1.953125 MHz (125 MHz / 64 decimation)
                  uint16_t decimationFactor = 64   // FPGA decimation factor
  );

  std::vector<float> generateSincSignal(uint32_t numSamples,
                                        uint32_t centralFreq,
                                        uint32_t bandwidth,
                                        float amplitude = 1.0f) const;

  std::vector<float> generateSineWave(uint32_t numSamples,
                                      double frequency,
                                      float amplitude = 1.0f) const;

  double getSamplingFrequency() const
  {
    return m_samplingFrequency;
  }
  double getSamplingPeriod() const
  {
    return 1.0 / m_samplingFrequency;
  }
  uint16_t getDecimationFactor() const
  {
    return m_decimationFactor;
  }

private:
  double m_samplingFrequency;
  uint16_t m_decimationFactor;

  void _applyHannWindow(std::vector<float>& signal) const;
};
