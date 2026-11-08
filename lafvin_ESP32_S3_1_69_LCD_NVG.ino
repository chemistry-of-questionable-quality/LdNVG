// Near-IR night vision monocular -- LAFVIN ESP32-S3 1.69" LCD board (OV3660)
// NO EXTRA LIBRARIES: uses only SPI.h and esp_camera.h from the ESP32 core,
// with a small built-in ST7789 driver.
//
// BOOT button (GPIO0):
//   short press = cycle view mode (NIGHT green -> MONO -> DAY colour)
//   long press  = cycle LCD backlight (100% -> 50% -> 15% -> 4%)
//
// Status strip (top 20 px of the screen, no text font needed):
//   coloured square = mode (green NIGHT, grey MONO, yellow DAY)
//   number          = frames per second
//   4 bars (right)  = backlight level
//
// Board settings: ESP32S3 Dev Module, 16MB flash, PSRAM = OPI PSRAM.
// Start-up error colours: RED screen = no PSRAM, MAGENTA = camera init failed.

#include <SPI.h>
#include "esp_camera.h"

// ---------- LCD pins ----------
#define TFT_MOSI 39
#define TFT_SCLK 38
#define TFT_CS   14
#define TFT_DC   10
#define TFT_RST  3
#define TFT_BL   13
#define BTN_PIN  0

#define SPI_HZ   80000000   // lower to 40000000 if the picture glitches
#define ROW_OFF  20         // 240x280 panel sits 20 rows into the controller RAM
#define SCR_W    240
#define SCR_H    280
#define STRIP_H  20
#define SWAP_BYTES 0        // set to 1 if red and blue look swapped

// ---------- Camera pins (OV3660) ----------
#define PWDN_GPIO_NUM   -1
#define RESET_GPIO_NUM  -1
#define XCLK_GPIO_NUM   17
#define SIOD_GPIO_NUM   1
#define SIOC_GPIO_NUM   2
#define VSYNC_GPIO_NUM  21
#define HREF_GPIO_NUM   18
#define PCLK_GPIO_NUM   16
#define Y2_GPIO_NUM     7
#define Y3_GPIO_NUM     5
#define Y4_GPIO_NUM     4
#define Y5_GPIO_NUM     6
#define Y6_GPIO_NUM     8
#define Y7_GPIO_NUM     9
#define Y8_GPIO_NUM     11
#define Y9_GPIO_NUM     15

// Flip these if the image is upside down / mirrored in your housing
#define IMG_VFLIP    0
#define IMG_HMIRROR  0

// =====================================================================
//  Minimal ST7789 driver
// =====================================================================
SPIClass lcdSPI(HSPI);
static const SPISettings lcdSettings(SPI_HZ, MSBFIRST, SPI_MODE0);
static uint8_t stripBuf[SCR_W * STRIP_H * 2];   // 9600 bytes, reused for strip/fills

void lcdCmd(uint8_t c) {
  digitalWrite(TFT_DC, LOW);
  digitalWrite(TFT_CS, LOW);
  lcdSPI.beginTransaction(lcdSettings);
  lcdSPI.write(c);
  lcdSPI.endTransaction();
  digitalWrite(TFT_CS, HIGH);
}

void lcdData(const uint8_t *d, size_t n) {
  digitalWrite(TFT_DC, HIGH);
  digitalWrite(TFT_CS, LOW);
  lcdSPI.beginTransaction(lcdSettings);
  lcdSPI.writeBytes(d, n);
  lcdSPI.endTransaction();
  digitalWrite(TFT_CS, HIGH);
}

void lcdCmdData(uint8_t c, const uint8_t *d, size_t n) {
  lcdCmd(c);
  if (n) lcdData(d, n);
}

#define CMD(c, ...) do { const uint8_t d_[] = {__VA_ARGS__}; lcdCmdData(c, d_, sizeof(d_)); } while (0)

