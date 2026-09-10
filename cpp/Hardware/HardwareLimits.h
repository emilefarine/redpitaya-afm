#pragma once

#include <cstddef>
#include <cstdint>

/**
 * @brief Portable hardware limits and validation helpers for the FPGA datapath.
 *
 * Extracted from RedPitayaHardware so the bounds checks can be unit-tested on
 * the host without /dev/mem access, mirroring DacCodec.h.
 */
namespace HardwareLimits
{

static constexpr size_t MAX_SAMPLES = 65536; // 2^16 shared-BRAM depth

// measure_ctrl uses delay_eff[17:0], so larger delays alias when truncated.
static constexpr uint32_t MAX_DELAY_SAMPLES = (1u << 18) - 1;

constexpr bool isValidSampleCount(size_t numSamples)
{
  return numSamples <= MAX_SAMPLES;
}

constexpr bool isValidDelaySamples(uint32_t delaySamples)
{
  return delaySamples <= MAX_DELAY_SAMPLES;
}

// The RTL count_measure is a 16-bit counter that wraps to 0 after exactly
// 65536 samples and is currently cleared when acquisition stops. A zero
// reading therefore means "unknown"; any other value must match the request.
constexpr bool isCountConsistent(uint32_t acquiredCount, size_t expectedCount)
{
  return acquiredCount == 0 || acquiredCount == expectedCount;
}

} // namespace HardwareLimits
