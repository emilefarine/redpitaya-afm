#include "ResonanceAnalyzer.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

ResonanceAnalyzer::ResonanceAnalyzer(double samplingFrequency)
    : m_samplingFrequency(samplingFrequency)
{
  if (samplingFrequency <= 0)
  {
    throw std::invalid_argument("Sampling frequency must be positive");
  }
}

ResonanceResults ResonanceAnalyzer::analyzeResonance(const std::vector<float>& magnitudeSpectrum,
                                                     const std::vector<float>& phaseSpectrum,
                                                     float searchBandCenter,
                                                     float searchBandWidth) const
{
  if (magnitudeSpectrum.size() != phaseSpectrum.size())
  {
    throw std::invalid_argument("Magnitude and phase spectra must have same size");
  }

  if (magnitudeSpectrum.empty())
  {
    throw std::invalid_argument("Spectra cannot be empty");
  }

  ResonanceResults results;

  float searchStart = searchBandCenter - searchBandWidth / 2.0f;
  float searchEnd = searchBandCenter + searchBandWidth / 2.0f;
  uint32_t peakIndex = findPeakIndex(magnitudeSpectrum, searchStart, searchEnd);

  uint32_t numSamples = magnitudeSpectrum.size() * 2; // Assuming real input (N/2+1 spectrum)
  results.peakFrequency = binToFrequency(peakIndex, numSamples);
  results.peakAmplitude = magnitudeSpectrum[peakIndex];
  results.peakPhase = phaseSpectrum[peakIndex];

  results.qFactor = calculateQFactor(magnitudeSpectrum, peakIndex);

  uint32_t leftIndex, rightIndex;
  _find3dBBandwidth(magnitudeSpectrum, peakIndex, leftIndex, rightIndex);

  float leftFreq = binToFrequency(leftIndex, numSamples);
  float rightFreq = binToFrequency(rightIndex, numSamples);

  results.centerFrequency = (leftFreq + rightFreq) / 2.0f;
  results.bandwidth3dB = rightFreq - leftFreq;

  return results;
}

uint32_t ResonanceAnalyzer::findPeakIndex(const std::vector<float>& magnitudeSpectrum,
                                          float startFreq,
                                          float endFreq) const
{
  uint32_t numSamples = magnitudeSpectrum.size() * 2;

  uint32_t startBin = frequencyToBin(startFreq, numSamples);
  uint32_t endBin = frequencyToBin(endFreq, numSamples);

  startBin = std::max(static_cast<uint32_t>(0), startBin);
  endBin = std::min(static_cast<uint32_t>(magnitudeSpectrum.size() - 1), endBin);

  uint32_t peakIndex = startBin;
  float maxAmplitude = magnitudeSpectrum[startBin];

  for (uint32_t i = startBin; i <= endBin; ++i)
  {
    if (magnitudeSpectrum[i] > maxAmplitude)
    {
      maxAmplitude = magnitudeSpectrum[i];
      peakIndex = i;
    }
  }

  return peakIndex;
}

float ResonanceAnalyzer::calculateQFactor(const std::vector<float>& magnitudeSpectrum,
                                          uint32_t peakIndex) const
{
  uint32_t leftIndex, rightIndex;
  _find3dBBandwidth(magnitudeSpectrum, peakIndex, leftIndex, rightIndex);

  uint32_t numSamples = magnitudeSpectrum.size() * 2;

  float peakFreq = binToFrequency(peakIndex, numSamples);
  float leftFreq = binToFrequency(leftIndex, numSamples);
  float rightFreq = binToFrequency(rightIndex, numSamples);

  float bandwidth = rightFreq - leftFreq;

  if (bandwidth <= 0)
  {
    return 0.0f;
  }

  return peakFreq / bandwidth;
}

uint32_t ResonanceAnalyzer::frequencyToBin(float frequency, uint32_t numSamples) const
{
  float binFloat = frequency * numSamples / m_samplingFrequency;
  return static_cast<uint32_t>(std::round(binFloat));
}

float ResonanceAnalyzer::binToFrequency(uint32_t binIndex, uint32_t numSamples) const
{
  return static_cast<float>(binIndex) * m_samplingFrequency / numSamples;
}

void ResonanceAnalyzer::_find3dBBandwidth(const std::vector<float>& magnitudeSpectrum,
                                          uint32_t peakIndex,
                                          uint32_t& leftIndex,
                                          uint32_t& rightIndex) const
{
  // -3dB threshold is peak amplitude / sqrt(2)
  float peakAmplitude = magnitudeSpectrum[peakIndex];
  float threshold = peakAmplitude / std::sqrt(2.0f);

  // Search left from peak
  leftIndex = peakIndex;
  while (leftIndex > 0 && magnitudeSpectrum[leftIndex] > threshold)
  {
    leftIndex--;
  }

  // Search right from peak
  rightIndex = peakIndex;
  while (rightIndex < magnitudeSpectrum.size() - 1 && magnitudeSpectrum[rightIndex] > threshold)
  {
    rightIndex++;
  }

  // Ensure we found valid -3dB points
  if (leftIndex == 0 && magnitudeSpectrum[leftIndex] > threshold)
  {
    leftIndex = 0; // Hit spectrum boundary
  }
  if (rightIndex == magnitudeSpectrum.size() - 1 && magnitudeSpectrum[rightIndex] > threshold)
  {
    rightIndex = magnitudeSpectrum.size() - 1; // Hit spectrum boundary
  }
}