void lcdSetWindow(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
  y0 += ROW_OFF;
  y1 += ROW_OFF;
  CMD(0x2A, (uint8_t)(x0 >> 8), (uint8_t)x0, (uint8_t)(x1 >> 8), (uint8_t)x1);
  CMD(0x2B, (uint8_t)(y0 >> 8), (uint8_t)y0, (uint8_t)(y1 >> 8), (uint8_t)y1);
  lcdCmd(0x2C);   // memory write
}

// data = big-endian RGB565 (high byte first)
void lcdPush(uint16_t x, uint16_t y, uint16_t w, uint16_t h, const uint8_t *data) {
  lcdSetWindow(x, y, x + w - 1, y + h - 1);
  lcdData(data, (size_t)w * h * 2);
}

void lcdInit() {
  pinMode(TFT_DC, OUTPUT);
  pinMode(TFT_CS, OUTPUT);
  pinMode(TFT_RST, OUTPUT);
  digitalWrite(TFT_CS, HIGH);
  lcdSPI.begin(TFT_SCLK, -1, TFT_MOSI, -1);

  digitalWrite(TFT_RST, HIGH); delay(10);
  digitalWrite(TFT_RST, LOW);  delay(20);
  digitalWrite(TFT_RST, HIGH); delay(120);

  lcdCmd(0x01); delay(150);          // software reset
  lcdCmd(0x11); delay(120);          // sleep out
  CMD(0x3A, 0x55);                   // 16-bit colour
  CMD(0x36, 0x00);                   // orientation / RGB order
  CMD(0xB2, 0x0C, 0x0C, 0x00, 0x33, 0x33);
  CMD(0xB7, 0x35);
  CMD(0xBB, 0x19);
  CMD(0xC0, 0x2C);
  CMD(0xC2, 0x01);
  CMD(0xC3, 0x12);
  CMD(0xC4, 0x20);
  CMD(0xC6, 0x0F);
  CMD(0xD0, 0xA4, 0xA1);
  CMD(0xE0, 0xD0, 0x04, 0x0D, 0x11, 0x13, 0x2B, 0x3F, 0x54, 0x4C, 0x18, 0x0D, 0x0B, 0x1F, 0x23);
  CMD(0xE1, 0xD0, 0x04, 0x0C, 0x11, 0x13, 0x2C, 0x3F, 0x44, 0x51, 0x2F, 0x1F, 0x1F, 0x20, 0x23);
  lcdCmd(0x21);                      // display inversion on (needed for this IPS panel)
  lcdCmd(0x13);                      // normal mode
  lcdCmd(0x29);                      // display on
  delay(20);
}

// Fill the whole screen with one RGB565 colour
void lcdFill(uint16_t color) {
  for (size_t i = 0; i < sizeof(stripBuf); i += 2) {
    stripBuf[i] = color >> 8;
    stripBuf[i + 1] = color & 0xFF;
  }
  for (int y = 0; y < SCR_H; y += STRIP_H) lcdPush(0, y, SCR_W, STRIP_H, stripBuf);
}

// =====================================================================
//  Status strip (drawn into a buffer, sent once per second)
// =====================================================================
static void setPix(int x, int y, uint16_t c) {
  if (x < 0 || x >= SCR_W || y < 0 || y >= STRIP_H) return;
  size_t i = ((size_t)y * SCR_W + x) * 2;
  stripBuf[i] = c >> 8;
  stripBuf[i + 1] = c & 0xFF;
}
static void rectBuf(int x, int y, int w, int h, uint16_t c) {
  for (int yy = 0; yy < h; yy++) for (int xx = 0; xx < w; xx++) setPix(x + xx, y + yy, c);
}
// 3x5 digit font, 15 bits each (row-major, MSB = top-left)
static const uint16_t DIGITS[10] = {0x7B6F, 0x2C97, 0x73E7, 0x73CF, 0x5BC9,
                                    0x79CF, 0x79EF, 0x7249, 0x7BEF, 0x7BCF};
