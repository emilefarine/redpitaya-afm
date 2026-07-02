/*
 * command_parser.cpp
 * UART Command Parser implementation
 *
 *  Created on: 26 Nov 2025
 */

#include "command_parser.h"
#include "mux_control.h"
#include "uart.h"
#include <cctype>
#include <cstring>

/*****************************************************************************
 * Internal State
 *****************************************************************************/

static char cmdBuffer[CMD_MAX_LENGTH];
static uint8_t cmdIndex = 0;
static bool cmdReady = false;

/*****************************************************************************
 * Response Helpers
 *****************************************************************************/

static void SendOK(void) { UART_SendString("OK\r\n"); }

static void SendError(const char *msg) {
  UART_SendString("ERR:");
  UART_SendString(msg);
  UART_SendString("\r\n");
}

/*****************************************************************************
 * Command Handlers
 *****************************************************************************/

/**
 * @brief Parse and execute MUX command
 * Format: MUX<out>,<in> where out=1-4, in=1-4 or X
 */
static bool HandleMuxCommand(const char *args) {
  // Expect: "<out>,<in>"
  if (strlen(args) < 3) {
    SendError("Invalid MUX format");
    return false;
  }

  // Parse output (1-4, converted to 0-3 internally)
  char outChar = args[0];
  if (outChar < '1' || outChar > '4') {
    SendError("Output must be 1-4");
    return false;
  }
  uint8_t output = outChar - '1';

  // Check comma
  if (args[1] != ',') {
    SendError("Expected comma");
    return false;
  }

  // Parse input (1-4, converted to 0-3 internally, or X for disconnect)
  char inChar = toupper(args[2]);
  uint8_t input;

  if (inChar == 'X') {
    input = MUX_DISCONNECTED;
  } else if (inChar >= '1' && inChar <= '4') {
    input = inChar - '1';
  } else {
    SendError("Input must be 1-4 or X");
    return false;
  }

  // Execute
  if (MUX_SetRoute(output, input)) {
    SendOK();
    return true;
  } else {
    SendError("Input already in use");
    return false;
  }
}

/**
 * @brief Parse and execute GAIN command
 * Format: GAIN<ch>,<val> where ch=1-4, val=0-7
 */
static bool HandleGainCommand(const char *args) {
  // Expect: "<ch>,<val>"
  if (strlen(args) < 3) {
    SendError("Invalid GAIN format");
    return false;
  }

  // Parse channel (1-4, converted to 0-3 internally)
  char chChar = args[0];
  if (chChar < '1' || chChar > '4') {
    SendError("Channel must be 1-4");
    return false;
  }
  uint8_t channel = chChar - '1';

  // Check comma
  if (args[1] != ',') {
    SendError("Expected comma");
    return false;
  }

  // Parse gain value
  char gainChar = args[2];
  if (gainChar < '0' || gainChar > '7') {
    SendError("Gain must be 0-7");
    return false;
  }
  GainSetting_t gain = (GainSetting_t)(gainChar - '0');

  // Execute
  if (AMP_SetGain(channel, gain)) {
    SendOK();
    return true;
  } else {
    SendError("Failed to set gain");
    return false;
  }
}

/**
 * @brief Handle STATUS command
 */
static void HandleStatusCommand(void) {
  MUX_PrintStatus();
  SendOK();
}

/**
 * @brief Handle RESET command
 */
static void HandleResetCommand(void) {
  MUX_Init();
  UART_SendString("Reset complete\r\n");
  SendOK();
}

/*****************************************************************************
 * Public Functions
 *****************************************************************************/

void CMD_Init(void) {
  cmdIndex = 0;
  cmdReady = false;
  memset(cmdBuffer, 0, CMD_MAX_LENGTH);
}

void CMD_ProcessChar(char c) {
  // Handle line endings
  if (c == '\r' || c == '\n') {
    if (cmdIndex > 0) {
      cmdBuffer[cmdIndex] = '\0';
      cmdReady = true;
    }
    return;
  }

  // Handle backspace
  if (c == '\b' || c == 0x7F) {
    if (cmdIndex > 0) {
      cmdIndex--;
      // Echo backspace
      UART_SendChar('\b');
      UART_SendChar(' ');
      UART_SendChar('\b');
    }
    return;
  }

  // Add character to buffer
  if (cmdIndex < CMD_MAX_LENGTH - 1) {
    cmdBuffer[cmdIndex++] = c;
    // Echo character
    UART_SendChar(c);
  }
}

bool CMD_IsReady(void) { return cmdReady; }

void CMD_Execute(void) {
  if (!cmdReady) {
    return;
  }

  // Echo newline
  UART_SendNewLine();

  // Convert to uppercase for comparison
  char upperCmd[CMD_MAX_LENGTH];
  for (int i = 0; i < CMD_MAX_LENGTH && cmdBuffer[i]; i++) {
    upperCmd[i] = toupper(cmdBuffer[i]);
  }
  upperCmd[cmdIndex] = '\0';

  // Parse command
  if (strncmp(upperCmd, "MUX", 3) == 0) {
    HandleMuxCommand(cmdBuffer + 3);
  } else if (strncmp(upperCmd, "GAIN", 4) == 0) {
    HandleGainCommand(cmdBuffer + 4);
  } else if (strcmp(upperCmd, "STATUS") == 0) {
    HandleStatusCommand();
  } else if (strcmp(upperCmd, "RESET") == 0) {
    HandleResetCommand();
  } else if (strcmp(upperCmd, "HELP") == 0 || strcmp(upperCmd, "?") == 0) {
    CMD_PrintHelp();
  } else if (cmdIndex > 0) {
    SendError("Unknown command. Type HELP for help.");
  }

  // Reset for next command
  CMD_Init();
}

void CMD_PrintHelp(void) {
  UART_SendString("\r\n");
  UART_SendString("=== Electronic Board Commands ===\r\n");
  UART_SendString("\r\n");
  UART_SendString("MUX<out>,<in>  - Route input to output\r\n");
  UART_SendString("                 out: 1-4, in: 1-4 or X (disconnect)\r\n");
  UART_SendString("                 Example: MUX1,2 or MUX2,X\r\n");
  UART_SendString("\r\n");
  UART_SendString("GAIN<ch>,<val> - Set input channel gain\r\n");
  UART_SendString("                 ch: 1-4, val: 0-7\r\n");
  UART_SendString(
      "                 0=x1/8, 1=x1/4, 2=x1/2, 3=x1, 4=x2, 5=x4, 6=x8, 7=x16\r\n");
  UART_SendString("                 Example: GAIN1,3 (ch1 gain x1)\r\n");
  UART_SendString("\r\n");
  UART_SendString("STATUS         - Show current configuration\r\n");
  UART_SendString("RESET          - Reset all settings\r\n");
  UART_SendString("HELP           - Show this help\r\n");
  UART_SendString("\r\n");
  UART_SendString("Note: Each input can only be routed to ONE output.\r\n");
  UART_SendString("=================================\r\n");
}
