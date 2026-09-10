#include "RedPitayaHardware.h"

#include "DacCodec.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <sys/mman.h>
#include <unistd.h>

RedPitayaHardware::RedPitayaHardware()
    : m_memFd(-1)
    , m_decimation(DEFAULT_DECIMATION)
    , m_regs(nullptr)
    , m_bram(nullptr)
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

  // Map the register page and the whole shared-BRAM window once. Every later
  // access is a plain memory read/write, not a mmap/munmap syscall pair.
  if (!_mapWindows())
  {
    cleanup();
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
  clearStatusRegister();
  resetMeasurement();

  if (!setDecimation(DEFAULT_DECIMATION))
  {
    std::cerr << "Warning: Failed to set default decimation" << std::endl;
  }

  return true;
}

void RedPitayaHardware::cleanup()
{
  if (_isMapped())
  {
    resetMeasurement();
  }

  _unmapWindows();

  if (m_memFd >= 0)
  {
    close(m_memFd);
    m_memFd = -1;
  }
}

uint32_t RedPitayaHardware::getVersion()
{
  return m_regs ? m_regs[REG_VERSION] : 0;
}

bool RedPitayaHardware::loadGenerationSignal(const std::vector<float>& signal)
{
  if (signal.size() > MAX_SAMPLES)
  {
    std::cerr << "Error: Signal too large (" << signal.size() << " > " << MAX_SAMPLES << ")"
              << std::endl;
    return false;
  }

  if (!_isMapped())
  {
    std::cerr << "Error: Hardware not initialized" << std::endl;
    return false;
  }

  uint32_t status = readStatusRegister();
  if (status & STATUS_BUSY_BIT)
  {
    std::cerr << "Warning: Measurement in progress (FSM is RUNNING)." << std::endl;
  }

  // Clear stale sticky flags so denials caused by this transfer are visible.
  clearStatusRegister();

  for (size_t i = 0; i < signal.size(); ++i)
  {
    int16_t dacValue = DacCodec::voltageToDAC(signal[i]);
    m_bram[i] = static_cast<uint32_t>(dacValue);
  }

  if (readStatusRegister() & STATUS_PS_ACCESS_DENIED_BIT)
  {
    std::cerr << "Error: BRAM write denied (measurement was running); signal is incomplete"
              << std::endl;
    return false;
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

  if (delaySamples > MAX_DELAY_SAMPLES)
  {
    std::cerr << "Error: delaySamples exceeds " << MAX_DELAY_SAMPLES << std::endl;
    return false;
  }

  if (!_isMapped())
  {
    std::cerr << "Error: Hardware not initialized" << std::endl;
    return false;
  }

  uint32_t status = readStatusRegister();
  if (status & STATUS_BUSY_BIT)
  {
    std::cerr << "Error: Measurement already in progress (FSM is RUNNING). "
              << "Wait for completion or reset." << std::endl;
    return false;
  }

  // Single-BRAM mode safety: enforce at least one sample of delay.
  uint32_t effectiveDelay = (delaySamples == 0) ? 1 : delaySamples;

  m_regs[REG_SIG_SIZE] = numSamples;
  m_regs[REG_DELAY] = effectiveDelay;
  m_regs[REG_START_MEASURE] = 1;

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

  if (!_isMapped())
  {
    std::cerr << "Error: Hardware not initialized" << std::endl;
    return false;
  }

  m_regs[REG_DECIMATION] = static_cast<uint32_t>(decimation);
  m_decimation = decimation;

  return true;
}

uint16_t RedPitayaHardware::getDecimation() const
{
  return m_decimation;
}

bool RedPitayaHardware::isMeasurementComplete()
{
  if (!_isMapped())
  {
    return false;
  }
  return m_regs[REG_END_MEASURE] == 1;
}

bool RedPitayaHardware::getAcquiredSignal(std::vector<float>& signal)
{
  if (!_isMapped())
  {
    std::cerr << "Error: Hardware not initialized" << std::endl;
    return false;
  }

  uint32_t status = readStatusRegister();
  if (status & STATUS_BUSY_BIT)
  {
    std::cerr << "Error: Measurement still in progress (FSM is RUNNING); data incomplete"
              << std::endl;
    return false;
  }

  // Clear stale sticky flags so denials caused by this transfer are visible.
  clearStatusRegister();

  for (size_t i = 0; i < signal.size(); ++i)
  {
    signal[i] = DacCodec::adcToVoltage(m_bram[i], m_decimation);
  }

  if (readStatusRegister() & STATUS_PS_ACCESS_DENIED_BIT)
  {
    std::cerr << "Error: BRAM read denied (measurement was running); data is invalid"
              << std::endl;
    return false;
  }

  // The RTL currently clears count_measure when acquisition stops, so a zero
  // count means "not available". Once the RTL latches the final count, this
  // becomes a hard check against truncated acquisitions.
  uint32_t acquiredCount = m_regs[REG_COUNT_MEASURE];
  if (acquiredCount != 0 && acquiredCount != signal.size())
  {
    std::cerr << "Error: Acquired sample count mismatch (expected " << signal.size()
              << ", got " << acquiredCount << ")" << std::endl;
    return false;
  }

  return true;
}

bool RedPitayaHardware::resetMeasurement()
{
  if (!_isMapped())
  {
    return false;
  }

  m_regs[REG_START_MEASURE] = 0; // Set to IDLE state

  return true;
}

uint32_t RedPitayaHardware::readStatusRegister()
{
  return m_regs ? m_regs[REG_STATUS] : 0;
}

uint32_t RedPitayaHardware::readDelayRegister()
{
  return m_regs ? m_regs[REG_DELAY] : 0;
}

bool RedPitayaHardware::clearStatusRegister()
{
  if (!_isMapped())
  {
    return false;
  }

  // STATUS bits are read-only from software perspective except sticky flag clear on write.
  m_regs[REG_STATUS] = 1u;

  return true;
}

bool RedPitayaHardware::_mapWindows()
{
  void* regs =
      mmap(nullptr, REG_WINDOW_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, m_memFd, REG_BASE);
  if (regs == MAP_FAILED)
  {
    std::cerr << "Error: mmap failed for registers: " << std::strerror(errno) << std::endl;
    return false;
  }
  m_regs = static_cast<volatile uint32_t*>(regs);

  void* bram = mmap(nullptr, BRAM_WINDOW_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, m_memFd,
                    BRAM_SHARED_BASE);
  if (bram == MAP_FAILED)
  {
    std::cerr << "Error: mmap failed for shared BRAM: " << std::strerror(errno) << std::endl;
    return false;
  }
  m_bram = static_cast<volatile uint32_t*>(bram);

  return true;
}

void RedPitayaHardware::_unmapWindows()
{
  if (m_regs != nullptr)
  {
    munmap(const_cast<uint32_t*>(m_regs), REG_WINDOW_SIZE);
    m_regs = nullptr;
  }
  if (m_bram != nullptr)
  {
    munmap(const_cast<uint32_t*>(m_bram), BRAM_WINDOW_SIZE);
    m_bram = nullptr;
  }
}

bool RedPitayaHardware::_isMapped() const
{
  return m_regs != nullptr && m_bram != nullptr;
}
