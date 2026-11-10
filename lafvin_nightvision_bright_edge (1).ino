// Near-IR night vision monocular -- LAFVIN ESP32-S3 1.69" LCD board (OV3660)
// NO EXTRA LIBRARIES: uses only SPI.h and esp_camera.h from the ESP32 core,
// with a small built-in ST7789 driver.
//
// 14 settings in GRAYSCALE and 14 in COLOUR:
//   L1-L4 = low-gain settings for bright / supplemental IR (L1 dimmest)
//   1-10  = night gain levels (10 = the old BOOST setting)
// After the last setting the cycle repeats all of them with EDGE HIGHLIGHTING,
// then goes back to plain again.
//
// BOOT button (GPIO0):
//   short press (under 0.8 s) = next step: L1..L4, 1..10, then the same with edges, ...
//   medium hold (0.8 - 2 s)   = cycle LCD backlight (100% -> 50% -> 15% -> 4%)
//   long hold   (over 2 s)    = switch GRAYSCALE <-> COLOUR
//
// Status strip (top 20 px of the screen):
//   square          = palette (grey = grayscale, orange = colour)
//   first number    = setting L1-L4 or 1-10 (cyan), "E" after it = edge highlighting on
//   second number   = frames per second (green)
//   4 bars (right)  = backlight level
//
// Board settings: ESP32S3 Dev Module, PSRAM = OPI PSRAM.
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
#define IMG_VFLIP    1
#define IMG_HMIRROR  0

// Status LED (WS2812) on the board: GPIO 48 per Lafvin's pin_config.h.
// Set to -1 to leave the LED alone.
#define LED_GPIO 48

// Edge highlighting: a pixel is marked when the brightness changes by more than
// this between its neighbours (scale 0-126). Lower = more edges (and more noise).
#define EDGE_THRESHOLD 8
#define EDGE_THICK     1        // 1 = draw highlights 2 pixels thick, 0 = 1 pixel
#define EDGE_COLOR     0xFFE0   // RGB565 yellow

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
static const uint16_t DIGITS[12] = {0x7B6F, 0x2C97, 0x73E7, 0x73CF, 0x5BC9,
                                    0x79CF, 0x79EF, 0x7249, 0x7BEF, 0x7BCF,
                                    0x79E7 /* 10 = letter E */, 0x4927 /* 11 = letter L */};
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
//  Gain levels (10 grayscale + 10 colour)
// =====================================================================
#define NUM_LEVELS 14
#define NUM_LOW    4     // first 4 entries = L1..L4 (low gain for bright / supplemental IR)

// One row per setting. Order = brightness order, dimmest first.
//   gain   : manual camera gain (0-30)
//   ae     : auto-exposure target (-2 darker .. +2 brighter)
//   bright : brightness (-2..2)
//   aec2   : 1 = sensor night-mode exposure (can slow the frame rate), 0 = off
struct LevelDef { int8_t gain, ae, bright, aec2; };
static const LevelDef LEVELS[NUM_LEVELS] = {
  {  0, -2, -2, 0 },   // L1  (very bright IR scenes)
  {  0, -1, -1, 0 },   // L2
  {  0,  0,  0, 0 },   // L3
  {  3,  1,  1, 0 },   // L4
  {  6,  2,  2, 1 },   // 1
  {  9,  2,  2, 1 },   // 2
  { 11,  2,  2, 1 },   // 3
  { 13,  2,  2, 1 },   // 4
  { 15,  2,  2, 1 },   // 5
  { 18,  2,  2, 1 },   // 6
  { 21,  2,  2, 1 },   // 7
  { 24,  2,  2, 1 },   // 8
  { 27,  2,  2, 1 },   // 9
  { 30,  2,  2, 1 },   // 10 = old BOOST
};

static int  gainLevel  = 8;      // 0-based index into LEVELS, so this is night level 5
static bool colourMode = false;  // false = grayscale, true = colour
static bool edgesOn    = false;  // edge highlighting
static camera_config_t camCfg;   // kept so the camera can be restarted
static float lastFps   = 0;

// Call a sensor function only if this sensor driver implements it.
#define SET(fn, ...) do { if (s->fn) s->fn(s, __VA_ARGS__); } while (0)

