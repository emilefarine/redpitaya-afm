/*
 * command_parser.h
 * UART Command Parser for Electronic Board
 *
 * Protocol:
 *   Commands are ASCII text terminated by '\n' (LF) or '\r' (CR)
 *   Responses: "OK\r\n" on success, "ERR:<message>\r\n" on error
 *
 * Commands:
 *   MUX<out>,<in>   - Route input to output (out=1-4, in=1-4 or X)
 *   GAIN<ch>,<val>  - Set gain for channel (ch=1-4, val=0-7)
 *   STATUS          - Print current configuration
 *   RESET           - Reset to defaults (all disconnected, gain=1)
 *   HELP            - Print command help
 *
 * Examples:
 *   MUX1,2    -> Route input 2 to output 1
 *   MUX2,X    -> Disconnect output 2
 *   GAIN1,5   -> Set input 1 gain to x4
 *   STATUS    -> Print all settings
 *
 *  Created on: 26 Nov 2025
 */

#ifndef COMMAND_PARSER_H_
#define COMMAND_PARSER_H_

#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

// Maximum command length
#define CMD_MAX_LENGTH 32

/**
 * @brief Initialize command parser
 */
void CMD_Init(void);

/**
 * @brief Process a received character
 * Call this for each character received from UART
 * @param c Received character
 */
void CMD_ProcessChar(char c);

/**
 * @brief Check if a complete command is ready
 * @return true if a command is ready to be executed
 */
bool CMD_IsReady(void);

/**
 * @brief Execute the pending command
 * Call this when CMD_IsReady() returns true
 */
void CMD_Execute(void);

/**
 * @brief Print help message
 */
void CMD_PrintHelp(void);

#ifdef __cplusplus
}
#endif

#endif /* COMMAND_PARSER_H_ */
