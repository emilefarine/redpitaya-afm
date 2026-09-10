#include "FFTProcessor.h"

#include <gtest/gtest.h>

#include <cmath>
#include <stdexcept>
#include <vector>

namespace
{

constexpr double c_Pi = 3.14159265358979323846;
constexpr double c_Fs = 1000000.0; // 1 MHz test sampling frequency

std::vector<float> makeSine(uint32_t n, double cycles, float amp = 1.0f)
{
  std::vector<float> s(n);
  for (uint32_t i = 0; i < n; ++i)
    s[i] = amp * static_cast<float>(sin(2.0 * c_Pi * cycles * static_cast<double>(i) / n));
  return s;
}

} // namespace

TEST(FftProcessorTest, ConstructorRejectsNonPositiveSamplingFrequency)
{
  EXPECT_THROW(FFTProcessor(0.0), std::invalid_argument);
  EXPECT_THROW(FFTProcessor(-100.0), std::invalid_argument);
}

TEST(FftProcessorTest, ComputeFftRejectsEmptyInput)
{
  FFTProcessor fft(c_Fs);
  EXPECT_THROW(fft.computeFFT({}), std::invalid_argument);
}

TEST(FftProcessorTest, PureSineGivesSinglePeakBin)
{
  const uint32_t N = 4096;
  const uint32_t bin = 100;

  FFTProcessor fft(c_Fs);
  auto fftData = fft.computeFFT(makeSine(N, bin));
  ASSERT_EQ(fftData.size(), N / 2 + 1);

  auto mag = fft.computeMagnitudeSpectrum(fftData);
  EXPECT_NEAR(mag[bin], 0.5f, 1e-3f); // normalized by N: A/2 for sine
  EXPECT_NEAR(mag[0], 0.0f, 1e-4f);

  for (size_t i = 1; i < mag.size(); ++i)
  {
    if (i != bin)
    {
      EXPECT_LT(mag[i], 1e-3f) << "unexpected energy at bin " << i;
    }
  }
}

TEST(FftProcessorTest, PhaseAtPeakBinIsMinusPiOverTwo)
{
  const uint32_t N = 1024;
  FFTProcessor fft(c_Fs);
  auto phase = fft.computePhaseSpectrum(fft.computeFFT(makeSine(N, 64)));
  EXPECT_NEAR(phase[64], -c_Pi / 2, 1e-3);
}

TEST(FftProcessorTest, ConstantSignalGivesDcBinOnly)
{
  const uint32_t N = 256;
  FFTProcessor fft(c_Fs);
  auto mag = fft.computeMagnitudeSpectrum(fft.computeFFT(std::vector<float>(N, 0.25f)));

  EXPECT_NEAR(mag[0], 0.25f, 1e-5f);
  for (size_t i = 1; i < mag.size(); ++i)
    EXPECT_LT(mag[i], 1e-6f) << "unexpected energy at bin " << i;
}

TEST(FftProcessorTest, FrequencyAxisMatchesSamplingTheorem)
{
  const uint32_t N = 1000;
  FFTProcessor fft(c_Fs);
  auto axis = fft.getFrequencyAxis(N);

  ASSERT_EQ(axis.size(), N / 2 + 1u);
  EXPECT_FLOAT_EQ(axis[0], 0.0f);
  EXPECT_NEAR(axis.back(), c_Fs / 2, 1.0f);
  EXPECT_NEAR(axis[10], 10.0 * c_Fs / N, 1e-3f);
}

TEST(FftProcessorTest, PlanReusedAcrossCallsAndSizes)
{
  const uint32_t N = 512;
  FFTProcessor fft(c_Fs);

  auto a = fft.computeFFT(makeSine(N, 32));
  auto b = fft.computeFFT(makeSine(N, 32));
  ASSERT_EQ(a.size(), b.size());
  for (size_t i = 0; i < a.size(); ++i)
  {
    EXPECT_FLOAT_EQ(a[i].real(), b[i].real());
    EXPECT_FLOAT_EQ(a[i].imag(), b[i].imag());
  }

  auto c = fft.computeFFT(makeSine(2 * N, 64));
  ASSERT_EQ(c.size(), N + 1u);
  EXPECT_NEAR(std::abs(c[64]), 0.5f, 1e-3f);
}

TEST(FftProcessorTest, ApplyWindowRectangleLeavesSignalUnchanged)
{
  const uint32_t N = 64;
  FFTProcessor fft(c_Fs);
  auto sig = makeSine(N, 7.25);
  auto copy = sig;

  fft.applyWindow(sig, WindowType::Rectangle);
  EXPECT_EQ(sig, copy);
}

TEST(FftProcessorTest, ApplyWindowEndpointValues)
{
  const uint32_t N = 128;
  FFTProcessor fft(c_Fs);
  std::vector<float> ones(N, 1.0f);

  auto hann = ones;
  fft.applyWindow(hann, WindowType::Hann);
  EXPECT_FLOAT_EQ(hann.front(), 0.0f);
  EXPECT_FLOAT_EQ(hann.back(), 0.0f);
  EXPECT_NEAR(hann[N / 2], 1.0f, 1e-3f);

  auto hamming = ones;
  fft.applyWindow(hamming, WindowType::Hamming);
  EXPECT_NEAR(hamming.front(), 0.08f, 1e-3f);
  EXPECT_NEAR(hamming[N / 2], 1.0f, 1e-3f);

  auto blackman = ones;
  fft.applyWindow(blackman, WindowType::Blackman);
  EXPECT_NEAR(blackman.front(), 0.0f, 1e-3f);
  EXPECT_NEAR(blackman[N / 2], 1.0f, 1e-3f);
}

TEST(FftProcessorTest, ApplyWindowSingleSampleIsNoOp)
{
  FFTProcessor fft(c_Fs);

  for (WindowType type : {WindowType::Hann, WindowType::Hamming, WindowType::Blackman})
  {
    std::vector<float> single = {0.5f};
    fft.applyWindow(single, type);
    EXPECT_TRUE(std::isfinite(single[0]));
    EXPECT_FLOAT_EQ(single[0], 0.5f);
  }
}

TEST(FftProcessorTest, HannWindowReducesSpectralLeakage)
{
  const uint32_t N = 2048;
  const double cycles = 100.5; // non-integer: forces leakage with rectangular window

  FFTProcessor fft(c_Fs);
  auto sig = makeSine(N, cycles);

  auto rectMag = fft.computeMagnitudeSpectrum(fft.computeFFT(sig));

  auto windowed = sig;
  fft.applyWindow(windowed, WindowType::Hann);
  auto hannMag = fft.computeMagnitudeSpectrum(fft.computeFFT(windowed));

  auto offMainLobeEnergy = [](const std::vector<float>& m) {
    double e = 0.0;
    for (size_t i = 1; i < m.size(); ++i)
    {
      if (i < 98 || i > 103)
        e += static_cast<double>(m[i]) * m[i];
    }
    return e;
  };

  EXPECT_LT(offMainLobeEnergy(hannMag), 0.01 * offMainLobeEnergy(rectMag));
}
