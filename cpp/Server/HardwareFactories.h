#pragma once

#include "../Hardware/IElectronicBoard.h"
#include "../Hardware/IRedPitayaHardware.h"

#include <functional>
#include <memory>

namespace AFM
{

/** @brief Creates a hardware abstraction instance (real implementation on device) */
using HardwareFactory = std::function<std::unique_ptr<IRedPitayaHardware>()>;

/** @brief Creates an electronic board instance (real implementation on device) */
using BoardFactory = std::function<std::unique_ptr<IElectronicBoard>()>;

/**
 * @brief Default factories constructing the real /dev/mem and UART classes.
 *
 * Defined in HardwareFactories.cpp which is only linked into on-device
 * binaries (afm_server). Host unit tests inject mocks instead, so this
 * translation unit is excluded from the host test build.
 */
std::unique_ptr<IRedPitayaHardware> defaultHardwareFactory();
std::unique_ptr<IElectronicBoard> defaultBoardFactory();

} // namespace AFM
