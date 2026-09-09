#include "FFTProcessor.h"
#include "SignalGenerator.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <vector>

namespace
{

constexpr double c_Pi = 3.14159265358979323846;
constexpr double c_Fs = 1000000.0; // 1 MHz test sampling frequency

class SignalGeneratorTest : public ::testing::Test
{
protected:
  SignalGenerator gen{c_Fs, 64};
};

TEST_F(SignalGeneratorTest, DefaultConstructorUsesRedPitayaDefaults)
{
  SignalGenerator def;
  EXPECT_DOUBLE_EQ(def.getSamplingFrequency(), 125000000.0 / 64.0);
  EXPECT_EQ(def.getDecimationFactor(), 64);
}

TEST_F(SignalGeneratorTest, AccessorsReturnConstructorValues)
{
  EXPECT_DOUBLE_EQ(gen.getSamplingFrequency(), c_Fs);
  EXPECT_EQ(gen.getDecimationFactor(), 64);
  EXPECT_DOUBLE_EQ(gen.getSamplingPeriod(), 1.0 / c_Fs);
}

TEST_F(SignalGeneratorTest, ConstructorRejectsInvalidParameters)
{
  EXPECT_THROW(SignalGenerator(0.0, 64), std::invalid_argument);
  EXPECT_THROW(SignalGenerator(-100.0, 64), std::invalid_argument);
  EXPECT_THROW(SignalGenerator(c_Fs, 0), std::invalid_argument);
}

TEST_F(SignalGeneratorTest, SineWaveMatchesAnalyticalExpression)
{
  const uint32_t N = 1000;
  const double f = 100.0;
  const float amp = 0.5f;

  auto sig = gen.generateSineWave(N, f, amp);
  ASSERT_EQ(sig.size(), N);

  for (uint32_t i = 0; i < N; ++i)
  {
    double t = static_cast<double>(i) / c_Fs;
    float expected = amp * static_cast<float>(sin(2.0 * c_Pi * f * t));
    EXPECT_NEAR(sig[i], expected, 1e-5f) << "sample " << i;
  }
}

TEST_F(SignalGeneratorTest, SineWaveKnownSampleValues)
{
  const uint32_t N = 1000;
  auto sig = gen.generateSineWave(N, 1000.0, 0.5f); // exactly one period over N

  EXPECT_NEAR(sig[0], 0.0f, 1e-6f);      // zero crossing at start
  EXPECT_NEAR(sig[250], 0.5f, 1e-4f);    // peak at quarter period
  EXPECT_NEAR(sig[500], 0.0f, 1e-6f);    // zero crossing at half period
  EXPECT_NEAR(sig[750], -0.5f, 1e-4f);   // trough at three quarter periods
}

TEST_F(SignalGeneratorTest, SineWaveRejectsInvalidParameters)
{
  EXPECT_THROW(gen.generateSineWave(0, 100.0), std::invalid_argument);
  EXPECT_THROW(gen.generateSineWave(1000, c_Fs / 2), std::invalid_argument);
  EXPECT_THROW(gen.generateSineWave(1000, c_Fs), std::invalid_argument);
  EXPECT_THROW(gen.generateSineWave(1000, 100.0, -0.1f), std::invalid_argument);
  EXPECT_THROW(gen.generateSineWave(1000, 100.0, 1.1f), std::invalid_argument);
}

TEST_F(SignalGeneratorTest, SincCenterSampleMatchesAmplitude)
{
  const uint32_t N = 1024;
  auto sig = gen.generateSincSignal(N, 200000, 100000, 0.8f);
  ASSERT_EQ(sig.size(), N);

  EXPECT_NEAR(std::abs(sig[N / 2]), 0.8f, 0.05f);
}

TEST_F(SignalGeneratorTest, SincIsHannWindowed)
{
  const uint32_t N = 1024;
  auto sig = gen.generateSincSignal(N, 200000, 100000, 1.0f);

  EXPECT_FLOAT_EQ(sig.front(), 0.0f);
  EXPECT_FLOAT_EQ(sig.back(), 0.0f);

  double energy = 0.0;
  for (float s : sig)
    energy += static_cast<double>(s) * s;
  EXPECT_GT(energy, 0.0);
}

TEST_F(SignalGeneratorTest, SincEnergyConcentratedInBand)
{
  const uint32_t N = 8192;
  const uint32_t fc = 200000;
  const uint32_t bw = 100000;

  auto sig = gen.generateSincSignal(N, fc, bw, 1.0f);

  FFTProcessor fft(c_Fs);
  auto mag = fft.computeMagnitudeSpectrum(fft.computeFFT(sig));
  auto axis = fft.getFrequencyAxis(N);

  double inBand = 0.0;
  double outOfBand = 0.0;
  for (size_t i = 0; i < mag.size(); ++i)
  {
    double p = static_cast<double>(mag[i]) * mag[i];
    if (axis[i] >= fc - bw / 2.0 && axis[i] <= fc + bw / 2.0)
      inBand += p;
    else
      outOfBand += p;
  }
  EXPECT_GT(inBand, 20.0 * outOfBand);
}

TEST_F(SignalGeneratorTest, SincSpectrumPeakInsidePassband)
{
  const uint32_t N = 8192;
  const uint32_t fc = 200000;
  const uint32_t bw = 100000;

  auto sig = gen.generateSincSignal(N, fc, bw, 1.0f);

  FFTProcessor fft(c_Fs);
  auto mag = fft.computeMagnitudeSpectrum(fft.computeFFT(sig));
  auto axis = fft.getFrequencyAxis(N);

  // A sinc pulse has a flat passband: the maximum must lie inside
  // [fc - bw/2, fc + bw/2], not in the stopband.
  size_t peakIdx = std::max_element(mag.begin() + 1, mag.end()) - mag.begin();
  EXPECT_GE(axis[peakIdx], static_cast<float>(fc - bw / 2));
  EXPECT_LE(axis[peakIdx], static_cast<float>(fc + bw / 2));
}

TEST_F(SignalGeneratorTest, SincAmplitudeScaling)
{
  const uint32_t N = 1024;
  auto small = gen.generateSincSignal(N, 200000, 100000, 0.25f);
  auto large = gen.generateSincSignal(N, 200000, 100000, 0.75f);

  for (size_t i = 0; i < N; ++i)
    EXPECT_NEAR(large[i], 3.0f * small[i], 1e-4f);
}

TEST_F(SignalGeneratorTest, SincRejectsInvalidParameters)
{
  EXPECT_THROW(gen.generateSincSignal(0, 200000, 100000), std::invalid_argument);
  EXPECT_THROW(gen.generateSincSignal(1024, c_Fs / 2, 100000), std::invalid_argument);
  EXPECT_THROW(gen.generateSincSignal(1024, 200000, 100000, -0.5f), std::invalid_argument);
  EXPECT_THROW(gen.generateSincSignal(1024, 200000, 100000, 1.5f), std::invalid_argument);
}

} // namespace
