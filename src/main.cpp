#include <Arduino.h>
#include "driver/gpio.h"
#include "SdFat.h"
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "format_wav.h"
#include "pin_config.h"
#include "I2S.h"
#include "processing.h"
#include "bitmap.h"

/* 定数定義 */
// ディスプレイ設定
#define SCREEN_WIDTH      128
#define SCREEN_HEIGHT     64
#define SCREEN_ADDRESS    0x3C    // SSD1306 I2C address
#define OLED_RESET        -1      // Reset pin # (or -1 if sharing Arduino reset pin)

// 録音設定(I2S設定と異なる音質で保存)
#define REC_SEC           10      // 最大録音時間[s]
#define REC_BIT_RATE      16      // 録音ビットレート
#define REC_SAMPLING_RATE 44100   // 録音サンプリングレート
#define REC_NUM_CHANNEL   1       // 録音チャンネル数

// バッファ, キューのサイズ設定
#define I2S_BUFF_SIZE     4096    // I2S用バッファサイズ(Byte)
#define REC_BUFF_SIZE     I2S_BUFF_SIZE / ((I2S_BIT_RATE * I2S_NUM_CHANNEL) / (REC_BIT_RATE * REC_NUM_CHANNEL))    // 実際に録音するデータを拾うバッファのサイズ
#define QUEUE_LENGTH_REC  4       // 録音時、確保するキューの数
#define QUEUE_LENGTH_PLAY 1       // 再生時、確保するキューの数

#define I2S_NUM_SAMPLES   I2S_BUFF_SIZE / (I2S_BIT_RATE * I2S_NUM_CHANNEL / 8)    // I2S音声データのサンプル数

// チャタリング対策のインターバル設定
#define REFUSE_TIME_REC   200
#define REFUSE_TIME_PLAY  110
#define REFUSE_TIME_MODE  500

#define SD_CONFIG SdSpiConfig(VSPI_CS, DEDICATED_SPI, SD_SCK_MHZ(24)) // SDカードとの通信設定を定義

// ディスプレイ操作用オブジェクトを生成
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// 録音音声データの最大データ量
uint32_t max_rec_byte = (REC_BIT_RATE * REC_NUM_CHANNEL / 8) * I2S_SAMPLING_RATE * REC_SEC;

// wavファイルのヘッダー情報を作成
const wav_header_t wav_header = WAV_HEADER_PCM_DEFAULT(max_rec_byte, REC_BIT_RATE, I2S_SAMPLING_RATE, REC_NUM_CHANNEL);

// キュー作成
QueueHandle_t audioRecQueue;     // 録音時のキューハンドル
QueueHandle_t audioPlayQueue;    // 再生時のキューハンドル

// 録音・再生中の残りデータ量を保存
int rec_rest_size = max_rec_byte;
int play_rest_size = 0;

uint8_t circle_size = 0;

/* 状態検出フラッグ */
volatile byte mode_flg = 0;
volatile byte recording_flg = 0;
volatile byte rec_end_flg = 0;
volatile byte play_pushed_flg = 0;
volatile byte playing_flg = 0;
volatile byte play_end_flg = 0;
volatile byte display_flg = 0;

/* 関数プロトタイプ宣言 */
static void i2s_dac_task(void *args);
static void i2s_tx_task(void *args);
static void sd_rw_task(void *args);
void initialDisp(void);
void modeDisp(void);
void stateDisp(void);
void interruptDisp(void);
void speakernDisp(void);

