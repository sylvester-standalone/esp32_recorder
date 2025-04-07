#pragma once

#include <Arduino.h>

void convertBit32to16(const uint8_t *input, uint8_t *output, size_t num_samples);

void convertBit16to32(const uint8_t *input, uint8_t *output, size_t num_samples);