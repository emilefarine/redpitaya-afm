#include "DacCodec.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <vector>

namespace
{

constexpr float c_LsbVoltage = 1.0f / 8191.0f;

// Pack a 14-bit signed DAC/ADC code into the raw FPGA word format
uint32_t packRaw(int16_t value14bit, uint8_t precision, uint16_t decimation)
{
  uint8_t precisionBits = (decimation < 64) ? 2 : 3;
  uint16_t encoded = static_cast<uint16_t>(value14bit) & 0x3FFF;
  return (static_cast<uint32_t>(encoded) << precisionBits) |
         (precision & ((1u << precisionBits) - 1));
}

TEST(DacCodecTest, ZeroVoltageGivesZeroCode)
{
  EXPECT_EQ(DacCodec::voltageToDAC(0.0f), 0);
}

TEST(DacCodecTest, FullScaleVoltagesGiveFullScaleCodes)
{
  EXPECT_EQ(DacCodec::voltageToDAC(1.0f), 8191);
  EXPECT_EQ(DacCodec::voltageToDAC(-1.0f), -8191); // truncation toward zero before clamp
}

TEST(DacCodecTest, OutOfRangeVoltagesAreClamped)
{
  EXPECT_EQ(DacCodec::voltageToDAC(5.0f), 8191);
  EXPECT_EQ(DacCodec::voltageToDAC(-5.0f), -8191);
}

TEST(DacCodecTest, HalfScaleVoltage)
{
  EXPECT_EQ(DacCodec::voltageToDAC(0.5f), 4095); // 0.5 * 8191.5 truncated
}

TEST(DacCodecTest, AdcZeroRawGivesZeroVolts)
{
  EXPECT_FLOAT_EQ(DacCodec::adcToVoltage(0, 64), 0.0f);
}

TEST(DacCodecTest, AdcFullScaleNegative)
{
  uint32_t raw = packRaw(-8192, 0, 64);
  EXPECT_NEAR(DacCodec::adcToVoltage(raw, 64), -8192.0f / 8191.0f, 1e-6f);
}

TEST(DacCodecTest, AdcExtractsPrecisionBitsHighDecimation)
{
  uint32_t raw = packRaw(100, 5, 64); // 3 precision bits
  float expected = 100.0f * c_LsbVoltage + (5.0f / 8.0f) * c_LsbVoltage;
  EXPECT_NEAR(DacCodec::adcToVoltage(raw, 64), expected, 1e-6f);
}

TEST(DacCodecTest, AdcExtractsPrecisionBitsLowDecimation)
{
  uint32_t raw = packRaw(100, 3, 32); // 2 precision bits below decimation 64
  float expected = 100.0f * c_LsbVoltage + (3.0f / 4.0f) * c_LsbVoltage;
  EXPECT_NEAR(DacCodec::adcToVoltage(raw, 32), expected, 1e-6f);
}

TEST(DacCodecTest, AdcNegativeValueWithPrecisionBits)
{
  uint32_t raw = packRaw(-100, 7, 64);
  float expected = -100.0f * c_LsbVoltage + (7.0f / 8.0f) * c_LsbVoltage;
  EXPECT_NEAR(DacCodec::adcToVoltage(raw, 64), expected, 1e-6f);
}

TEST(DacCodecTest, PrecisionBitBoundaryAtDecimation64)
{
  uint32_t rawLow = packRaw(100, 3, 63);  // 2 precision bits below decimation 64
  uint32_t rawHigh = packRaw(100, 3, 64); // 3 precision bits at decimation 64

  float low = DacCodec::adcToVoltage(rawLow, 63);
  float high = DacCodec::adcToVoltage(rawHigh, 64);

  EXPECT_NEAR(low, 100.0f * c_LsbVoltage + (3.0f / 4.0f) * c_LsbVoltage, 1e-6f);
  EXPECT_NEAR(high, 100.0f * c_LsbVoltage + (3.0f / 8.0f) * c_LsbVoltage, 1e-6f);
  EXPECT_GT(low, high);
}

TEST(DacCodecTest, ExtremeRawWordsAreFinite)
{
  for (uint32_t raw : {0u, 0xFFFFFFFFu, 0x00001FFFu, 0x00001FF0u})
  {
    EXPECT_TRUE(std::isfinite(DacCodec::adcToVoltage(raw, 64))) << "raw 0x" << std::hex << raw;
  }
}

TEST(DacCodecTest, VoltageAdcRoundTripWithinTwoLsb)
{
  const float voltages[] = {-1.0f, -0.75f, -0.5f, -0.25f, 0.0f,
                            0.25f, 0.5f,   0.75f, 1.0f};

  for (float v : voltages)
  {
    int16_t dac = DacCodec::voltageToDAC(v);
    uint32_t raw = packRaw(dac, 0, 64);
    float recovered = DacCodec::adcToVoltage(raw, 64);
    EXPECT_NEAR(recovered, v, 2.0f * c_LsbVoltage) << "voltage " << v;
  }
}

} // namespace
