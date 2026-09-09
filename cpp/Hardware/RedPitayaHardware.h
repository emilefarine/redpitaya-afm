#pragma once

#include "IRedPitayaHardware.h"

#include <cstddef>
#include <cstdint>
#include <sys/types.h>
#include <vector>

/**
 * @brief Hardware implementation for RedPitaya FPGA-based signal generation
 *        and acquisition via /dev/mem
 *
 * Memory map (from measure_api.c):
 * - 0x40600000: BRAM_SHARED (generation + acquisition, in-place overwrite, 65536 samples max, 256KB window)
 * - 0x40640000: Control registers
 */

class RedPitayaHardware : public IRedPitayaHardware
{
public:
  RedPitayaHardware();
  ~RedPitayaHardware() override;

  bool initialize() override;
  void cleanup() override;

  uint32_t getVersion() override; // Returns 0x15062026 (date) if correct bitfile loaded

  // Signal generation (DAC)
  bool loadGenerationSignal(const std::vector<float>& signal) override;

  // Decimation control
  bool setDecimation(uint16_t decimation) override;
  uint16_t getDecimation() const override;

  // Signal acquisition (ADC)
  bool startMeasurement(uint32_t numSamples, uint32_t delaySamples = 0) override;
  bool isMeasurementComplete() override;
  bool getAcquiredSignal(std::vector<float>& signal) override;


  bool resetMeasurement() override;
  uint32_t readStatusRegister() override;   // Public accessor for STATUS register (test observability)
  uint32_t readDelayRegister() override;    // Read back REG_DELAY to verify clamping
  bool clearStatusRegister() override;      // Clear sticky STATUS flags (e.g., ps_access_denied)

private:
  // Memory mapping
  int m_memFd;
  uint16_t m_decimation; // Current decimation factor (16-1024)
  volatile uint32_t* _mapRegister(off_t targetAddr);
  void _unmapRegister(volatile uint32_t* addr);

  uint32_t _readStatusRegister();  // Read status (busy, denied) flags

  static constexpr off_t BRAM_SHARED_BASE = 0x40600000;
  static constexpr off_t REG_BASE = 0x40640000;

  static constexpr uint32_t REG_START_MEASURE = 2;
  static constexpr uint32_t REG_DELAY = 3;
  static constexpr uint32_t REG_SIG_SIZE = 4;
  static constexpr uint32_t REG_END_MEASURE = 5;

  static constexpr uint32_t REG_VERSION = 7; // Read-only version register (date of bitfile)
  static constexpr uint32_t REG_DECIMATION = 8;
  static constexpr uint32_t REG_STATUS = 9;   // Read-only status register (busy, ps_access_denied)

  static constexpr std::size_t MAP_SIZE = 4096UL;
  static constexpr std::size_t MAP_MASK = MAP_SIZE - 1;
};