void applyLevel() {
  sensor_t *s = esp_camera_sensor_get();
  if (!s) return;

  SET(set_vflip, IMG_VFLIP);
  SET(set_hmirror, IMG_HMIRROR);
  SET(set_lenc, 1);

  if (colourMode) {
    SET(set_special_effect, 0);    // normal colour
    SET(set_whitebal, 1);
    SET(set_awb_gain, 1);
    SET(set_saturation, 0);
  } else {
    SET(set_special_effect, 2);    // grayscale
    SET(set_whitebal, 0);
    SET(set_awb_gain, 0);
    SET(set_saturation, -2);
  }

  const LevelDef &d = LEVELS[gainLevel];
  SET(set_gain_ctrl, 0);                       // manual gain...
  SET(set_agc_gain, d.gain);                   // ...stepped by level
  SET(set_exposure_ctrl, 1);                   // exposure stays automatic
  SET(set_aec2, d.aec2);
  SET(set_ae_level, d.ae);
  SET(set_gainceiling, GAINCEILING_128X);
  SET(set_brightness, d.bright);
  SET(set_contrast, 1);
}

// Full camera restart: clears any stuck exposure/gain state. Used when the gain
// drops back from level 10 to level 1.
void cameraRestart() {
  esp_camera_deinit();
  delay(50);
  if (esp_camera_init(&camCfg) != ESP_OK) {
    lcdFill(0xF81F);                 // magenta screen
    while (true) delay(1000);
  }
}

// ---------- edge highlighting ----------
static uint8_t gRowA[SCR_W], gRowB[SCR_W], gRowC[SCR_W];

static void loadRowG(const uint16_t *row, uint8_t *dst, int w) {
  for (int x = 0; x < w; x++) dst[x] = (__builtin_bswap16(row[x]) >> 5) & 63;   // 6-bit green = brightness
}

// px = big-endian RGB565 frame. Marks pixels where brightness changes sharply.
// Works one row at a time on copies of the ORIGINAL brightness, so painting
// highlights never disturbs the pixels still to be examined.
void drawEdges(uint16_t *px, int w, int h) {
  uint8_t *up = gRowA, *cur = gRowB, *down = gRowC;
  loadRowG(px, cur, w);
  memcpy(up, cur, w);                               // top edge: repeat first row
  for (int y = 0; y < h; y++) {
    if (y + 1 < h) loadRowG(px + (size_t)(y + 1) * w, down, w);
    else memcpy(down, cur, w);                      // bottom edge: repeat last row
    uint16_t *row = px + (size_t)y * w;
    for (int x = 1; x < w - 1; x++) {
      int gx = cur[x + 1] - cur[x - 1];
      int gy = down[x] - up[x];
      if (gx < 0) gx = -gx;
      if (gy < 0) gy = -gy;
      if (gx + gy > EDGE_THRESHOLD) {
        const uint16_t e = __builtin_bswap16(EDGE_COLOR);
        row[x] = e;
#if EDGE_THICK
        row[x + 1] = e;                              // x+1 always exists (loop stops at w-2)
        if (y + 1 < h) { row[w + x] = e; row[w + x + 1] = e; }   // and the row below
#endif
      }
    }
    uint8_t *tmp = up; up = cur; cur = down; down = tmp;
  }
}

// Turn the WS2812 status LED off (needs LED_GPIO set above)
void ledsOff() {
  if (LED_GPIO >= 0) {
    neopixelWrite(LED_GPIO, 0, 0, 0);
    delay(2);
    neopixelWrite(LED_GPIO, 0, 0, 0);
  }
}

// =====================================================================
void setup() {
  Serial.begin(115200);
  pinMode(BTN_PIN, INPUT_PULLUP);

  lcdInit();
  ledsOff();
  backlightInit();
  backlightSet(BL_LEVELS[blIndex]);
  lcdFill(0x0000);

  if (!psramFound()) {
    Serial.println("PSRAM not found - set Tools > PSRAM > OPI PSRAM");
    lcdFill(0xF800);                 // red screen
    while (true) delay(1000);
  }

  camera_config_t &cfg = camCfg;   // global copy, so the camera can be restarted
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
  applyLevel();
  ledsOff();                         // again, in case camera setup disturbed the pin
}

