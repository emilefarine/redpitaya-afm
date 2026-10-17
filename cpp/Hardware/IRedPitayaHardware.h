#pragma once

#include "HardwareLimits.h"

#include <cstdint>
#include <vector>

/**
 * @brief Interface for RedPitaya FPGA-based signal generation and acquisition
 *
 * Allows the server logic (CommandHandler) to be unit-tested against mocks
 * while the real implementation (RedPitayaHardware) talks to /dev/mem.
 */
class IRedPitayaHardware
{
public:
  virtual ~IRedPitayaHardware() = default;

  virtual bool initialize() = 0;
  virtual void cleanup() = 0;

  // Signal generation (DAC)
  virtual bool loadGenerationSignal(const std::vector<float>& signal) = 0;

  // Decimation control
  virtual bool setDecimation(uint16_t decimation) = 0;
  virtual uint16_t getDecimation() const = 0;

  // Signal acquisition (ADC)
  virtual bool startMeasurement(uint32_t numSamples, uint32_t delaySamples = 0) = 0;
  virtual bool isMeasurementComplete() = 0;
  virtual bool getAcquiredSignal(std::vector<float>& signal) = 0;

  virtual bool resetMeasurement() = 0;

  // 2^16 shared BRAM depth, see HardwareLimits.h
  static constexpr uint32_t MAX_SAMPLES = static_cast<uint32_t>(HardwareLimits::MAX_SAMPLES);
  static constexpr uint16_t DEFAULT_DECIMATION = 64;
};
