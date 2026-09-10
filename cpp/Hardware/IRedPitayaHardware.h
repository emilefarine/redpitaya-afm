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

  virtual uint32_t getVersion() = 0; // Returns 0x15062026 (date) if correct bitfile loaded

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
  virtual uint32_t readStatusRegister() = 0; // Public accessor for STATUS register
  virtual uint32_t readDelayRegister() = 0;  // Read back REG_DELAY to verify clamping
  virtual bool clearStatusRegister() = 0;    // Clear sticky STATUS flags

  // 2^16 shared BRAM depth, see HardwareLimits.h
  static constexpr uint32_t MAX_SAMPLES = static_cast<uint32_t>(HardwareLimits::MAX_SAMPLES);
  static constexpr uint16_t DEFAULT_DECIMATION = 64;

  // STATUS register bit masks (read-only, at register address 0x24)
  static constexpr uint32_t STATUS_BUSY_BIT = 0x01;              // bit[0]: FSM in RUNNING state
  static constexpr uint32_t STATUS_PS_ACCESS_DENIED_BIT = 0x02;  // bit[1]: PS tried BRAM access during busy
};
