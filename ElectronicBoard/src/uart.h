/*
 * uart.h
 * UART driver for LPC1114/LPC1125
 *
 *  Created on: 26 Nov 2025
 */

#ifndef UART_H_
#define UART_H_

#include "chip.h"
#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

// UART Configuration
#define UART_BAUD_RATE 9600

/**
 * @brief Initialize UART0 for debug output
 * Configures pins PIO1_6 (RXD) and PIO1_7 (TXD)
 */
void UART_Init(void);

/**
 * @brief Send a null-terminated string via UART (blocking)
 * @param str String to send
 */
void UART_SendString(const char *str);

/**
 * @brief Send a single character via UART
 * @param c Character to send
 */
void UART_SendChar(char c);

/**
 * @brief Send an integer as decimal string via UART
 * @param num Integer to send
 */
void UART_SendInt(int num);

/**
 * @brief Send an unsigned integer as decimal string via UART
 * @param num Unsigned integer to send
 */
void UART_SendUInt(uint32_t num);

/**
 * @brief Send a value as hexadecimal string via UART (0x prefixed)
 * @param value Value to send
 */
void UART_SendHex(uint32_t value);

/**
 * @brief Send a newline (CR+LF) via UART
 */
void UART_SendNewLine(void);

/**
 * @brief Check if data is available to read
 * @return true if at least one byte is available
 */
bool UART_DataAvailable(void);

/**
 * @brief Read a single character from UART (non-blocking)
 * @param c Pointer to store received character
 * @return true if character was read, false if no data available
 */
bool UART_ReadChar(char *c);

#ifdef __cplusplus
}
#endif

#endif /* UART_H_ */
