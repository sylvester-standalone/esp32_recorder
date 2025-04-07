#pragma once

#include <Arduino.h>
#include "driver/i2s_std.h"

#define I2S_SAMPLING_RATE     44100
#define I2S_BIT_RATE          I2S_DATA_BIT_WIDTH_32BIT
#define I2S_NUM_CHANNEL       I2S_SLOT_MODE_MONO

extern i2s_chan_handle_t rx_handle;
extern i2s_chan_handle_t tx_handle;

void i2s_init_onlyTx();
void i2s_init_duplex();