/* 割り込み関数 */
void IRAM_ATTR onRecPlayBtn()
{
  static uint32_t lastmills_rec = 0;       // 前回割り込みHIGH発生時の時間
  static uint32_t lastmills_play_high = 0; // 前回割り込みHIGH発生時の時間
  // rec modeの場合 -> 録音の開始/終了
  if (mode_flg == 0)
  {
    if (millis() - lastmills_rec > REFUSE_TIME_REC)
    {
      if (gpio_get_level(BTN_REC_PLAY) == HIGH)
      {
        recording_flg = 1;
        Serial.printf("recording_flg: %d\n", recording_flg);

        Serial.println("RISE");
      }
      else
      {
        // rec中なら終了フラグ(録音上限達した時の処理と区別するため)
        if (recording_flg == 1)
        {
          recording_flg = 0;
          rec_end_flg = 1;
          if (display_flg == 0)
            display_flg = 1;
        }
        Serial.println("FALL");
      }
      lastmills_rec = millis();
    }
  }

  // play modeの場合 -> 再生
  else
  {
    // ボタン押下時に再生
    if (gpio_get_level(BTN_REC_PLAY) == HIGH && millis() - lastmills_play_high > REFUSE_TIME_PLAY)
    {
      if (play_pushed_flg == 0)
        play_pushed_flg = 1;
      if (playing_flg == 0)
        playing_flg = 1;
      lastmills_play_high = millis();
    }
  }
}

void IRAM_ATTR onModeBtn()
{
  static uint32_t lastmills_mode = 0; // 前回割り込み発生時の時間
  // チャタリング対策(前回の割り込みから一定時間経過していないと承認しない)
  if (millis() - lastmills_mode > REFUSE_TIME_MODE)
  {
    mode_flg = !mode_flg;
    Serial.printf("mode change: %d(%s mode)\n", mode_flg, mode_flg == 0 ? "REC" : "PLAY");

    // play modeに切り替わったときに、録音強制終了
    if (mode_flg == 1 && recording_flg == 1)
    {
      recording_flg = 0;
      rec_end_flg = 1;
      if (display_flg == 0)
        display_flg = 1;
    }

    // rec modeに切り替わった時に、再生強制終了
    if (mode_flg == 0)
    {
      if (play_pushed_flg == 1)
        play_pushed_flg = 0;
      if (playing_flg == 1)
        playing_flg = 0;
      play_end_flg = 1;
      Serial.printf("play_end_flg: %d\n", play_end_flg);
    }

    lastmills_mode = millis();
  }
}

