/*
 * pin_init.h
 * GPIO pin initialization for the electronic board
 *
 *  Created on: 26 Nov 2025
 */

#ifndef PIN_INIT_H_
#define PIN_INIT_H_

#include "chip.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize all GPIO pins for the board
 * Configures LEDs, multiplexers, and amplifiers
 */
void GPIO_InitAllPins(void);


/**
 * @brief Initialize the led control pins
 */
void GPIO_InitStatusLed(void);

/**
 * @brief Initialize multiplexer control pins
 */
void GPIO_InitMultiplexers(void);

/**
 * @brief Initialize amplifier control pins
 */
void GPIO_InitAmplifiers(void);

#ifdef __cplusplus
}
#endif

#endif /* PIN_INIT_H_ */
