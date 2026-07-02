/*
 * pin_init.cpp
 * GPIO pin initialization implementation
 *
 *  Created on: 26 Nov 2025
 */

#include "pin_init.h"

void GPIO_InitAllPins(void) {
  GPIO_InitStatusLed();
  GPIO_InitMultiplexers();
  GPIO_InitAmplifiers();
}

void GPIO_InitStatusLed(void) {
  Chip_GPIO_SetPinDIROutput(LPC_GPIO, 2, 8);
  Chip_GPIO_SetPinDIROutput(LPC_GPIO, 2, 9);
  Chip_GPIO_SetPinDIROutput(LPC_GPIO, 2, 10);
}

void GPIO_InitMultiplexers(void) {
  // Configure IOCON for pins marked with "R" in LPC11xx datasheet
  // These pins default to analog mode and require IOCON_ADMODE_EN for digital
  // GPIO
  // PIO0_11 and PIO1_0 are ADC pins - need ADMODE_EN and FUNC0/FUNC1 for GPIO
  Chip_IOCON_PinMuxSet(LPC_IOCON, IOCON_PIO0_11,
                       IOCON_FUNC1 | IOCON_MODE_INACT | IOCON_ADMODE_EN);
  // PIO1_0 is TDO/PIO1_0/AD1/CT32B1_CAP0 - FUNC1 = PIO1_0 (GPIO)
  Chip_IOCON_PinMuxSet(LPC_IOCON, IOCON_PIO1_0,
                       IOCON_FUNC1 | IOCON_MODE_INACT | IOCON_ADMODE_EN);

  // Configure IOCON for SPI-related pins that default to SPI function
  // PIO0_6 (SCK), PIO0_7 (CTS), PIO0_8 (MISO), PIO0_9 (MOSI) need to be set to
  // GPIO function
  Chip_IOCON_PinMuxSet(LPC_IOCON, IOCON_PIO0_6,
                       IOCON_FUNC0 |
                           IOCON_MODE_INACT); // MUX1 A1 - GPIO function
  Chip_IOCON_PinMuxSet(LPC_IOCON, IOCON_PIO0_7,
                       IOCON_FUNC0 |
                           IOCON_MODE_INACT); // MUX3 A0 - GPIO function
  Chip_IOCON_PinMuxSet(LPC_IOCON, IOCON_PIO0_8,
                       IOCON_FUNC0 |
                           IOCON_MODE_INACT); // MUX3 A1 - GPIO function
  Chip_IOCON_PinMuxSet(LPC_IOCON, IOCON_PIO0_9,
                       IOCON_FUNC0 |
                           IOCON_MODE_INACT); // MUX2 A0 - GPIO function

  // Multiplexer 1 (Output 0)
  Chip_GPIO_SetPinDIROutput(LPC_GPIO, 0, 3); // A0
  Chip_GPIO_SetPinDIROutput(LPC_GPIO, 0, 6); // A1
  Chip_GPIO_SetPinDIROutput(LPC_GPIO, 3, 1); // EN

  // Multiplexer 2 (Output 1)
  Chip_GPIO_SetPinDIROutput(LPC_GPIO, 0, 7); // A0
  Chip_GPIO_SetPinDIROutput(LPC_GPIO, 0, 8); // A1
  Chip_GPIO_SetPinDIROutput(LPC_GPIO, 3, 2); // EN

  // Multiplexer 3 (Output 2)
  Chip_GPIO_SetPinDIROutput(LPC_GPIO, 0, 9); // A0
  Chip_GPIO_SetPinDIROutput(LPC_GPIO, 2, 3); // A1
  Chip_GPIO_SetPinDIROutput(LPC_GPIO, 3, 3); // EN

  // Multiplexer 4 (Output 3)
  Chip_GPIO_SetPinDIROutput(LPC_GPIO, 0, 11); // A0
  Chip_GPIO_SetPinDIROutput(LPC_GPIO, 1, 0);  // A1
  Chip_GPIO_SetPinDIROutput(LPC_GPIO, 3, 4);  // EN
}

