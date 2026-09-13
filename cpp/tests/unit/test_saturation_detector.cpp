#include "SaturationDetector.h"

#include <gtest/gtest.h>

#include <vector>

namespace
{

TEST(SaturationDetectorTest, EmptyBufferIsNotSaturated)
{
  auto report = SaturationDetector::analyze({});
  EXPECT_FALSE(report.saturated);
  EXPECT_EQ(report.saturatedSamples, 0u);
  EXPECT_FLOAT_EQ(report.saturatedRatio, 0.0f);
}

TEST(SaturationDetectorTest, CleanSignalIsNotSaturated)
{
  std::vector<float> samples(1000, 0.5f);
  auto report = SaturationDetector::analyze(samples);
  EXPECT_FALSE(report.saturated);
  EXPECT_EQ(report.saturatedSamples, 0u);
  EXPECT_FLOAT_EQ(report.saturatedRatio, 0.0f);
}

TEST(SaturationDetectorTest, PinnedSamplesCountAsSaturated)
{
  std::vector<float> samples(100, 1.0f);
  auto report = SaturationDetector::analyze(samples);
  EXPECT_TRUE(report.saturated);
  EXPECT_EQ(report.saturatedSamples, 100u);
  EXPECT_FLOAT_EQ(report.saturatedRatio, 1.0f);
}

TEST(SaturationDetectorTest, NegativeClippedSamplesCountAsSaturated)
{
  std::vector<float> samples = {-1.0f, -1.0f, 0.0f, 0.25f, -0.5f};
  auto report = SaturationDetector::analyze(samples);
  EXPECT_TRUE(report.saturated);
  EXPECT_EQ(report.saturatedSamples, 2u);
  EXPECT_FLOAT_EQ(report.saturatedRatio, 0.4f);
}

TEST(SaturationDetectorTest, RatioCountsSamplesAboveThreshold)
{
  std::vector<float> samples(10, 0.5f);
  samples[0] = 0.995f;
  samples[1] = -0.999f;
  auto report = SaturationDetector::analyze(samples);
  EXPECT_TRUE(report.saturated);
  EXPECT_EQ(report.saturatedSamples, 2u);
  EXPECT_FLOAT_EQ(report.saturatedRatio, 0.2f);
}

TEST(SaturationDetectorTest, ThresholdBoundaryIsInclusive)
{
  // Threshold 0.99 counts a sample exactly at 0.99, but not 0.989
  std::vector<float> below = {0.989f, 0.0f};
  EXPECT_FALSE(SaturationDetector::analyze(below).saturated);

  std::vector<float> atLimit = {0.99f, 0.0f};
  EXPECT_TRUE(SaturationDetector::analyze(atLimit).saturated);
}

TEST(SaturationDetectorTest, CustomFullScaleScalesTheLimit)
{
  // With a +/-2 V full scale, 1.5 V is far below the saturation threshold
  std::vector<float> samples(50, 1.5f);
  EXPECT_FALSE(SaturationDetector::analyze(samples, 2.0f).saturated);
  EXPECT_TRUE(SaturationDetector::analyze(samples, 1.0f).saturated);
}

} // namespace
