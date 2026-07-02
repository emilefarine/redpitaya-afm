#include "RedPitayaHardware.h"

#include <cmath>
#include <fcntl.h>
#include <iostream>
#include <sys/mman.h>
#include <unistd.h>

RedPitayaHardware::RedPitayaHardware()
    : m_memFd(-1)
    , m_decimation(DEFAULT_DECIMATION)
{
}

RedPitayaHardware::~RedPitayaHardware()
{
  cleanup();
}

bool RedPitayaHardware::initialize()
{
  // Open /dev/mem for memory-mapped I/O
  m_memFd = open("/dev/mem", O_RDWR | O_SYNC);
  if (m_memFd < 0)
  {
    std::cerr << "Error: Cannot open /dev/mem. Run as root!" << std::endl;
    return false;
  }

  if (getVersion() != 0x15062026) // Date of correct bitfile (VERSION in fpga register)
  {
    std::cerr << "Error: Incorrect FPGA bitfile loaded!" << std::endl;
    std::cerr << "       VERSION register = 0x" << std::hex << getVersion() << std::dec
              << std::endl;
    cleanup();
    return false;
  }

  std::cout << "Hardware initialized" << std::endl;

  // Reset to known state
  resetMeasurement();

  if (!setDecimation(DEFAULT_DECIMATION))
  {
    std::cerr << "Warning: Failed to set default decimation" << std::endl;
  }

  return true;
}

void RedPitayaHardware::cleanup()
{
  if (m_memFd >= 0)
  {
    resetMeasurement();
    close(m_memFd);
    m_memFd = -1;
  }
}

uint32_t RedPitayaHardware::getVersion()
{
  volatile uint32_t* versionReg = _mapRegister(REG_BASE + REG_VERSION * 4);
  if (!versionReg)
    return 0;

  uint32_t version = *versionReg;
  _unmapRegister(versionReg);

  return version;
}

bool RedPitayaHardware::loadGenerationSignal(const std::vector<float>& signal)
{
  if (signal.size() > MAX_SAMPLES)
  {
    std::cerr << "Error: Signal too large (" << signal.size() << " > " << MAX_SAMPLES << ")"
              << std::endl;
    return false;
  }

  uint32_t status = _readStatusRegister();
  if (status & STATUS_PS_ACCESS_DENIED_BIT)
  {
    std::cerr << "Warning: Previous BRAM access was denied (measurement may be running). "
              << "Waiting for measurement to complete..." << std::endl;
  }

  for (size_t i = 0; i < signal.size(); ++i)
  {
    volatile uint32_t* addr = _mapRegister(BRAM_SHARED_BASE + i * 4);
    if (!addr)
    {
      std::cerr << "Error: Failed to map BRAM_SHARED[" << i << "]" << std::endl;
      return false;
    }

    int16_t dacValue = _voltageToDAC(signal[i]);
    *addr = static_cast<uint32_t>(dacValue);

    _unmapRegister(addr);
  }

  return true;
}

bool RedPitayaHardware::startMeasurement(uint32_t numSamples, uint32_t delaySamples)
{
  if (numSamples > MAX_SAMPLES)
  {
    std::cerr << "Error: numSamples too large" << std::endl;
    return false;
  }

  uint32_t status = _readStatusRegister();
  if (status & STATUS_BUSY_BIT)
  {
    std::cerr << "Error: Measurement already in progress (FSM is RUNNING). "
              << "Wait for completion or reset." << std::endl;
    return false;
  }

  // Single-BRAM mode safety: enforce at least one sample of delay.
  uint32_t effectiveDelay = (delaySamples == 0) ? 1 : delaySamples;

  // Set signal size
  volatile uint32_t* sizeReg = _mapRegister(REG_BASE + REG_SIG_SIZE * 4);
  if (!sizeReg)
  {
    return false;
  }
  *sizeReg = numSamples;
  _unmapRegister(sizeReg);

  // Set delay between generation and acquisition
  volatile uint32_t* delayReg = _mapRegister(REG_BASE + REG_DELAY * 4);
  if (!delayReg)
  {
    return false;
  }
  *delayReg = effectiveDelay;
  _unmapRegister(delayReg);

  // Start measurement
  volatile uint32_t* startReg = _mapRegister(REG_BASE + REG_START_MEASURE * 4);
  if (!startReg)
  {
    return false;
  }
  *startReg = 1;
  _unmapRegister(startReg);

  return true;
}