void GPIO_InitAmplifiers(void) {
  // Configure IOCON for pins marked with "R" in LPC11xx datasheet
  // These pins default to analog mode and require IOCON_ADMODE_EN for digital
  // GPIO
  Chip_IOCON_PinMuxSet(LPC_IOCON, IOCON_PIO1_1,
                       IOCON_FUNC1 | IOCON_MODE_INACT | IOCON_ADMODE_EN);
  Chip_IOCON_PinMuxSet(LPC_IOCON, IOCON_PIO1_2,
                       IOCON_FUNC1 | IOCON_MODE_INACT | IOCON_ADMODE_EN);

  // Configure standard PIO1 pins (not marked with "R")
  // These pins default to digital mode, no IOCON_ADMODE_EN needed
  Chip_IOCON_PinMuxSet(LPC_IOCON, IOCON_PIO1_4, IOCON_FUNC0 | IOCON_MODE_INACT);
  Chip_IOCON_PinMuxSet(LPC_IOCON, IOCON_PIO1_5, IOCON_FUNC0 | IOCON_MODE_INACT);
  Chip_IOCON_PinMuxSet(LPC_IOCON, IOCON_PIO1_8, IOCON_FUNC0 | IOCON_MODE_INACT);
  Chip_IOCON_PinMuxSet(LPC_IOCON, IOCON_PIO1_9, IOCON_FUNC0 | IOCON_MODE_INACT);
  Chip_IOCON_PinMuxSet(LPC_IOCON, IOCON_PIO1_10,
                       IOCON_FUNC0 | IOCON_MODE_INACT);
  Chip_IOCON_PinMuxSet(LPC_IOCON, IOCON_PIO1_11,
                       IOCON_FUNC0 | IOCON_MODE_INACT);

  // Amplifier 1 (IN0)
  Chip_GPIO_SetPinDIROutput(LPC_GPIO, 1, 1);  // A0
  Chip_GPIO_SetPinDIROutput(LPC_GPIO, 1, 2);  // A1
  Chip_GPIO_SetPinDIROutput(LPC_GPIO, 2, 11); // A2
  // Set initial state to LOW
  Chip_GPIO_SetPinState(LPC_GPIO, 1, 1, true);
  Chip_GPIO_SetPinState(LPC_GPIO, 1, 2, true);
  Chip_GPIO_SetPinState(LPC_GPIO, 2, 11, false);

  // Amplifier 2 (IN1)
  Chip_GPIO_SetPinDIROutput(LPC_GPIO, 1, 4); // A0
  Chip_GPIO_SetPinDIROutput(LPC_GPIO, 1, 5); // A1
  Chip_GPIO_SetPinDIROutput(LPC_GPIO, 2, 1); // A2
  Chip_GPIO_SetPinState(LPC_GPIO, 1, 4, true);
  Chip_GPIO_SetPinState(LPC_GPIO, 1, 5, true);
  Chip_GPIO_SetPinState(LPC_GPIO, 2, 1, false);

  // Amplifier 3 (IN2)
  Chip_GPIO_SetPinDIROutput(LPC_GPIO, 2, 2); // A0
  Chip_GPIO_SetPinDIROutput(LPC_GPIO, 1, 8); // A1
  Chip_GPIO_SetPinDIROutput(LPC_GPIO, 1, 9); // A2
  Chip_GPIO_SetPinState(LPC_GPIO, 2, 2, true);
  Chip_GPIO_SetPinState(LPC_GPIO, 1, 8, true);
  Chip_GPIO_SetPinState(LPC_GPIO, 1, 9, false);

  // Amplifier 4 (IN3)
  Chip_GPIO_SetPinDIROutput(LPC_GPIO, 1, 10); // A0
  Chip_GPIO_SetPinDIROutput(LPC_GPIO, 1, 11); // A1
  Chip_GPIO_SetPinDIROutput(LPC_GPIO, 2, 4);  // A2
  Chip_GPIO_SetPinState(LPC_GPIO, 1, 10, true);
  Chip_GPIO_SetPinState(LPC_GPIO, 1, 11, true);
  Chip_GPIO_SetPinState(LPC_GPIO, 2, 4, false);
}
