/*
 * uart.cpp
 * UART driver implementation for LPC1114/LPC1125
 *
 *  Created on: 26 Nov 2025
 */

#include "uart.h"
#include <cstring>

/**
 * @brief Initialize UART pin mux
 * Configure PIO1_6 and PIO1_7 for UART function
 */
static void Init_UART_PinMux(void) {
  Chip_IOCON_PinMuxSet(
      LPC_IOCON, IOCON_PIO1_6,
      (IOCON_FUNC1 | IOCON_MODE_INACT | IOCON_DIGMODE_EN)); /* RXD */
  Chip_IOCON_PinMuxSet(
      LPC_IOCON, IOCON_PIO1_7,
      (IOCON_FUNC1 | IOCON_MODE_INACT | IOCON_DIGMODE_EN)); /* TXD */
}

void UART_Init(void) {
  // IMPORTANT: Enable UART clock before any UART register access
  Chip_Clock_EnablePeriphClock(SYSCON_CLOCK_UART0);
  Chip_Clock_SetUARTClockDiv(1); // Divide by 1 (not 0 which disables)

  // Configure pin mux for UART function
  Init_UART_PinMux();

  // Initialize UART0 peripheral
  Chip_UART_Init(LPC_UART0);
  Chip_UART_SetBaud(LPC_UART0, UART_BAUD_RATE);

  // Configure: 8 data bits, 1 stop bit, no parity
  Chip_UART_ConfigData(LPC_UART0, UART_LCR_WLEN8 | UART_LCR_SBS_1BIT |
                                      UART_LCR_PARITY_DIS);

  // Enable and reset FIFOs
  Chip_UART_SetupFIFOS(LPC_UART0,
                       UART_FCR_FIFO_EN | UART_FCR_RX_RS | UART_FCR_TX_RS);

  // Enable transmitter
  Chip_UART_TXEnable(LPC_UART0);
}

void UART_SendString(const char *str) {
  while (*str) {
    UART_SendChar(*str++);
  }
}

void UART_SendChar(char c) {
  // Wait until TX holding register is empty with timeout
  volatile uint32_t timeout = 100000;
  while (!(Chip_UART_ReadLineStatus(LPC_UART0) & UART_LSR_THRE)) {
    if (--timeout == 0) {
      return; // Timeout - don't hang forever
    }
  }
  Chip_UART_SendByte(LPC_UART0, c);
}

void UART_SendInt(int num) {
  char buffer[12]; // Enough for 32-bit int + sign + null
  int i = 0;
  bool negative = false;

  // Handle negative numbers by converting to unsigned
  // This avoids overflow for INT_MIN (-2147483648)
  uint32_t unum;
  if (num < 0) {
    negative = true;
    unum = (uint32_t)(-(num + 1)) + 1; // Safe conversion for INT_MIN
  } else {
    unum = (uint32_t)num;
  }

  // Handle zero case
  if (unum == 0) {
    UART_SendChar('0');
    return;
  }

  // Convert to string (reversed)
  while (unum > 0) {
    buffer[i++] = '0' + (unum % 10);
    unum /= 10;
  }

  if (negative) {
    buffer[i++] = '-';
  }

  // Send in reverse order
  while (i > 0) {
    UART_SendChar(buffer[--i]);
  }
}

void UART_SendUInt(uint32_t num) {
  char buffer[12];
  int i = 0;

  // Handle zero case
  if (num == 0) {
    UART_SendChar('0');
    return;
  }

  // Convert to string (reversed)
  while (num > 0) {
    buffer[i++] = '0' + (num % 10);
    num /= 10;
  }

  // Send in reverse order
  while (i > 0) {
    UART_SendChar(buffer[--i]);
  }
}

void UART_SendHex(uint32_t value) {
  const char hexChars[] = "0123456789ABCDEF";
  UART_SendString("0x");
  for (int i = 7; i >= 0; i--) {
    UART_SendChar(hexChars[(value >> (i * 4)) & 0xF]);
  }
}

void UART_SendNewLine(void) { UART_SendString("\r\n"); }

bool UART_DataAvailable(void) {
  return (Chip_UART_ReadLineStatus(LPC_UART0) & UART_LSR_RDR) != 0;
}

bool UART_ReadChar(char *c) {
  if (UART_DataAvailable()) {
    *c = (char)Chip_UART_ReadByte(LPC_UART0);
    return true;
  }
  return false;
}
