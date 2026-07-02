#include <iostream>
#include <cstdlib>
#include <unistd.h>
#include <chrono>
#include "rp.h"

int main(int argc, char **argv) {
    constexpr unsigned int period = 1000000; // uS
    unsigned int led_index = 0; // Default to LED 0

    // index of blinking LED can be provided as an argument
    if (argc > 1) {
        led_index = std::atoi(argv[1]);
    }

    std::cout << "Blinking LED[" << led_index << "]" << std::endl;
    
    // Convert to proper rp_dpin_t type
    rp_dpin_t led = static_cast<rp_dpin_t>(RP_LED0 + led_index);

    // Initialization of API
    if (rp_Init() != RP_OK) {
        std::cerr << "Red Pitaya API init failed!" << std::endl;
        return EXIT_FAILURE;
    }

    constexpr unsigned int retries = 1000;
    
    for (unsigned int i = 0; i < retries; ++i) {
        if (rp_DpinSetState(led, RP_HIGH) != RP_OK) {
            std::cerr << "Failed to set LED high!" << std::endl;
            break;
        }
        usleep(period / 2);
        
        if (rp_DpinSetState(led, RP_LOW) != RP_OK) {
            std::cerr << "Failed to set LED low!" << std::endl;
            break;
        }
        usleep(period / 2);
    }

    // Releasing resources
    rp_Release();

    return EXIT_SUCCESS;
}