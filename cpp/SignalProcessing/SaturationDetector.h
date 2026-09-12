#pragma once

#include <cmath>
#include <cstddef>
#include <vector>

/**
 * @brief Detects ADC saturation in acquired (volt-scaled) samples
 *
 * The custom FPGA datapath exposes no overrange flag, so saturation is
 * detected statistically: samples pinned at the input full scale indicate a
 * clipped acquisition. This catches real overrange even when the predicted
 * worst case (InputSafety) was optimistic.
 */
namespace SaturationDetector
{

struct Report
{
  bool saturated;
  size_t saturatedSamples; ///< Samples at or above the threshold
  float saturatedRatio;    ///< saturatedSamples / total samples (0 when empty)
};

/**
 * @brief Analyze acquired samples for saturation
 * @param samples Volt-scaled acquisition buffer
 * @param fullScale Input full scale in volts (default +/-1 V, LV range)
 * @param threshold Relative level above which a sample counts as saturated
 */
inline Report analyze(const std::vector<float>& samples, float fullScale = 1.0f,
                      float threshold = 0.99f)
{
  Report report;
  report.saturated = false;
  report.saturatedSamples = 0;
  report.saturatedRatio = 0.0f;

  if (samples.empty())
  {
    return report;
  }

  const float limit = threshold * fullScale;
  for (float sample : samples)
  {
    if (std::fabs(sample) >= limit)
    {
      ++report.saturatedSamples;
    }
  }

  report.saturatedRatio =
      static_cast<float>(report.saturatedSamples) / static_cast<float>(samples.size());
  report.saturated = report.saturatedSamples > 0;
  return report;
}

} // namespace SaturationDetector
