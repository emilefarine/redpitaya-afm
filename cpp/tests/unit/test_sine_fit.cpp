#include "MathConstants.h"
#include "SineFit.h"

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

namespace
{

std::vector<float> sine(size_t n, double periods, double amplitude, double phaseRad,
                        double offset)
{
  const double omega = 2.0 * c_Pi * periods / static_cast<double>(n);
  std::vector<float> samples(n);
  for (size_t i = 0; i < n; ++i)
  {
    samples[i] = static_cast<float>(
        amplitude * std::sin(omega * static_cast<double>(i) + phaseRad) + offset);
  }
  return samples;
}

double omegaFor(size_t n, double periods)
{
  return 2.0 * c_Pi * periods / static_cast<double>(n);
}

TEST(SineFitTest, RecoversSineWithOffsetOverNonWholePeriods)
{
  const size_t n = 8192;
  for (double periods : {1.5, 4.3, 37.7})
  {
    auto samples = sine(n, periods, 0.5, 0.7, -0.34);
    SineFit::Result result = SineFit::fit(samples, n, omegaFor(n, periods));
    EXPECT_NEAR(result.amplitude, 0.5, 1e-5) << periods;
    EXPECT_NEAR(result.phaseRad, 0.7, 1e-5) << periods;
    EXPECT_NEAR(result.offset, -0.34, 1e-5) << periods;
  }
}

TEST(SineFitTest, PhaseIsRelativeToSine)
{
  const size_t n = 4096;
  EXPECT_NEAR(SineFit::fit(sine(n, 10.0, 1.0, -c_Pi / 6.0, 0.0), n, omegaFor(n, 10.0)).phaseRad,
              -c_Pi / 6.0, 1e-6);
  EXPECT_NEAR(SineFit::fit(sine(n, 10.0, 1.0, c_Pi / 2.0, 0.0), n, omegaFor(n, 10.0)).phaseRad,
              c_Pi / 2.0, 1e-6);
}

TEST(SineFitTest, UsesOnlyTheFirstCountSamples)
{
  const size_t n = 8192;
  auto samples = sine(n, 20.0, 0.25, 0.0, 0.0);
  samples.back() = 100.0f;
  SineFit::Result result = SineFit::fit(samples, n - 1, omegaFor(n, 20.0));
  EXPECT_NEAR(result.amplitude, 0.25, 1e-5);
  EXPECT_NEAR(result.phaseRad, 0.0, 1e-5);
}

TEST(SineFitTest, BelowOnePeriodFitsWithoutOffset)
{
  const size_t n = 8192;
  auto samples = sine(n, 0.8, 0.5, 0.3, 0.0);
  SineFit::Result result = SineFit::fit(samples, n, omegaFor(n, 0.8));
  EXPECT_NEAR(result.amplitude, 0.5, 1e-5);
  EXPECT_NEAR(result.phaseRad, 0.3, 1e-5);
  EXPECT_EQ(result.offset, 0.0);
}

TEST(SineFitTest, NoSamplesGivesZero)
{
  SineFit::Result result = SineFit::fit({}, 0, 0.1);
  EXPECT_EQ(result.amplitude, 0.0);
  EXPECT_EQ(result.offset, 0.0);
}

} // namespace
