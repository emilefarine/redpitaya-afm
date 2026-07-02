#pragma once

#include <cstdint>
#include <string>
#include <vector>

/**
 * @brief Parameters that define a calibration context.
 *
 * Calibration data is only valid when the measurement uses the exact same
 * decimation, sample count, center/bandwidth, and amplitude.  If any of
 * these change the user must re-calibrate.
 */
struct CalibrationParams
{
  uint16_t decimation;
  float centerKHz;
  float bandwidthKHz;
  uint32_t numSamples;
  float amplitude;

  CalibrationParams()
      : decimation(0)
      , centerKHz(0.0f)
      , bandwidthKHz(0.0f)
      , numSamples(0)
      , amplitude(0.0f)
  {
  }

  bool operator==(const CalibrationParams& other) const
  {
    return decimation == other.decimation && numSamples == other.numSamples &&
           centerKHz == other.centerKHz && bandwidthKHz == other.bandwidthKHz &&
           amplitude == other.amplitude;
  }

  bool operator!=(const CalibrationParams& other) const
  {
    return !(*this == other);
  }
};

/**
 * @brief Manages loopback calibration for the AFM measurement chain.
 *
 * Workflow:
 *   1. User physically connects DAC OUT1 -> electronic board -> ADC IN1.
 *   2. A sinc measurement is run (same as MEASURE:SINC) and the resulting
 *      spectrum is stored here as the reference transfer function H_ref(f).
 *   3. On subsequent MEASURE:SINC calls the raw spectrum is divided by H_ref:
 *        Magnitude_cal(f) = Magnitude_raw(f) / Magnitude_ref(f)
 *        Phase_cal(f)     = Phase_raw(f)     - Phase_ref(f)
 *      This removes frequency-dependent gain/phase artifacts introduced by
 *      the DAC, ADC, cables, PGA, and MUX.
 *
 * The calibration is in-memory only and is lost on server restart.
 */
class CalibrationManager
{
public:
  CalibrationManager();

  /**
   * @brief Store a loopback reference spectrum.
   *
   * @param freqKHz     Frequency axis (kHz), monotonically increasing.
   * @param magnitude   Reference magnitude at each frequency bin.
   * @param phaseRad    Reference phase (rad) at each frequency bin.
   * @param params      Measurement parameters used during calibration.
   */
  void setCalibration(const std::vector<float>& freqKHz,
                      const std::vector<float>& magnitude,
                      const std::vector<float>& phaseRad,
                      const CalibrationParams& params);

  /**
   * @brief Apply calibration to a raw measurement (in-place).
   *
   * Divides magnitude and subtracts phase.  If a frequency bin in the raw
   * data falls outside the calibration range or the reference magnitude is
   * below a safety threshold, that bin is left unchanged.
   *
   * @param magnitude   [in/out] Raw magnitude to be corrected.
   * @param phaseRad    [in/out] Raw phase to be corrected.
   * @param freqKHz     Frequency axis of the raw measurement.
   * @return true if calibration was applied, false if skipped (not calibrated
   *         or vectors are empty).
   */
  bool applyCalibration(std::vector<float>& magnitude,
                        std::vector<float>& phaseRad,
                        const std::vector<float>& freqKHz) const;

  /** @brief Returns true if a reference spectrum is loaded. */
  bool isCalibrated() const;

  /**
   * @brief Check whether the stored calibration matches the given params.
   *
   * If the parameters do not match, the calibration should not be applied
   * (the user must re-calibrate).
   */
  bool isValidFor(const CalibrationParams& params) const;

  /** @brief Discard stored calibration data. */
  void clearCalibration();

  /** @brief Get the parameters used during calibration. */
  const CalibrationParams& getParams() const;

  /** @brief Build a human-readable status string for SCPI responses. */
  std::string statusString() const;

  /**
   * @brief Minimum reference magnitude for division.
   *
   * Bins where the reference magnitude is below this threshold are
   * skipped to avoid amplifying noise (division by near-zero).
   */
  static constexpr float MIN_REF_MAGNITUDE = 1e-9f;

private:
  bool m_calibrated;
  CalibrationParams m_params;

  std::vector<float> m_refFreqKHz;
  std::vector<float> m_refMagnitude;
  std::vector<float> m_refPhaseRad;

  /**
   * @brief Linear interpolation of the reference magnitude at a given freq.
   * @return Interpolated magnitude, or 0.0f if out of range.
   */
  float _interpolateMagnitude(float freqKHz) const;

  /**
   * @brief Linear interpolation of the reference phase at a given freq.
   * @return Interpolated phase in radians, or 0.0f if out of range.
   */
  float _interpolatePhase(float freqKHz) const;
};
