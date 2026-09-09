#pragma once

#include "IRedPitayaHardware.h"

#include <gmock/gmock.h>

class MockRedPitayaHardware : public IRedPitayaHardware
{
public:
  MOCK_METHOD(bool, initialize, (), (override));
  MOCK_METHOD(void, cleanup, (), (override));
  MOCK_METHOD(uint32_t, getVersion, (), (override));
  MOCK_METHOD(bool, loadGenerationSignal, (const std::vector<float>&), (override));
  MOCK_METHOD(bool, setDecimation, (uint16_t), (override));
  MOCK_METHOD(uint16_t, getDecimation, (), (const, override));
  MOCK_METHOD(bool, startMeasurement, (uint32_t, uint32_t), (override));
  MOCK_METHOD(bool, isMeasurementComplete, (), (override));
  MOCK_METHOD(bool, getAcquiredSignal, (std::vector<float>&), (override));
  MOCK_METHOD(bool, resetMeasurement, (), (override));
  MOCK_METHOD(uint32_t, readStatusRegister, (), (override));
  MOCK_METHOD(uint32_t, readDelayRegister, (), (override));
  MOCK_METHOD(bool, clearStatusRegister, (), (override));
};
