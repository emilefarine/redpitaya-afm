#pragma once

#include "IElectronicBoard.h"

#include <array>
#include <gmock/gmock.h>

class MockElectronicBoard : public IElectronicBoard
{
public:
  MOCK_METHOD(bool, initialize, (), (override));
  MOCK_METHOD(void, close, (), (override));
  MOCK_METHOD(bool, isConnected, (), (const, override));
  MOCK_METHOD(bool, setMuxRoute, (uint8_t, uint8_t), (override));
  MOCK_METHOD(bool, disconnectMux, (uint8_t), (override));
  MOCK_METHOD(bool, setGain, (uint8_t, GainSetting), (override));
  MOCK_METHOD(bool, getStatus, (std::string&), (override));
  MOCK_METHOD(bool, queryGains,
              ((std::array<GainSetting, 4>&), (std::array<bool, 4>&)), (override));
  MOCK_METHOD(bool, reset, (), (override));
  MOCK_METHOD(const std::string&, getLastError, (), (const, override));
};