static void glyphBuf(int x, int y, int digit, int s, uint16_t c) {
  uint16_t g = DIGITS[digit];
  for (int r = 0; r < 5; r++)
    for (int col = 0; col < 3; col++)
      if (g & (1 << (14 - (r * 3 + col)))) rectBuf(x + col * s, y + r * s, s, s, c);
}

// =====================================================================
//  Backlight PWM (works on Arduino-ESP32 core 2.x and 3.x)
// =====================================================================
static const uint8_t BL_LEVELS[] = {255, 128, 38, 10};
static uint8_t blIndex = 0;

void backlightInit() {
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttach(TFT_BL, 5000, 8);
#else
  ledcSetup(0, 5000, 8);
  ledcAttachPin(TFT_BL, 0);
#endif
}
void backlightSet(uint8_t duty) {
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(TFT_BL, duty);
#else
  ledcWrite(0, duty);
#endif
}

// =====================================================================
//  View modes / sensor tuning
// =====================================================================
enum Mode { NIGHT_GREEN, NIGHT_MONO, DAY, MODE_COUNT };
static const uint16_t MODE_COLORS[] = {0x07E0, 0xBDF7, 0xFFE0};
static Mode mode = NIGHT_GREEN;

// Call a sensor function only if this sensor driver implements it.
#define SET(fn, ...) do { if (s->fn) s->fn(s, __VA_ARGS__); } while (0)

void applyMode(int m) {   // int (not Mode): Arduino auto-prototypes are placed before the enum
  sensor_t *s = esp_camera_sensor_get();
  if (!s) return;

  SET(set_vflip, IMG_VFLIP);
  SET(set_hmirror, IMG_HMIRROR);
  SET(set_lenc, 1);

  if (m == DAY) {
    SET(set_special_effect, 0);
    SET(set_whitebal, 1);
    SET(set_awb_gain, 1);
    SET(set_gain_ctrl, 1);
    SET(set_exposure_ctrl, 1);
    SET(set_aec2, 0);
    SET(set_ae_level, 0);
    SET(set_gainceiling, GAINCEILING_8X);
    SET(set_brightness, 0);
    SET(set_contrast, 0);
    SET(set_saturation, 0);
  } else {
    SET(set_special_effect, m == NIGHT_GREEN ? 4 : 2);  // 4 = green tint, 2 = grayscale
    SET(set_whitebal, 0);
    SET(set_awb_gain, 0);
    SET(set_gain_ctrl, 1);
    SET(set_exposure_ctrl, 1);
    SET(set_aec2, 1);
    SET(set_ae_level, 2);
    SET(set_gainceiling, GAINCEILING_128X);
    SET(set_brightness, 1);
    SET(set_contrast, 1);
    SET(set_saturation, -2);
  }
}

