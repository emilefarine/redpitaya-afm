#pragma once

#include <cstdint>
#include <string>
#include <vector>

/**
 * @brief Hardware interface for RedPitaya FPGA-based signal generation and acquisition
 *
 * Memory map (from measure_api.c):
 * - 0x40600000: BRAM_SHARED (generation + acquisition, in-place overwrite, 65536 samples max, 256KB window)
 * - 0x40640000: Control registers
 */

class RedPitayaHardware
{
public:
  RedPitayaHardware();
  ~RedPitayaHardware();

  bool initialize();
  void cleanup();

  uint32_t getVersion(); // Returns 0x15062026 (date) if correct bitfile loaded

  // Signal generation (DAC)
  bool loadGenerationSignal(const std::vector<float>& signal);

  // Decimation control
  bool setDecimation(uint16_t decimation);
  uint16_t getDecimation() const;

  // Signal acquisition (ADC)
  bool startMeasurement(uint32_t numSamples, uint32_t delaySamples = 0);
  bool isMeasurementComplete();
  bool getAcquiredSignal(std::vector<float>& signal);


  bool resetMeasurement();
  uint32_t readStatusRegister();   // Public accessor for STATUS register (test observability)
  uint32_t readDelayRegister();    // Read back REG_DELAY to verify clamping
  bool clearStatusRegister();      // Clear sticky STATUS flags (e.g., ps_access_denied)

  static constexpr uint32_t MAX_SAMPLES = 65536;  // 2^16 shared BRAM depth
  static constexpr int32_t MAX_DAC_VALUE = 8191;  // 14-bit signed max
  static constexpr int32_t MIN_DAC_VALUE = -8192; // 14-bit signed min
  static constexpr uint16_t DEFAULT_DECIMATION = 64;

  // STATUS register bit masks (read-only, at register address 0x24)
  static constexpr uint32_t STATUS_BUSY_BIT = 0x01;              // bit[0]: FSM in RUNNING state
  static constexpr uint32_t STATUS_PS_ACCESS_DENIED_BIT = 0x02;  // bit[1]: PS tried BRAM access during busy

private:
  // Memory mapping
  int m_memFd;
  uint16_t m_decimation; // Current decimation factor (16-1024)
  volatile uint32_t* _mapRegister(off_t targetAddr);
  void _unmapRegister(volatile uint32_t* addr);

  int16_t _voltageToDAC(float voltage);
  float _adcToVoltage(uint32_t rawValue, uint16_t decimation);
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

  static constexpr size_t MAP_SIZE = 4096UL;
  static constexpr size_t MAP_MASK = MAP_SIZE - 1;
};