/* タスク定義 */
static void i2s_tx_task(void *args)
{
  Serial.println("play_task start");

  uint8_t *play_q_receive_buf = (uint8_t *)calloc(1, I2S_BUFF_SIZE);
  assert(play_q_receive_buf); // Check if r_buf allocation success

  size_t w_bytes = I2S_BUFF_SIZE;

  i2s_init_onlyTx();
  ESP_ERROR_CHECK(i2s_channel_enable(tx_handle));

  while (mode_flg == 1)
  {
    // Serial.println("play_task running");

    if (xQueueReceive(audioPlayQueue, play_q_receive_buf, 0) == pdTRUE)
    {
      if (i2s_channel_write(tx_handle, play_q_receive_buf, I2S_BUFF_SIZE, &w_bytes, 1000) == ESP_OK)
      {
        // Serial.printf("Queue: %d\n", uxQueueMessagesWaiting(audioPlayQueue));
      }
      else
      {
        printf("Write Task: i2s write failed\n");
      }
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }

  // rec modeに切り替わったらキューを空にする
  // こうしないとキューが満杯の場合にモードが切り替わってもsd write側のqueuesendが待ち続ける
  xQueueReset(audioPlayQueue);

  /* Disable the TX channel */
  ESP_ERROR_CHECK(i2s_channel_disable(tx_handle));

  i2s_del_channel(tx_handle);

  xTaskCreatePinnedToCore(i2s_dac_task, "i2s_dac_task", 4096, NULL, 5, NULL, PRO_CPU_NUM);

  free(play_q_receive_buf);

  vTaskDelete(NULL);
}

static void i2s_dac_task(void *args)
{

  circle_size = 0;

  Serial.println("dac_task running");

  // buffer
  uint8_t *i2s_rw_buf = (uint8_t *)calloc(1, I2S_BUFF_SIZE);
  uint8_t *rec_q_send_buf = (uint8_t *)calloc(1, REC_BUFF_SIZE);

  assert(i2s_rw_buf); // Check if rw_buf allocation success
  assert(rec_q_send_buf); // Check if rw_buf allocation success
  size_t i2s_rw_bytes = 0;

  i2s_init_duplex();

  /* Enable the RX channel */
  ESP_ERROR_CHECK(i2s_channel_enable(rx_handle));
  ESP_ERROR_CHECK(i2s_channel_enable(tx_handle));

  while (mode_flg == 0)
  {
    /* Read i2s data */
    if (i2s_channel_read(rx_handle, i2s_rw_buf, I2S_BUFF_SIZE, &i2s_rw_bytes, 1000) == ESP_OK)
    {

      circle_size = i2s_rw_buf[2] / 26;

      // 録音上限に達したら強制的に録音終了
      if (rec_rest_size <= 0)
      {
        recording_flg = 0;
        rec_end_flg = 1;
        if (display_flg == 0)
          display_flg = 1;
      }

      // チャタリングでボタンをホールドしていないのにrecが走るってしまうのを回避
      if (recording_flg == 1 && gpio_get_level(BTN_REC_PLAY) == LOW)
      {
        recording_flg = 0;
        rec_end_flg = 1;
        if (display_flg == 0)
          display_flg = 1;
      }

      // buttonがHIGH, restsizeが残っている場合は録音処理
      if (recording_flg == 1)
      {
        convertBit32to16(i2s_rw_buf, rec_q_send_buf, I2S_NUM_SAMPLES);

        // 取得データをキューに送る（容量がいっぱいなら待機）
        if (xQueueSend(audioRecQueue, rec_q_send_buf, portMAX_DELAY) != pdTRUE)
        {
          printf("Queue full, data lost\n");
        }

        printf("rest:%d\n", rec_rest_size);
        rec_rest_size -= REC_BUFF_SIZE; // rest_sizeを減らす
      }

      if (i2s_channel_write(tx_handle, i2s_rw_buf, I2S_BUFF_SIZE, &i2s_rw_bytes, 1000) == ESP_OK)
      {
      }
      else
      {
        printf("Write Task: i2s write failed\n");
      }
    }
    else
    {
      printf("Read Task: i2s read failed\n");
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }

  /* Disable the RX channel */
  ESP_ERROR_CHECK(i2s_channel_disable(rx_handle));
  ESP_ERROR_CHECK(i2s_channel_disable(tx_handle));

  i2s_del_channel(rx_handle);
  i2s_del_channel(tx_handle);

  circle_size = 0;

  xTaskCreatePinnedToCore(i2s_tx_task, "i2s_tx_task", 4096, NULL, 5, NULL, PRO_CPU_NUM);

  free(i2s_rw_buf);
  free(rec_q_send_buf);
  vTaskDelete(NULL);
}

static void sd_rw_task(void *args)
{
  SdFs sd;
  FsFile file;

  uint8_t *rec_q_receive_buf = (uint8_t *)calloc(1, REC_BUFF_SIZE);
  uint8_t *sd_r_buf = (uint8_t *)calloc(1, REC_BUFF_SIZE);
  uint8_t *play_q_send_buf = (uint8_t *)calloc(1, I2S_BUFF_SIZE);

  assert(rec_q_receive_buf);   // Check if sd_buf allocation success
  assert(sd_r_buf);  // Check if sd_buf allocation success
  assert(play_q_send_buf); // Check if sd_buf allocation success

  size_t r_bytes = I2S_BUFF_SIZE;

  if (!sd.begin(SD_CONFIG))
  {
    Serial.println("Card Mount Failed");
    return;
  }

  while (1)
  {
    // 再生モード終了処理
    if (play_end_flg == 1)
    {
      if (file.isOpen())
      {
        file.close();
      }
      play_end_flg = 0;
      Serial.println("play mode end");
    }

    /* ---------------- rec modeでの処理 ----------------*/

    if (recording_flg == 1 || rec_end_flg == 1)
    {

      // 初期化処理
      if (!file.isOpen())
      {

        // 古いファイルがあれば削除(これもここでやらないと上書きできない)
        if (sd.exists("/test_new.wav"))
        {
          sd.remove("/test_new.wav");
        }

        // 新規作成
        file = sd.open("/test_new.wav", FILE_WRITE);

        // wavヘッダー書き込み
        uint8_t byteArray[sizeof(wav_header)];
        memcpy(byteArray, &wav_header, sizeof(wav_header));
        file.write(byteArray, sizeof(wav_header));
      }

      // キュー待ち
      if (xQueueReceive(audioRecQueue, rec_q_receive_buf, 0) == pdTRUE)
      {
        file.write(rec_q_receive_buf, REC_BUFF_SIZE); // SDへの書き出し
      }

      // 録音が終了していて、ファイルが開いているなら終了処理
      if (rec_end_flg == 1 && file.isOpen())
      {

        // 終了処理
        if (uxQueueMessagesWaiting(audioRecQueue) == 0)
        {
          file.close();
          if (sd.exists("/test_new.wav"))
          {
            Serial.println("Success");
          }
          else
          {
            Serial.println("Fail");
          }
          rec_end_flg = 0;
          rec_rest_size = max_rec_byte; // rec_time reset
        }
      }
    }

    /* ---------------- play modeでの処理 ----------------*/

    if (mode_flg == 1 && rec_end_flg == 0)
    {
      // fileが開いていないなら開いておく
      if (!file.isOpen())
      {
        file = sd.open("/test_new.wav");
      }

      if (play_pushed_flg == 1)
      {
        file.seek(sizeof(wav_header));
        play_rest_size = file.size() - sizeof(wav_header);
        play_pushed_flg = 0;
      }

      // ボタンが押されたら、readしてqueueに渡す。
      if (playing_flg == 1)
      {
        // 音声サイズ分読み込む
        if (file.read(sd_r_buf, REC_BUFF_SIZE))
        {
          Serial.printf("rest read size:%d\n", play_rest_size);
          convertBit16to32(sd_r_buf, play_q_send_buf, I2S_NUM_SAMPLES);
          circle_size = play_q_send_buf[2] / 26;
          // 取得データをキューに送る（容量がいっぱいなら待機）
          if (xQueueSend(audioPlayQueue, play_q_send_buf, portMAX_DELAY) != pdTRUE)
          {
            printf("Queue full, data lost\n");
          }

          play_rest_size -= REC_BUFF_SIZE;
        }
        if (play_rest_size <= 0)
        {
          Serial.println("play end");
          playing_flg = 0;
          circle_size = 0;
        }
      }
    }

    vTaskDelay(pdMS_TO_TICKS(10));
  }

  Serial.println("sd_write_task end");
  free(rec_q_receive_buf);
  free(sd_r_buf);
  // free(w_mono_buf);
  vTaskDelete(NULL);
}

void setup()
{
  Serial.begin(115200);                               // Set up Serial Monitor
  SPI.begin(VSPI_SCK, VSPI_MISO, VSPI_MOSI, VSPI_CS); // SPIピン番号設定(デフォルト値から変更)

  // 自分でピン番号を指定
  Wire.begin(OLED_SDA, OLED_SCL); // Wire.setPins(OLED_SDA, OLED_SCL)でもいいはず

  audioRecQueue = xQueueCreate(QUEUE_LENGTH_REC, REC_BUFF_SIZE);
  audioPlayQueue = xQueueCreate(QUEUE_LENGTH_PLAY, I2S_BUFF_SIZE);

  if (!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS))
  {
    Serial.println("oled error");
    for (;;);
  };

  gpio_set_direction(BTN_REC_PLAY, GPIO_MODE_INPUT);
  gpio_set_direction(BTN_MODE, GPIO_MODE_INPUT);

  initialDisp();

  attachInterrupt(BTN_REC_PLAY, onRecPlayBtn, CHANGE);
  attachInterrupt(BTN_MODE, onModeBtn, FALLING);

  xTaskCreatePinnedToCore(i2s_dac_task, "i2s_dac_task", 4096, NULL, 5, NULL, PRO_CPU_NUM);
  xTaskCreatePinnedToCore(sd_rw_task, "sd_rw_task", 4096, NULL, 5, NULL, APP_CPU_NUM);
}

void loop()
{
  display.clearDisplay(); // display初期化

  if (display_flg == 1)
  {
    interruptDisp(); // recording end表示
  }
  else
  {
    modeDisp(); // mode表示
    stateDisp();
    speakernDisp();

    display.display(); // 設定した内容をディスプレイに表示
  }

  delay(50);
}

/* ディスプレイ表示用関数 */
void initialDisp(void)
{
  int logo_cursor_y = -50;
  uint8_t logo_end_y = 6;

  uint8_t disp_count = 1;
  bool isDisp = true;

  // ボタン押されるまで表示
  while (gpio_get_level(BTN_REC_PLAY) == LOW && gpio_get_level(BTN_MODE) == LOW)
  {
    display.clearDisplay(); // 画面初期化

    display.setTextColor(WHITE); // 文字色
    display.setTextSize(1);      // テキストサイズ

    if (disp_count % 5 == 0)
      isDisp = !isDisp;

    display.setCursor(10, 50);
    if (isDisp && logo_cursor_y >= logo_end_y)
      display.println("PRESS ANY BUTTON.."); // 文字列表示
    display.drawBitmap(0, logo_cursor_y, bitmap_test, 128, 64, WHITE);

    display.display(); // 表示

    if (logo_cursor_y < logo_end_y)
      logo_cursor_y += 2;
    disp_count++;

    delay(50);
  }

  isDisp = true;

  // ボタン押された時のエフェクト
  for (int i; i < 12; i++)
  {
    display.clearDisplay(); // 画面初期化

    display.setTextColor(WHITE); // 文字色
    display.setTextSize(1);      // テキストサイズ

    display.setCursor(10, 50);
    if (isDisp)
      display.println("PRESS ANY BUTTON.."); // 文字列表示

    display.drawBitmap(0, logo_end_y, bitmap_test, 128, 64, WHITE); // ロゴ表示

    display.display(); // 表示

    isDisp = !isDisp;

    delay(30);
  }

  delay(100);
}

void modeDisp(void)
{

  display.setTextColor(WHITE);
  display.setTextSize(1);
  display.setCursor(20, 10); // 文字座標

  // rec modeの場合
  if (mode_flg == 0)
  {
    display.println("MODE: REC"); // 文字列表示
  }
  // play mode
  else
  {
    display.println("MODE: PLAY"); // 文字列表示
  }
}

void stateDisp(void)
{

  display.setTextColor(WHITE); // 文字色
  display.setTextSize(1);      // テキストサイズ
  display.setCursor(20, 50);

  // rec modeの場合
  if (mode_flg == 0)
  {
    // recording now
    if (recording_flg == 1)
    {
      display.println("RECORDING..."); // 文字列表示
    }
    // waiting for recording
    else
    {
      display.println("WAITING REC"); // 文字列表示
    }
  }
  // play mode
  else
  {
    // playing now
    if (playing_flg == 1)
    {
      display.println("PLAYING..."); // 文字列表示
    }
    // waiting for playing
    else
    {
      display.println("WAITING PLAY"); // 文字列表示
    }
  }
}

void interruptDisp(void)
{

  display.setTextColor(WHITE); // 文字色
  display.setTextSize(1);      // テキストサイズ
  display.setCursor(20, 30);

  display.println("COMPLETE!"); // 文字列表示

  display.display(); // 設定した内容をディスプレイに表示

  delay(400);
  display_flg = 0;
}

void speakernDisp(void)
{
  display.fillCircle(64, 32, 3, SSD1306_INVERSE);
  display.drawCircle(64, 32, 4 + circle_size, SSD1306_INVERSE);
}
