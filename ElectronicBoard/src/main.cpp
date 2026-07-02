/*
 * main.cpp
 * Main application for LPC1114/LPC1125 Electronic Board
 *
 * Analog Signal Multiplexer with Variable Gain Amplification
 * - 4 analog inputs with variable gain (x1/4 to x8)
 * - 4 multiplexed outputs
 * - Addition output (OUT1 + OUT2)
 * - Subtraction output (OUT3 - OUT4)
 * - UART command interface for Red Pitaya control
 */

#include "chip.h"
#include "command_parser.h"
#include "mux_control.h"
#include "pin_init.h"
#include "uart.h"
#include <cr_section_macros.h>
#include <cstdint>

const uint32_t OscRateIn = 12000000; // 12 MHz
const uint32_t ExtRateIn = 0;        // No external clock input

/*****************************************************************************
 * Main application
 *****************************************************************************/
int main(void) {
  // Initialize system clock
  SystemCoreClockUpdate();

  // Initialize IOCON clock (needed for pin mux)
  Chip_Clock_EnablePeriphClock(SYSCON_CLOCK_IOCON);

  // Initialize GPIO
  Chip_GPIO_Init(LPC_GPIO);

  // Initialize all GPIO pins
  GPIO_InitAllPins();

  // Initialize UART for communication
  UART_Init();

  // Initialize multiplexer control
  MUX_Init();

  // Initialize command parser
  CMD_Init();

  // Send startup message
  UART_SendString("\r\n");
  UART_SendString("Baud: ");
  UART_SendInt(UART_BAUD_RATE);
  UART_SendString("\r\nClock: ");
  UART_SendUInt(SystemCoreClock);
  UART_SendString(" Hz\r\n");
  UART_SendString("\r\nType HELP for commands\r\n");
  UART_SendString("> ");

  // LED blink counter for heartbeat
  volatile uint32_t blinkCounter = 0;
  bool ledState = false;

  // Main loop - command processing
  while (1) {
    // Check for incoming UART data (non-blocking)
    char c;
    if (UART_ReadChar(&c)) {
      CMD_ProcessChar(c);

      // Execute command if ready
      if (CMD_IsReady()) {
        CMD_Execute();
        UART_SendString("> ");
      }
    }

    // Heartbeat LED: 50ms ON, 2s OFF
    blinkCounter++;
    if (ledState) {
      if (blinkCounter >= 10000) {
        blinkCounter = 0;
        ledState = false;
        Chip_GPIO_SetPinState(LPC_GPIO, 2, 8, true);
        Chip_GPIO_SetPinState(LPC_GPIO, 2, 9, true);
        Chip_GPIO_SetPinState(LPC_GPIO, 2, 10, true);
      }
    } else {
      if (blinkCounter >= 400000) {
        blinkCounter = 0;
        ledState = true;
        Chip_GPIO_SetPinState(LPC_GPIO, 2, 8, false);
        Chip_GPIO_SetPinState(LPC_GPIO, 2, 9, false);
        Chip_GPIO_SetPinState(LPC_GPIO, 2, 10, false);
      }
    }
  }

  return 0;
}
