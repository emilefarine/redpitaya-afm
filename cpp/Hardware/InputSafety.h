#pragma once

#include "IElectronicBoard.h"

/**
 * @brief Portable safety estimates for the Red Pitaya ADC input path
 *
 * Signal loop: the Red Pitaya DAC (<= +/-1 V) drives a board input whose
 * PGA849 can boost it (x1/8 .. x16), the signal leaves the board towards the
 * AFM and the response returns through another board input channel into the
 * Red Pitaya ADC. The Red Pitaya input survives up to +/-30 V, but its LV
 * range (jumper selectable) saturates beyond +/-1 V, so this header models
 * the worst case peak at the ADC to protect measurement validity.
 *
 * The board output stage clips near +/-10.5 V (OPA828 buffers on +/-12 V
 * rails, TVS clamps at ~12 V), so no reachable signal can damage the ADC.
 */
namespace InputSafety
{

/** Full scale of the Red Pitaya ADC input in LV range (V) */
static constexpr float ADC_FULL_SCALE = 1.0f;

/** Approximate clip level of the board output buffers (V) */
static constexpr float BOARD_RAIL_VOLTAGE = 10.5f;

/**
 * @brief Peak voltage the board can drive towards the AFM
 * @param amplitude Excitation amplitude as a fraction of the +/-1 V DAC range
 * @param exciteGain Gain of the board input driven by the RP DAC
 */
inline float boardDrivePeak(float amplitude, GainSetting exciteGain)
{
  float peak = amplitude * IElectronicBoard::gainFactor(exciteGain);
  if (peak > BOARD_RAIL_VOLTAGE)
  {
    peak = BOARD_RAIL_VOLTAGE;
  }
  return peak;
}

/**
 * @brief Worst case peak voltage at the ADC input
 *
 * Assumes the AFM transmits the excitation amplitude back unchanged (worst
 * case, e.g. a direct cable loop during setup).
 */
inline float estimateAdcPeak(float amplitude, GainSetting exciteGain, GainSetting returnGain)
{
  return boardDrivePeak(amplitude, exciteGain) * IElectronicBoard::gainFactor(returnGain);
}

/** @brief True when the worst case peak would saturate the ADC input */
inline bool isOverdrive(float amplitude, GainSetting exciteGain, GainSetting returnGain)
{
  return estimateAdcPeak(amplitude, exciteGain, returnGain) > ADC_FULL_SCALE;
}

/**
 * @brief Highest return gain setting that keeps the worst case peak within
 *        the ADC full scale
 * @param amplitude Excitation amplitude as a fraction of the +/-1 V DAC range
 * @param exciteGain Gain of the board input driven by the RP DAC
 * @param safeGainOut Receives the suggested gain when true is returned
 * @return true if a safe return gain setting exists
 */
inline bool maxSafeReturnGain(float amplitude, GainSetting exciteGain, GainSetting& safeGainOut)
{
  bool found = false;
  for (uint8_t idx = 0; idx <= 7; ++idx)
  {
    GainSetting candidate = static_cast<GainSetting>(idx);
    if (!isOverdrive(amplitude, exciteGain, candidate))
    {
      safeGainOut = candidate;
      found = true;
    }
  }
  return found;
}

} // namespace InputSafety
