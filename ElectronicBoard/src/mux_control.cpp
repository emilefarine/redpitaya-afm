/*
 * mux_control.cpp
 * Multiplexer and Amplifier control implementation
 *
 * Hardware Configuration:
 * - ADG1409YRUZ: 4:1 analog multiplexer, controlled by A0, A1, EN pins
 * - PGA855: Variable gain amplifier, controlled by A0, A1, A2 pins
 *
 *  Created on: 26 Nov 2025
 */

#include "mux_control.h"
#include "uart.h"

// Pin Definitions - Multiplexers (ADG1409)

// Multiplexer 1 - Output 1 (0-indexed internally: 0)
#define MUX1_A0_PORT 0
#define MUX1_A0_PIN 3
#define MUX1_A1_PORT 0
#define MUX1_A1_PIN 6
#define MUX1_EN_PORT 3
#define MUX1_EN_PIN 1

// Multiplexer 2 - Output 2 (0-indexed internally: 1)
#define MUX2_A0_PORT 0
#define MUX2_A0_PIN 7
#define MUX2_A1_PORT 0
#define MUX2_A1_PIN 8
#define MUX2_EN_PORT 3
#define MUX2_EN_PIN 2

// Multiplexer 3 - Output 3 (0-indexed internally: 2)
#define MUX3_A0_PORT 0
#define MUX3_A0_PIN 9
#define MUX3_A1_PORT 2
#define MUX3_A1_PIN 3
#define MUX3_EN_PORT 3
#define MUX3_EN_PIN 3

// Multiplexer 4 - Output 4 (0-indexed internally: 3)
#define MUX4_A0_PORT 0
#define MUX4_A0_PIN 11
#define MUX4_A1_PORT 1
#define MUX4_A1_PIN 0
#define MUX4_EN_PORT 3
#define MUX4_EN_PIN 4

// Pin Definitions - Variable Gain Amplifiers (PGA849)

// Amplifier 1 - Input 1 (0-indexed internally: 0)
#define AMP1_A0_PORT 1
#define AMP1_A0_PIN 1
#define AMP1_A1_PORT 1
#define AMP1_A1_PIN 2
#define AMP1_A2_PORT 2
#define AMP1_A2_PIN 11

// Amplifier 2 - Input 2 (0-indexed internally: 1)
#define AMP2_A0_PORT 1
#define AMP2_A0_PIN 4
#define AMP2_A1_PORT 1
#define AMP2_A1_PIN 5
#define AMP2_A2_PORT 2
#define AMP2_A2_PIN 1

// Amplifier 3 - Input 3 (0-indexed internally: 2)
#define AMP3_A0_PORT 2
#define AMP3_A0_PIN 2
#define AMP3_A1_PORT 1
#define AMP3_A1_PIN 8
#define AMP3_A2_PORT 1
#define AMP3_A2_PIN 9

// Amplifier 4 - Input 4 (0-indexed internally: 3)
#define AMP4_A0_PORT 1
#define AMP4_A0_PIN 10
#define AMP4_A1_PORT 1
#define AMP4_A1_PIN 11
#define AMP4_A2_PORT 2
#define AMP4_A2_PIN 4

/*****************************************************************************
 * Internal State
 *****************************************************************************/

// Current routing: muxRouting[output] = input (or MUX_DISCONNECTED)
static uint8_t muxRouting[MUX_NUM_CHANNELS];

// Current gain settings for each input channel
static GainSetting_t ampGains[MUX_NUM_CHANNELS];

// Gain strings for display
static const char *gainStrings[GAIN_COUNT] = {"1/8", "1/4", "1/2", "1",
                                              "2",   "4",   "8",   "16"};

/*****************************************************************************
 * Pin Mapping Tables
 *****************************************************************************/

typedef struct {
  uint8_t a0Port, a0Pin;
  uint8_t a1Port, a1Pin;
  uint8_t enPort, enPin;
} MuxPins_t;

typedef struct {
  uint8_t a0Port, a0Pin;
  uint8_t a1Port, a1Pin;
  uint8_t a2Port, a2Pin;
} AmpPins_t;

static const MuxPins_t muxPins[MUX_NUM_CHANNELS] = {
    {MUX1_A0_PORT, MUX1_A0_PIN, MUX1_A1_PORT, MUX1_A1_PIN, MUX1_EN_PORT,
     MUX1_EN_PIN},
    {MUX2_A0_PORT, MUX2_A0_PIN, MUX2_A1_PORT, MUX2_A1_PIN, MUX2_EN_PORT,
     MUX2_EN_PIN},
    {MUX3_A0_PORT, MUX3_A0_PIN, MUX3_A1_PORT, MUX3_A1_PIN, MUX3_EN_PORT,
     MUX3_EN_PIN},
    {MUX4_A0_PORT, MUX4_A0_PIN, MUX4_A1_PORT, MUX4_A1_PIN, MUX4_EN_PORT,
     MUX4_EN_PIN},
};

static const AmpPins_t ampPins[MUX_NUM_CHANNELS] = {
    {AMP1_A0_PORT, AMP1_A0_PIN, AMP1_A1_PORT, AMP1_A1_PIN, AMP1_A2_PORT,
     AMP1_A2_PIN},
    {AMP2_A0_PORT, AMP2_A0_PIN, AMP2_A1_PORT, AMP2_A1_PIN, AMP2_A2_PORT,
     AMP2_A2_PIN},
    {AMP3_A0_PORT, AMP3_A0_PIN, AMP3_A1_PORT, AMP3_A1_PIN, AMP3_A2_PORT,
     AMP3_A2_PIN},
    {AMP4_A0_PORT, AMP4_A0_PIN, AMP4_A1_PORT, AMP4_A1_PIN, AMP4_A2_PORT,
     AMP4_A2_PIN},
};

