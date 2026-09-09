#pragma once

#include <cstdint>

/**
 * @brief Portable DAC/ADC conversion helpers for the Red Pitaya 14-bit signal path
 *
 * Extracted from RedPitayaHardware so the conversion math can be unit-tested
 * on the host without /dev/mem access.
 */
namespace DacCodec
{

static constexpr int32_t MAX_DAC_VALUE = 8191;  // 14-bit signed max
static constexpr int32_t MIN_DAC_VALUE = -8192; // 14-bit signed min

/**
 * @brief Convert a voltage in [-1 V, 1 V] to the 14-bit signed DAC code
 *
 * Voltages outside the range are clamped.
 */
inline int16_t voltageToDAC(float voltage)
{
  // Clamp voltage to ±1V
  if (voltage > 1.0f)
  {
    voltage = 1.0f;
  }
  if (voltage < -1.0f)
  {
    voltage = -1.0f;
  }

  // Scale to 14-bit signed range
  float scale = static_cast<float>(MAX_DAC_VALUE - MIN_DAC_VALUE) / 2.0f;
  int16_t value = static_cast<int16_t>(voltage * scale);

  // Clamp to range
  if (value > MAX_DAC_VALUE)
  {
    value = MAX_DAC_VALUE;
  }
  if (value < MIN_DAC_VALUE)
  {
    value = MIN_DAC_VALUE;
  }

  return value;
}

/**
 * @brief Convert a raw ADC word to a voltage
 *
 * The FPGA packs the 14-bit signed ADC value in the upper bits and appends
 * extra precision bits (2 LSBs for decimation < 64, else 3 LSBs).
 */
inline float adcToVoltage(uint32_t rawValue, uint16_t decimation)
{
  uint8_t precisionBits;
  if (decimation < 64)
  {
    precisionBits = 2;
  }
  else
  {
    precisionBits = 3;
  }

  // Extract precision bits (LSBs) and 14-bit ADC value (upper bits)
  uint8_t precisionMask = static_cast<uint8_t>((1 << precisionBits) - 1);
  uint8_t precision = rawValue & precisionMask;
  uint16_t value14bit = (rawValue >> precisionBits) & 0x3FFF;

  // Sign-extend 14-bit value to 16-bit signed integer
  int16_t signedValue;
  if (value14bit & 0x2000) // Check bit 13 (sign bit of 14-bit number)
  {
    signedValue = static_cast<int16_t>(value14bit | 0xC000); // Sign-extend with 1s
  }
  else
  {
    signedValue = static_cast<int16_t>(value14bit);
  }

  float voltage = static_cast<float>(signedValue) / static_cast<float>(MAX_DAC_VALUE);

  float lsbVoltage = 1.0f / static_cast<float>(MAX_DAC_VALUE);
  float precisionFraction =
      static_cast<float>(precision) / static_cast<float>(1 << precisionBits);

  // Precision bits are the lower bits of the value, so they are always added
  // regardless of the sign of the upper bits (2's complement)
  voltage += precisionFraction * lsbVoltage;

  return voltage;
}

} // namespace DacCodec
