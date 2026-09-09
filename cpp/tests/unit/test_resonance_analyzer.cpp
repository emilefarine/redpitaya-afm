#include "ResonanceAnalyzer.h"

#include <gtest/gtest.h>

#include <cmath>
#include <stdexcept>
#include <vector>

namespace
{

constexpr double c_Fs = 1000000.0;   // 1 MHz
constexpr uint32_t c_N = 65536;      // FFT size behind the spectrum
constexpr double c_Df = c_Fs / c_N;    // bin spacing (~15.26 Hz)
constexpr float c_F0 = 200000.0f;    // resonance frequency
constexpr float c_Bw3dB = 10000.0f;  // target -3 dB bandwidth

// Discrete-time Lorentzian whose -3 dB bandwidth is exactly c_Bw3dB:
// M(f) = 1 / (1 + ((f-f0)/g)^2), M = 1/sqrt(2) at |f-f0| = g*sqrt(sqrt(2)-1)
constexpr double c_LorentzGamma =
    c_Bw3dB / (2.0 * std::sqrt(std::sqrt(2.0) - 1.0));

std::vector<float> makeLorentzianSpectrum()
{
  std::vector<float> mag(c_N / 2 + 1, 0.0f);
  for (size_t i = 0; i < mag.size(); ++i)
  {
    double f = static_cast<double>(i) * c_Df;
    double x = (f - c_F0) / c_LorentzGamma;
    mag[i] = static_cast<float>(1.0 / (1.0 + x * x));
  }
  return mag;
}

class ResonanceAnalyzerTest : public ::testing::Test
{
protected:
  ResonanceAnalyzer analyzer{c_Fs};
};

TEST_F(ResonanceAnalyzerTest, ConstructorRejectsNonPositiveSamplingFrequency)
{
  EXPECT_THROW(ResonanceAnalyzer(0.0), std::invalid_argument);
  EXPECT_THROW(ResonanceAnalyzer(-1.0), std::invalid_argument);
}

TEST_F(ResonanceAnalyzerTest, FrequencyBinConversions)
{
  EXPECT_EQ(analyzer.frequencyToBin(1000.0f, c_N), 66u); // round(65.536)
  EXPECT_NEAR(analyzer.binToFrequency(66, c_N), 1007.08f, 0.01f);
  EXPECT_FLOAT_EQ(analyzer.binToFrequency(0, c_N), 0.0f);
}

TEST_F(ResonanceAnalyzerTest, BinConversionRoundTripWithinHalfBin)
{
  const float freqs[] = {0.0f, 12345.6f, c_F0, 350000.7f, 499000.0f};
  for (float f : freqs)
  {
    uint32_t bin = analyzer.frequencyToBin(f, c_N);
    EXPECT_LE(std::abs(analyzer.binToFrequency(bin, c_N) - f), c_Df / 2.0 + 1e-3f) << f;
  }
}

TEST_F(ResonanceAnalyzerTest, FindPeakIndexLocatesGlobalPeakInBand)
{
  auto mag = makeLorentzianSpectrum();
  uint32_t peak = analyzer.findPeakIndex(mag, c_F0 - 5000.0f, c_F0 + 5000.0f);
  EXPECT_EQ(peak, static_cast<uint32_t>(std::lround(c_F0 / c_Df)));
}

TEST_F(ResonanceAnalyzerTest, FindPeakIndexIgnoresPeaksOutsideSearchBand)
{
  auto mag = makeLorentzianSpectrum();

  // Strong interferer far outside the search band
  uint32_t interfererBin = static_cast<uint32_t>(std::lround(300000.0 / c_Df));
  mag[interfererBin] = 10.0f;

  uint32_t peak = analyzer.findPeakIndex(mag, c_F0 - 25000.0f, c_F0 + 25000.0f);
  EXPECT_EQ(peak, static_cast<uint32_t>(std::lround(c_F0 / c_Df)));
}

TEST_F(ResonanceAnalyzerTest, FindPeakIndexFindsLocalMaximumInNarrowBand)
{
  auto mag = makeLorentzianSpectrum();
  for (float& v : mag)
    v += 0.05f; // noise floor

  // Local bump at 192 kHz, clearly above the Lorentzian tail in the band
  uint32_t bumpBin = static_cast<uint32_t>(std::lround(192000.0 / c_Df));
  mag[bumpBin] += 0.6f;

  uint32_t peak = analyzer.findPeakIndex(mag, 190000.0f, 196000.0f);
  EXPECT_EQ(peak, bumpBin);
}

TEST_F(ResonanceAnalyzerTest, AnalyzeResonanceRecoversLorentzianParameters)
{
  auto mag = makeLorentzianSpectrum();
  std::vector<float> phase(mag.size(), 0.0f);

  auto res = analyzer.analyzeResonance(mag, phase, c_F0, 50000.0f);

  EXPECT_NEAR(res.peakFrequency, c_F0, c_Df);
  EXPECT_NEAR(res.peakAmplitude, 1.0f, 1e-3f);
  EXPECT_FLOAT_EQ(res.peakPhase, 0.0f);
  EXPECT_NEAR(res.bandwidth3dB, c_Bw3dB, 2.0 * c_Df); // crossing resolution: one bin per side
  EXPECT_NEAR(res.centerFrequency, c_F0, c_Df);
  EXPECT_NEAR(res.qFactor, c_F0 / c_Bw3dB, 0.3f);
}

TEST_F(ResonanceAnalyzerTest, CalculateQFactorMatchesPeakOverBandwidth)
{
  auto mag = makeLorentzianSpectrum();
  uint32_t peakBin = static_cast<uint32_t>(std::lround(c_F0 / c_Df));

  float q = analyzer.calculateQFactor(mag, peakBin);
  EXPECT_NEAR(q, c_F0 / c_Bw3dB, 0.3f);
}

TEST_F(ResonanceAnalyzerTest, AnalyzeResonanceRejectsInvalidSpectra)
{
  std::vector<float> mag(64, 1.0f);
  std::vector<float> shortPhase(32, 0.0f);

  EXPECT_THROW(analyzer.analyzeResonance(mag, shortPhase, c_F0, 1000.0f),
               std::invalid_argument);
  EXPECT_THROW(analyzer.analyzeResonance({}, {}, c_F0, 1000.0f), std::invalid_argument);
}

} // namespace