/**
 * @brief Set GPIO pin state
 */
static inline void SetPin(uint8_t port, uint8_t pin, bool state) {
  Chip_GPIO_SetPinState(LPC_GPIO, port, pin, state);
}

/**
 * @brief Apply multiplexer hardware settings
 * @param muxIndex Multiplexer index (0-3)
 */
static void MUX_ApplyHardware(uint8_t muxIndex) {
  const MuxPins_t *pins = &muxPins[muxIndex];
  uint8_t input = muxRouting[muxIndex];

  if (input == MUX_DISCONNECTED || input >= MUX_NUM_INPUTS) {
    // Disable multiplexer (EN = LOW)
    SetPin(pins->enPort, pins->enPin, false);
    SetPin(pins->a0Port, pins->a0Pin, false);
    SetPin(pins->a1Port, pins->a1Pin, false);
  } else {
    // Set input selection (A0, A1)
    SetPin(pins->a0Port, pins->a0Pin, (input & 0x01) != 0);
    SetPin(pins->a1Port, pins->a1Pin, (input & 0x02) != 0);
    // Enable multiplexer (EN = HIGH)
    SetPin(pins->enPort, pins->enPin, true);
  }
}

/**
 * @brief Apply amplifier hardware settings
 * @param ampIndex Amplifier index (0-3)
 */
static void AMP_ApplyHardware(uint8_t ampIndex) {
  const AmpPins_t *pins = &ampPins[ampIndex];
  uint8_t gain = (uint8_t)ampGains[ampIndex];

  // Set gain selection (A0, A1, A2)
  SetPin(pins->a0Port, pins->a0Pin, (gain & 0x01) != 0);
  SetPin(pins->a1Port, pins->a1Pin, (gain & 0x02) != 0);
  SetPin(pins->a2Port, pins->a2Pin, (gain & 0x04) != 0);
}

/*****************************************************************************
 * Public Functions
 *****************************************************************************/

void MUX_Init(void) {
  // Initialize all outputs as disconnected
  for (uint8_t i = 0; i < MUX_NUM_CHANNELS; i++) {
    muxRouting[i] = MUX_DISCONNECTED;
    MUX_ApplyHardware(i);
  }

  // Initialize all gains to 1 (unity gain)
  for (uint8_t i = 0; i < MUX_NUM_CHANNELS; i++) {
    ampGains[i] = GAIN_1;
    AMP_ApplyHardware(i);
  }
}

bool MUX_SetRoute(uint8_t output, uint8_t input) {
  // Validate output
  if (output >= MUX_NUM_CHANNELS) {
    return false;
  }

  // Handle disconnection
  if (input == MUX_DISCONNECTED) {
    MUX_Disconnect(output);
    return true;
  }

  // Validate input
  if (input >= MUX_NUM_INPUTS) {
    return false;
  }

  // Set the routing (same input can be routed to multiple outputs)
  muxRouting[output] = input;
  MUX_ApplyHardware(output);

  return true;
}

uint8_t MUX_GetRoute(uint8_t output) {
  if (output >= MUX_NUM_CHANNELS) {
    return MUX_DISCONNECTED;
  }
  return muxRouting[output];
}

uint8_t MUX_GetInputUsage(uint8_t input) {
  if (input >= MUX_NUM_INPUTS) {
    return MUX_DISCONNECTED;
  }

  for (uint8_t i = 0; i < MUX_NUM_CHANNELS; i++) {
    if (muxRouting[i] == input) {
      return i;
    }
  }

  return MUX_DISCONNECTED;
}

void MUX_Disconnect(uint8_t output) {
  if (output < MUX_NUM_CHANNELS) {
    muxRouting[output] = MUX_DISCONNECTED;
    MUX_ApplyHardware(output);
  }
}

void MUX_DisconnectAll(void) {
  for (uint8_t i = 0; i < MUX_NUM_CHANNELS; i++) {
    muxRouting[i] = MUX_DISCONNECTED;
    MUX_ApplyHardware(i);
  }
}

bool AMP_SetGain(uint8_t channel, GainSetting_t gain) {
  if (channel >= MUX_NUM_CHANNELS || gain >= GAIN_COUNT) {
    return false;
  }

  ampGains[channel] = gain;
  AMP_ApplyHardware(channel);

  return true;
}

GainSetting_t AMP_GetGain(uint8_t channel) {
  if (channel >= MUX_NUM_CHANNELS) {
    return GAIN_1;
  }
  return ampGains[channel];
}

const char *AMP_GainToString(GainSetting_t gain) {
  if (gain >= GAIN_COUNT) {
    return "?";
  }
  return gainStrings[gain];
}

void MUX_PrintStatus(void) {
  UART_SendString("\r\n=== MUX Status ===\r\n");

  // Print routing (display 1-based to match board connector labels)
  UART_SendString("Routing:\r\n");
  for (uint8_t i = 0; i < MUX_NUM_CHANNELS; i++) {
    UART_SendString("  OUT");
    UART_SendInt(i + 1);
    UART_SendString(" <- ");
    if (muxRouting[i] == MUX_DISCONNECTED) {
      UART_SendString("X (disconnected)");
    } else {
      UART_SendString("IN");
      UART_SendInt(muxRouting[i] + 1);
    }
    UART_SendNewLine();
  }

  // Print gains (display 1-based to match board connector labels)
  UART_SendString("Gains:\r\n");
  for (uint8_t i = 0; i < MUX_NUM_CHANNELS; i++) {
    UART_SendString("  IN");
    UART_SendInt(i + 1);
    UART_SendString(": x");
    UART_SendString(AMP_GainToString(ampGains[i]));
    UART_SendNewLine();
  }

  UART_SendString("==================\r\n");
}
