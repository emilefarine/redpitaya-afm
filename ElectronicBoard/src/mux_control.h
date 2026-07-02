/*
 * mux_control.h
 * Multiplexer and Amplifier control for the Electronic Board
 *
 * Hardware: ADG1409YRUZ multiplexers, PGA855 variable gain amplifiers
 *
 *  Created on: 26 Nov 2025
 */

#ifndef MUX_CONTROL_H_
#define MUX_CONTROL_H_

#include "chip.h"
#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

/*****************************************************************************
 * Constants
 *****************************************************************************/

#define MUX_NUM_CHANNELS 4    // 4 multiplexer outputs
#define MUX_NUM_INPUTS 4      // 4 possible inputs per mux
#define MUX_DISCONNECTED 0xFF // Value for disconnected output

// Gain codes for PGA849 (3-bit: A2, A1, A0)
typedef enum {
  GAIN_1_8 = 0,
  GAIN_1_4 = 1,
  GAIN_1_2 = 2,
  GAIN_1 = 3,
  GAIN_2 = 4,
  GAIN_4 = 5,
  GAIN_8 = 6,
  GAIN_16 = 7,
  GAIN_COUNT = 8
} GainSetting_t;

/**
 * @brief Initialize multiplexer control module
 * Sets all outputs to disconnected and gains to 1
 */
void MUX_Init(void);

/**
 * @brief Set multiplexer routing
 * @param output Output channel (0-3, maps to board connector OUT1-OUT4)
 * @param input Input channel (0-3, maps to board connector IN1-IN4) or MUX_DISCONNECTED to disable
 * @return true if successful, false if invalid parameters or input already used
 */
bool MUX_SetRoute(uint8_t output, uint8_t input);

/**
 * @brief Get current multiplexer routing
 * @param output Output channel (0-3, maps to board connector OUT1-OUT4)
 * @return Input channel (0-3) or MUX_DISCONNECTED if disabled
 */
uint8_t MUX_GetRoute(uint8_t output);

/**
 * @brief Check if an input is already routed to an output
 * @param input Input channel (0-3, maps to board connector IN1-IN4)
 * @return Output channel (0-3) if routed, or MUX_DISCONNECTED if not used
 */
uint8_t MUX_GetInputUsage(uint8_t input);

/**
 * @brief Disconnect a multiplexer output
 * @param output Output channel (0-3, maps to board connector OUT1-OUT4)
 */
void MUX_Disconnect(uint8_t output);

/**
 * @brief Disconnect all multiplexer outputs
 */
void MUX_DisconnectAll(void);

/**
 * @brief Set amplifier gain for an input channel
 * @param channel Input channel (0-3, maps to board connector IN1-IN4)
 * @param gain Gain setting (GAIN_1_8 to GAIN_16)
 * @return true if successful, false if invalid parameters
 */
bool AMP_SetGain(uint8_t channel, GainSetting_t gain);

/**
 * @brief Get current amplifier gain for a channel
 * @param channel Input channel (0-3, maps to board connector IN1-IN4)
 * @return Current gain setting
 */
GainSetting_t AMP_GetGain(uint8_t channel);

/**
 * @brief Get gain value as a string (for display/debug)
 * @param gain Gain setting
 * @return String representation (e.g., "1/4", "2", "8")
 */
const char *AMP_GainToString(GainSetting_t gain);

/**
 * @brief Print current configuration via UART
 */
void MUX_PrintStatus(void);

#ifdef __cplusplus
}
#endif

#endif /* MUX_CONTROL_H_ */
