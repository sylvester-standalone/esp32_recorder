#pragma once

/* pin設定 */
// VSPI pin
#define VSPI_SCK               GPIO_NUM_18
#define VSPI_MOSI              GPIO_NUM_21         // default -> 23
#define VSPI_MISO              GPIO_NUM_19
#define VSPI_CS                GPIO_NUM_5

// I2S pin
#define I2S_STD_MCLK           GPIO_NUM_0          // I2S master clock io number
#define I2S_STD_BCLK           GPIO_NUM_33         // I2S bit clock io number
#define I2S_STD_WS             GPIO_NUM_32         // I2S word select io number
#define I2S_STD_DOUT           GPIO_NUM_25         // I2S data out io number
#define I2S_STD_DIN            GPIO_NUM_26         // I2S data in io number

// I2C pin
#define OLED_SDA               GPIO_NUM_22         // default SDA:GPIO21
#define OLED_SCL               GPIO_NUM_23         // default SCL:GPIO22

// Button pin
#define BTN_REC_PLAY           GPIO_NUM_34         // rec/playボタンピン(3.3V, pulldown)
#define BTN_MODE               GPIO_NUM_35         // mode切り替えボタンピン(3.3V, pulldown)