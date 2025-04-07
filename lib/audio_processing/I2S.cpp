#include "I2S.h"
#include "pin_config.h"

i2s_chan_handle_t rx_handle;
i2s_chan_handle_t tx_handle;

void i2s_init_onlyTx() {
  i2s_chan_config_t tx_chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
  tx_chan_cfg.auto_clear_after_cb = true;
  tx_chan_cfg.auto_clear_before_cb = true;

  ESP_ERROR_CHECK(i2s_new_channel(&tx_chan_cfg, &tx_handle, NULL));

  i2s_std_config_t tx_std_cfg = {
    .clk_cfg  = {
      .sample_rate_hz = I2S_SAMPLING_RATE,
      .clk_src = I2S_CLK_SRC_DEFAULT,
      .mclk_multiple = I2S_MCLK_MULTIPLE_256,
    },
    .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_BIT_RATE, I2S_NUM_CHANNEL),
    .gpio_cfg = {
      .mclk = I2S_GPIO_UNUSED,
      .bclk = I2S_STD_BCLK,
      .ws   = I2S_STD_WS,
      .dout = I2S_STD_DOUT,
      .din  = I2S_GPIO_UNUSED,
      .invert_flags = {
          .mclk_inv = false,
          .bclk_inv = false,
          .ws_inv   = false,
      },
    }
  };

  ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle, &tx_std_cfg));

}

void i2s_init_duplex() {
  i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);

  ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_handle, &rx_handle));

  i2s_std_config_t std_cfg = {
    .clk_cfg  = {
      .sample_rate_hz = I2S_SAMPLING_RATE,
      .clk_src = I2S_CLK_SRC_DEFAULT,
      .mclk_multiple = I2S_MCLK_MULTIPLE_256,
    },
    .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_BIT_RATE, I2S_NUM_CHANNEL),
    .gpio_cfg = {
      .mclk = I2S_STD_MCLK,
      .bclk = I2S_STD_BCLK,
      .ws   = I2S_STD_WS,
      .dout = I2S_STD_DOUT,
      .din  = I2S_STD_DIN,
      .invert_flags = {
          .mclk_inv = false,
          .bclk_inv = false,
          .ws_inv   = false,
      },
    },
  };

  ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle, &std_cfg));
  ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle, &std_cfg));
  
}