// =====================================================================
void setup() {
  Serial.begin(115200);
  pinMode(BTN_PIN, INPUT_PULLUP);

  lcdInit();
  backlightInit();
  backlightSet(BL_LEVELS[blIndex]);
  lcdFill(0x0000);

  if (!psramFound()) {
    Serial.println("PSRAM not found - set Tools > PSRAM > OPI PSRAM");
    lcdFill(0xF800);                 // red screen
    while (true) delay(1000);
  }

  camera_config_t cfg = {};
  cfg.ledc_channel = LEDC_CHANNEL_7;   // camera clock gets its own LEDC timer/channel
  cfg.ledc_timer   = LEDC_TIMER_3;     // so it can't collide with the backlight PWM
  cfg.pin_d0 = Y2_GPIO_NUM;  cfg.pin_d1 = Y3_GPIO_NUM;
  cfg.pin_d2 = Y4_GPIO_NUM;  cfg.pin_d3 = Y5_GPIO_NUM;
  cfg.pin_d4 = Y6_GPIO_NUM;  cfg.pin_d5 = Y7_GPIO_NUM;
  cfg.pin_d6 = Y8_GPIO_NUM;  cfg.pin_d7 = Y9_GPIO_NUM;
  cfg.pin_xclk  = XCLK_GPIO_NUM;
  cfg.pin_pclk  = PCLK_GPIO_NUM;
  cfg.pin_vsync = VSYNC_GPIO_NUM;
  cfg.pin_href  = HREF_GPIO_NUM;
  cfg.pin_sccb_sda = SIOD_GPIO_NUM;
  cfg.pin_sccb_scl = SIOC_GPIO_NUM;
  cfg.pin_pwdn  = PWDN_GPIO_NUM;
  cfg.pin_reset = RESET_GPIO_NUM;
  cfg.xclk_freq_hz = 20000000;
  cfg.pixel_format = PIXFORMAT_RGB565;
  cfg.frame_size   = FRAMESIZE_240X240;
  cfg.fb_count     = 2;
  cfg.fb_location  = CAMERA_FB_IN_PSRAM;
  cfg.grab_mode    = CAMERA_GRAB_LATEST;

  esp_err_t err = esp_camera_init(&cfg);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed: 0x%x\n", err);
    lcdFill(0xF81F);                 // magenta screen
    while (true) delay(1000);
  }
  applyMode(mode);
}

// Returns 0 = nothing, 1 = short press, 2 = long press (reported on release)
int readButton() {
  static bool down = false;
  static uint32_t tDown = 0;
  bool pressed = (digitalRead(BTN_PIN) == LOW);
  if (pressed && !down) { down = true; tDown = millis(); }
  if (!pressed && down) {
    down = false;
    uint32_t held = millis() - tDown;
    if (held > 800) return 2;
    if (held > 30)  return 1;
  }
  return 0;
}

void drawStatus(float fps) {
  memset(stripBuf, 0, sizeof(stripBuf));
  rectBuf(4, 2, 16, 16, MODE_COLORS[mode]);                 // mode square
  int f = (int)(fps + 0.5f);
  if (f > 99) f = 99;
  uint16_t txt = (mode == DAY) ? 0xFFFF : 0x07E0;
  int x = 28;
  if (f >= 10) { glyphBuf(x, 2, f / 10, 3, txt); x += 12; }
  glyphBuf(x, 2, f % 10, 3, txt);
  for (int i = 0; i < 4; i++)                               // backlight bars
    rectBuf(196 + i * 11, 3, 8, 14, (i < 4 - blIndex) ? 0xFFFF : 0x2104);
  lcdPush(0, 0, SCR_W, STRIP_H, stripBuf);
}

void loop() {
  static uint32_t frames = 0, t0 = millis();

  int b = readButton();
  if (b == 1) { mode = (Mode)((mode + 1) % MODE_COUNT); applyMode(mode); }
  if (b == 2) { blIndex = (blIndex + 1) % 4; backlightSet(BL_LEVELS[blIndex]); }

  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) return;

  if (fb->width <= SCR_W && fb->height <= (SCR_H - STRIP_H)) {
#if SWAP_BYTES
    for (size_t i = 0; i + 1 < fb->len; i += 2) { uint8_t t = fb->buf[i]; fb->buf[i] = fb->buf[i + 1]; fb->buf[i + 1] = t; }
#endif
    uint16_t x0 = (SCR_W - fb->width) / 2;
    uint16_t y0 = STRIP_H + ((SCR_H - STRIP_H) - fb->height) / 2;
    lcdPush(x0, y0, fb->width, fb->height, fb->buf);
  }
  esp_camera_fb_return(fb);

  frames++;
  if (millis() - t0 >= 1000) {
    float fps = frames * 1000.0f / (millis() - t0);
    Serial.printf("mode %d  %.1f fps  backlight step %d\n", (int)mode, fps, blIndex);
    drawStatus(fps);
    frames = 0;
    t0 = millis();
  }
}
