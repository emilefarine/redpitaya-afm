#include "InputSafety.h"

#include <gtest/gtest.h>

namespace
{

using GainSetting = ::GainSetting;

TEST(InputSafetyTest, GainFactorCoversAllSettings)
{
  EXPECT_FLOAT_EQ(0.125f, IElectronicBoard::gainFactor(GainSetting::GAIN_1_8));
  EXPECT_FLOAT_EQ(0.25f, IElectronicBoard::gainFactor(GainSetting::GAIN_1_4));
  EXPECT_FLOAT_EQ(0.5f, IElectronicBoard::gainFactor(GainSetting::GAIN_1_2));
  EXPECT_FLOAT_EQ(1.0f, IElectronicBoard::gainFactor(GainSetting::GAIN_1));
  EXPECT_FLOAT_EQ(2.0f, IElectronicBoard::gainFactor(GainSetting::GAIN_2));
  EXPECT_FLOAT_EQ(4.0f, IElectronicBoard::gainFactor(GainSetting::GAIN_4));
  EXPECT_FLOAT_EQ(8.0f, IElectronicBoard::gainFactor(GainSetting::GAIN_8));
  EXPECT_FLOAT_EQ(16.0f, IElectronicBoard::gainFactor(GainSetting::GAIN_16));
}

TEST(InputSafetyTest, BoardDrivePeakScalesWithGain)
{
  EXPECT_FLOAT_EQ(0.5f, InputSafety::boardDrivePeak(0.5f, GainSetting::GAIN_1));
  EXPECT_FLOAT_EQ(1.0f, InputSafety::boardDrivePeak(0.5f, GainSetting::GAIN_2));
  EXPECT_FLOAT_EQ(8.0f, InputSafety::boardDrivePeak(1.0f, GainSetting::GAIN_8));
}

TEST(InputSafetyTest, BoardDrivePeakClampsAtBoardRail)
{
  // 1 V * 16 = 16 V ideal, but the +/-12 V rails clip near +/-10.5 V
  EXPECT_FLOAT_EQ(InputSafety::BOARD_RAIL_VOLTAGE,
                  InputSafety::boardDrivePeak(1.0f, GainSetting::GAIN_16));
  EXPECT_FLOAT_EQ(InputSafety::BOARD_RAIL_VOLTAGE,
                  InputSafety::boardDrivePeak(0.8f, GainSetting::GAIN_16));
  EXPECT_FLOAT_EQ(2.0f, InputSafety::boardDrivePeak(0.5f, GainSetting::GAIN_4));
}

TEST(InputSafetyTest, EstimateAdcPeakCombinesBothChannels)
{
  // 0.5 V boosted x16 = 8 V, returned through x1/8 -> 1 V at the ADC
  EXPECT_FLOAT_EQ(1.0f, InputSafety::estimateAdcPeak(0.5f, GainSetting::GAIN_16,
                                                     GainSetting::GAIN_1_8));
  // Clamped board drive 10.5 V attenuated by x1/8
  EXPECT_FLOAT_EQ(1.3125f, InputSafety::estimateAdcPeak(1.0f, GainSetting::GAIN_16,
                                                        GainSetting::GAIN_1_8));
  // Unity loop
  EXPECT_FLOAT_EQ(0.4f, InputSafety::estimateAdcPeak(0.4f, GainSetting::GAIN_1,
                                                     GainSetting::GAIN_1));
}

TEST(InputSafetyTest, IsOverdriveFlagsOnlySaturationLevel)
{
  // Exactly at full scale is not reported; the ADC pins at +/-1 V only above it
  EXPECT_FALSE(InputSafety::isOverdrive(1.0f, GainSetting::GAIN_1, GainSetting::GAIN_1));
  EXPECT_TRUE(InputSafety::isOverdrive(1.0f, GainSetting::GAIN_2, GainSetting::GAIN_1));
  EXPECT_TRUE(InputSafety::isOverdrive(1.0f, GainSetting::GAIN_1, GainSetting::GAIN_2));
  EXPECT_FALSE(InputSafety::isOverdrive(0.4f, GainSetting::GAIN_2, GainSetting::GAIN_1));
}

TEST(InputSafetyTest, MaxSafeReturnGainFindsHighestSafeSetting)
{
  GainSetting safe = GainSetting::GAIN_16;
  ASSERT_TRUE(InputSafety::maxSafeReturnGain(1.0f, GainSetting::GAIN_1, safe));
  EXPECT_EQ(GainSetting::GAIN_1, safe);

  // x2 excite: x1/2 return gives a peak of exactly 1.0 V, which is allowed
  ASSERT_TRUE(InputSafety::maxSafeReturnGain(1.0f, GainSetting::GAIN_2, safe));
  EXPECT_EQ(GainSetting::GAIN_1_2, safe);

  // x4 excite: x1/4 return gives a peak of exactly 1.0 V
  ASSERT_TRUE(InputSafety::maxSafeReturnGain(1.0f, GainSetting::GAIN_4, safe));
  EXPECT_EQ(GainSetting::GAIN_1_4, safe);

  // x8 excite: even x1/4 gives 2.0 V, only x1/8 fits
  ASSERT_TRUE(InputSafety::maxSafeReturnGain(1.0f, GainSetting::GAIN_8, safe));
  EXPECT_EQ(GainSetting::GAIN_1_8, safe);
}

TEST(InputSafetyTest, MaxSafeReturnGainAtMaximumBoostHasNoSafeSetting)
{
  // x16 boost clamps at the rail; even x1/8 leaves ~1.31 V at the ADC
  GainSetting safe = GainSetting::GAIN_1;
  EXPECT_FALSE(InputSafety::maxSafeReturnGain(1.0f, GainSetting::GAIN_16, safe));
}

TEST(InputSafetyTest, MaxSafeReturnGainSucceedsWithReducedAmplitude)
{
  GainSetting safe = GainSetting::GAIN_1;
  ASSERT_TRUE(InputSafety::maxSafeReturnGain(0.5f, GainSetting::GAIN_16, safe));
  EXPECT_EQ(GainSetting::GAIN_1_8, safe);
}

} // namespace