bool RedPitayaHardware::setDecimation(uint16_t decimation)
{
  if (decimation < 16 || decimation > 1024)
  {
    std::cerr << "Error: Decimation must be between 16 and 1024" << std::endl;
    return false;
  }

  if ((decimation & (decimation - 1)) != 0)
  {
    std::cerr << "Error: Decimation must be a power of 2 (16, 32, 64, 128, 256, 512, 1024)"
              << std::endl;
    return false;
  }

  volatile uint32_t* decimationReg = _mapRegister(REG_BASE + REG_DECIMATION * 4);
  if (!decimationReg)
  {
    return false;
  }

  *decimationReg = static_cast<uint32_t>(decimation);
  _unmapRegister(decimationReg);

  m_decimation = decimation;

  return true;
}

uint16_t RedPitayaHardware::getDecimation() const
{
  return m_decimation;
}

bool RedPitayaHardware::isMeasurementComplete()
{
  volatile uint32_t* endReg = _mapRegister(REG_BASE + REG_END_MEASURE * 4);
  if (!endReg)
  {
    return false;
  }

  bool isComplete = (*endReg == 1);
  _unmapRegister(endReg);

  return isComplete;
}

bool RedPitayaHardware::getAcquiredSignal(std::vector<float>& signal)
{
  uint32_t status = _readStatusRegister();
  if (status & STATUS_BUSY_BIT)
  {
    std::cerr << "Warning: Measurement still in progress (FSM is RUNNING). "
              << "Data may be incomplete." << std::endl;
  }
  if (status & STATUS_PS_ACCESS_DENIED_BIT)
  {
    std::cerr << "Warning: Previous BRAM read was denied during measurement." << std::endl;
  }

  for (size_t i = 0; i < signal.size(); ++i)
  {
    volatile uint32_t* addr = _mapRegister(BRAM_SHARED_BASE + i * 4);
    if (!addr)
    {
      std::cerr << "Error: Failed to map BRAM_SHARED[" << i << "]" << std::endl;
      return false;
    }

    uint32_t rawValue = *addr;
    signal[i] = _adcToVoltage(rawValue, m_decimation);

    _unmapRegister(addr);
  }

  return true;
}

bool RedPitayaHardware::resetMeasurement()
{
  volatile uint32_t* startReg = _mapRegister(REG_BASE + REG_START_MEASURE * 4);
  if (!startReg)
  {
    return false;
  }

  *startReg = 0; // Set to IDLE state
  _unmapRegister(startReg);

  return true;
}

uint32_t RedPitayaHardware::readStatusRegister()
{
  return _readStatusRegister();
}

uint32_t RedPitayaHardware::readDelayRegister()
{
  volatile uint32_t* delayReg = _mapRegister(REG_BASE + REG_DELAY * 4);
  if (!delayReg)
    return 0;

  uint32_t delay = *delayReg;
  _unmapRegister(delayReg);

  return delay;
}

bool RedPitayaHardware::clearStatusRegister()
{
  volatile uint32_t* statusReg = _mapRegister(REG_BASE + REG_STATUS * 4);
  if (!statusReg)
    return false;

  // STATUS bits are read-only from software perspective except sticky flag clear on write.
  *statusReg = 1u;
  _unmapRegister(statusReg);

  return true;
}

uint32_t RedPitayaHardware::_readStatusRegister()
{
  volatile uint32_t* statusReg = _mapRegister(REG_BASE + REG_STATUS * 4);
  if (!statusReg)
  {
    return 0;
  }

  uint32_t status = *statusReg;
  _unmapRegister(statusReg);

  return status;
}

volatile uint32_t* RedPitayaHardware::_mapRegister(off_t targetAddr)
{
  off_t baseAddr = targetAddr & ~MAP_MASK;
  off_t offset = targetAddr & MAP_MASK;

  void* mapBase = mmap(NULL, MAP_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, m_memFd, baseAddr);

  if (mapBase == MAP_FAILED)
  {
    std::cerr << "Error: mmap failed for address 0x" << std::hex << targetAddr << std::dec
              << std::endl;
    return nullptr;
  }

  return static_cast<volatile uint32_t*>(mapBase) + (offset / sizeof(uint32_t));
}

void RedPitayaHardware::_unmapRegister(volatile uint32_t* addr)
{
  if (addr)
  {
    void* mapBase = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(addr) & ~MAP_MASK);
    munmap(mapBase, MAP_SIZE);
  }
}

int16_t RedPitayaHardware::_voltageToDAC(float voltage)
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

float RedPitayaHardware::_adcToVoltage(uint32_t rawValue, uint16_t decimation)
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
  uint8_t precisionMask = (1 << precisionBits) - 1; // 0x3 for 2 bits, 0x7 for 3 bits
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
  float precisionFraction = static_cast<float>(precision) / static_cast<float>(1 << precisionBits);

  // Precision bits are the lower bits of the value, so they are always added
  // regardless of the sign of the upper bits (2's complement)
  voltage += precisionFraction * lsbVoltage;

  return voltage;
}