// Returns 0 = nothing, 1 = short press, 2 = medium hold, 3 = long hold
// (reported when the button is released)
int readButton() {
  static bool down = false;
  static uint32_t tDown = 0;
  bool pressed = (digitalRead(BTN_PIN) == LOW);
  if (pressed && !down) { down = true; tDown = millis(); }
  if (!pressed && down) {
    down = false;
    uint32_t held = millis() - tDown;
    if (held > 2000) return 3;
    if (held > 800)  return 2;
    if (held > 30)   return 1;
  }
  return 0;
}

void drawStatus(float fps) {
  memset(stripBuf, 0, sizeof(stripBuf));
  rectBuf(4, 2, 16, 16, colourMode ? 0xFD20 : 0xBDF7);      // palette square

  int x = 28;
  if (gainLevel < NUM_LOW) {                                 // low-gain settings: L1-L4
    glyphBuf(x, 2, 11, 3, 0x07FF);                           // "L"
    glyphBuf(x + 12, 2, gainLevel + 1, 3, 0x07FF);
    x += 24;
  } else {
    int lvl = gainLevel - NUM_LOW + 1;                       // night levels 1-10
    if (lvl >= 10) {
      glyphBuf(x, 2, 1, 3, 0x07FF);
      glyphBuf(x + 12, 2, 0, 3, 0x07FF);
      x += 24;
    } else {
      glyphBuf(x, 2, lvl, 3, 0x07FF);
      x += 12;
    }
  }
  if (edgesOn) { glyphBuf(x + 2, 2, 10, 3, 0xFFE0); x += 12; }   // "E" = edges on
  rectBuf(x + 2, 8, 6, 3, 0x7BEF);                           // separator dash
  x += 14;

  int f = (int)(fps + 0.5f);                                 // frames per second
  if (f > 99) f = 99;
  if (f >= 10) { glyphBuf(x, 2, f / 10, 3, 0x07E0); x += 12; }
  glyphBuf(x, 2, f % 10, 3, 0x07E0);

  for (int i = 0; i < 4; i++)                                // backlight bars
    rectBuf(196 + i * 11, 3, 8, 14, (i < 4 - blIndex) ? 0xFFFF : 0x2104);
  lcdPush(0, 0, SCR_W, STRIP_H, stripBuf);
}

void loop() {
  static uint32_t frames = 0, t0 = millis();

  int b = readButton();
  if (b == 1) {
    gainLevel = (gainLevel + 1) % NUM_LEVELS;
    if (gainLevel == 0) {                 // wrapped round to the start: toggle edges, restart camera
      edgesOn = !edgesOn;
      cameraRestart();
    }
    applyLevel();
  }
  if (b == 2) { blIndex = (blIndex + 1) % 4; backlightSet(BL_LEVELS[blIndex]); }
  if (b == 3) { colourMode = !colourMode; applyLevel(); }
  if (b != 0) drawStatus(lastFps);   // show the change straight away

  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) return;

  if (fb->width <= SCR_W && fb->height <= (SCR_H - STRIP_H)) {
#if SWAP_BYTES
    for (size_t i = 0; i + 1 < fb->len; i += 2) { uint8_t t = fb->buf[i]; fb->buf[i] = fb->buf[i + 1]; fb->buf[i + 1] = t; }
#endif
    if (edgesOn) drawEdges((uint16_t *)fb->buf, fb->width, fb->height);
    uint16_t x0 = (SCR_W - fb->width) / 2;
    uint16_t y0 = STRIP_H + ((SCR_H - STRIP_H) - fb->height) / 2;
    lcdPush(x0, y0, fb->width, fb->height, fb->buf);
  }
  esp_camera_fb_return(fb);

  frames++;
  if (millis() - t0 >= 1000) {
    lastFps = frames * 1000.0f / (millis() - t0);
    Serial.printf("%s setting %d%s  %.1f fps  backlight step %d\n",
                  colourMode ? "colour" : "gray", gainLevel, edgesOn ? " +edges" : "", lastFps, blIndex);
    drawStatus(lastFps);
    frames = 0;
    t0 = millis();
  }
}
