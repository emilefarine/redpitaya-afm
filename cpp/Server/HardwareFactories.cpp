/**
 * @file HardwareFactories.cpp
 * @brief Default hardware factories (on-device builds only)
 */

#include "HardwareFactories.h"

#include "../Hardware/ElectronicBoardUART.h"
#include "../Hardware/RedPitayaHardware.h"

namespace AFM
{

std::unique_ptr<IRedPitayaHardware> defaultHardwareFactory()
{
  return std::make_unique<RedPitayaHardware>();
}

std::unique_ptr<IElectronicBoard> defaultBoardFactory()
{
  return std::make_unique<ElectronicBoardUART>();
}

} // namespace AFM
