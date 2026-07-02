#pragma once

#include <complex>
#include <cstdint>
#include <vector>

struct ResonanceResults
{
  float peakFrequency;
  float peakAmplitude;
  float peakPhase;
  float qFactor;
  float centerFrequency;
  float bandwidth3dB;

  ResonanceResults()
      : peakFrequency(0)
      , peakAmplitude(0)
      , peakPhase(0)
      , qFactor(0)
      , centerFrequency(0)
      , bandwidth3dB(0)
  {
  }
};

class ResonanceAnalyzer
{
public:
  explicit ResonanceAnalyzer(double samplingFrequency);

  ResonanceResults analyzeResonance(const std::vector<float>& magnitudeSpectrum,
                                    const std::vector<float>& phaseSpectrum,
                                    float searchBandCenter,
                                    float searchBandWidth) const;

  uint32_t findPeakIndex(const std::vector<float>& magnitudeSpectrum,
                         float startFreq,
                         float endFreq) const;

  float calculateQFactor(const std::vector<float>& magnitudeSpectrum, uint32_t peakIndex) const;
  uint32_t frequencyToBin(float frequency, uint32_t numSamples) const;
  float binToFrequency(uint32_t binIndex, uint32_t numSamples) const;

  double getSamplingFrequency() const
  {
    return m_samplingFrequency;
  }

private:
  double m_samplingFrequency;

  void _find3dBBandwidth(const std::vector<float>& magnitudeSpectrum,
                         uint32_t peakIndex,
                         uint32_t& leftIndex,
                         uint32_t& rightIndex) const;
};
