#include "processing.h"

void convertBit32to16(const uint8_t *input, uint8_t *output, size_t num_samples) {
  for (size_t i = 0; i < num_samples; i++) {
    output[i * 2] = input[i * 4 + 2];         // 下位バイト
    output[i * 2 + 1] = input[i * 4 + 3]; // 上位バイト
  }
}

void convertBit16to32(const uint8_t *input, uint8_t *output, size_t num_samples) {
  uint8_t reduction_coeff = 2;
  uint16_t value16;
  for (size_t i = 0; i < num_samples; i++) {
    value16 = input[i * 2 + 1] << 8 | input[i * 2];
    value16 /= reduction_coeff;
    
    output[i * 4 + 2] = value16 & 0xFF;
    output[i * 4 + 3] = (value16 >> 8) & 0xFF;
  }
}