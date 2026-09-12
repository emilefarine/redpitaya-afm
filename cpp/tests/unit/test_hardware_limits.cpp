#include "HardwareLimits.h"

#include <gtest/gtest.h>

#include <cstdint>

TEST(HardwareLimitsTest, SampleCountBoundaries)
{
  EXPECT_TRUE(HardwareLimits::isValidSampleCount(0));
  EXPECT_TRUE(HardwareLimits::isValidSampleCount(1));
  EXPECT_TRUE(HardwareLimits::isValidSampleCount(HardwareLimits::MAX_SAMPLES));
  EXPECT_FALSE(HardwareLimits::isValidSampleCount(HardwareLimits::MAX_SAMPLES + 1));
}

TEST(HardwareLimitsTest, DelayBoundaries)
{
  EXPECT_TRUE(HardwareLimits::isValidDelaySamples(0)); // clamped to 1 downstream
  EXPECT_TRUE(HardwareLimits::isValidDelaySamples(1));
  EXPECT_TRUE(HardwareLimits::isValidDelaySamples(HardwareLimits::MAX_DELAY_SAMPLES));
  EXPECT_FALSE(HardwareLimits::isValidDelaySamples(HardwareLimits::MAX_DELAY_SAMPLES + 1));
}

TEST(HardwareLimitsTest, CountZeroIsUnknown)
{
  EXPECT_TRUE(HardwareLimits::isCountConsistent(0, 8192));
  EXPECT_TRUE(HardwareLimits::isCountConsistent(0, HardwareLimits::MAX_SAMPLES));
}

TEST(HardwareLimitsTest, CountMustMatchWhenPresent)
{
  EXPECT_TRUE(HardwareLimits::isCountConsistent(8192, 8192));
  EXPECT_FALSE(HardwareLimits::isCountConsistent(8191, 8192));
  EXPECT_FALSE(HardwareLimits::isCountConsistent(65535, HardwareLimits::MAX_SAMPLES));
}
