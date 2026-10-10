#pragma once

#include "IElectronicBoard.h"

/**
 * @brief Peak estimate at the Red Pitaya ADC input, used to refuse overdrive
 *
 * Loop: RP DAC -> board input (PGA x1/8..x16) -> AFM -> board input (PGA) -> RP ADC.
 *
 * The RP inputs stay on LV (+/-1 V): 6 V absolute maximum, and above 1 kHz the
 * full scale itself is the limit. The board can output about +/-10.5 V.
 *
 * The estimate is nominal: the AFM is assumed to return the excitation
 * scaled by the declared sample gain (default 1). SaturationDetector reports
 * what actually clipped.
 */
namespace InputSafety
{

/** ADC full scale in LV range (V), also the enforced limit */
static constexpr float ADC_FULL_SCALE = 1.0f;

/** DAC peak at amplitude 1.0 into 50 ohm (V); to be measured into the board input */
static constexpr float DAC_FULL_SCALE = 1.0f;

/** Board output clip level (V) */
static constexpr float BOARD_RAIL_VOLTAGE = 10.5f;

/** Highest valid SCPI gain index of the board PGA settings */
static constexpr uint8_t MAX_GAIN_INDEX = IElectronicBoard::MAX_GAIN_INDEX;

/** Board input wired to the RP DAC (IN1, 0-based) */
static constexpr uint8_t DAC_LINK_INPUT = 0;

/** Board output wired to the acquired RP ADC input (OUT1, 0-based) */
static constexpr uint8_t ADC_LINK_OUTPUT = 0;

/** Accepted range of the sample gain (return / excitation ratio of the AFM) */
static constexpr float MIN_SAMPLE_GAIN = 1.0e-6f;
static constexpr float MAX_SAMPLE_GAIN = 1.0e3f;

/**
 * @brief Peak voltage the board can drive towards the AFM
 * @param amplitude Excitation amplitude as a fraction of the DAC full scale
 * @param exciteGain Gain of the board input driven by the RP DAC
 */
inline float boardDrivePeak(float amplitude, GainSetting exciteGain)
{
  float peak = amplitude * DAC_FULL_SCALE * IElectronicBoard::gainFactor(exciteGain);
  if (peak > BOARD_RAIL_VOLTAGE)
  {
    peak = BOARD_RAIL_VOLTAGE;
  }
  return peak;
}

/** @brief Nominal peak at the ADC input; both gain stages clip at the board rail */
inline float estimateAdcPeak(float amplitude, GainSetting exciteGain, GainSetting returnGain,
                             float sampleGain = 1.0f)
{
  float peak = boardDrivePeak(amplitude, exciteGain) * sampleGain *
               IElectronicBoard::gainFactor(returnGain);
  if (peak > BOARD_RAIL_VOLTAGE)
  {
    peak = BOARD_RAIL_VOLTAGE;
  }
  return peak;
}

/** @brief True when the nominal peak would exceed the ADC full scale */
inline bool isOverdrive(float amplitude, GainSetting exciteGain, GainSetting returnGain,
                        float sampleGain = 1.0f)
{
  return estimateAdcPeak(amplitude, exciteGain, returnGain, sampleGain) > ADC_FULL_SCALE;
}

/**
 * @brief Highest return gain setting that keeps the nominal peak within
 *        the ADC full scale
 *
 * Gain factors strictly increase with the setting index, so the search can
 * stop at the first overdriving candidate.
 *
 * @param amplitude Excitation amplitude as a fraction of the DAC full scale
 * @param exciteGain Gain of the board input driven by the RP DAC
 * @param safeGainOut Receives the suggested gain when true is returned
 * @return true if a safe return gain setting exists
 */
inline bool maxSafeReturnGain(float amplitude, GainSetting exciteGain, GainSetting& safeGainOut,
                              float sampleGain = 1.0f)
{
  bool found = false;
  for (uint8_t idx = 0; idx <= MAX_GAIN_INDEX; ++idx)
  {
    GainSetting candidate = IElectronicBoard::gainFromIndex(idx);
    if (isOverdrive(amplitude, exciteGain, candidate, sampleGain))
    {
      break;
    }
    safeGainOut = candidate;
    found = true;
  }
  return found;
}

/**
 * @brief Highest excitation gain setting that keeps the nominal peak
 *        within the ADC full scale for the given return gain
 * @param amplitude Excitation amplitude as a fraction of the DAC full scale
 * @param returnGain Gain of the board input returning into the ADC
 * @param safeGainOut Receives the suggested gain when true is returned
 * @return true if a safe excitation gain setting exists
 */
inline bool maxSafeExciteGain(float amplitude, GainSetting returnGain, GainSetting& safeGainOut,
                              float sampleGain = 1.0f)
{
  bool found = false;
  for (uint8_t idx = 0; idx <= MAX_GAIN_INDEX; ++idx)
  {
    GainSetting candidate = IElectronicBoard::gainFromIndex(idx);
    if (isOverdrive(amplitude, candidate, returnGain, sampleGain))
    {
      break;
    }
    safeGainOut = candidate;
    found = true;
  }
  return found;
}

/** @brief Highest safe gain of an input routed straight to the ADC (one gain stage) */
inline bool maxSafeDirectGain(float amplitude, GainSetting& safeGainOut)
{
  bool found = false;
  for (uint8_t idx = 0; idx <= MAX_GAIN_INDEX; ++idx)
  {
    GainSetting candidate = IElectronicBoard::gainFromIndex(idx);
    if (boardDrivePeak(amplitude, candidate) > ADC_FULL_SCALE)
    {
      break;
    }
    safeGainOut = candidate;
    found = true;
  }
  return found;
}

} // namespace InputSafety
