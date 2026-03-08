/*
  ESP32-CAM + Edge Impulse
  - UART commands: 'A' = LEFT half -> bottle (B/I/X)
                  'B' = RIGHT half -> can    (C/I/X)
  - UART Serial1 RX=13 TX=15
  - Debug commands also accepted from USB Serial Monitor
  - Prints best label and confidence %
  - Captures RGB565 directly (no fmt2rgb888 crash)
  - NEW: Flash LED (GPIO4) ON during each capture
*/

#include "migs-project-1_inferencing.h"
#include "edge-impulse-sdk/dsp/image/image.hpp"
#include "esp_camera.h"
#include "esp_heap_caps.h"

#define CAMERA_MODEL_AI_THINKER

#if defined(CAMERA_MODEL_AI_THINKER)
  #define PWDN_GPIO_NUM     32
  #define RESET_GPIO_NUM    -1
  #define XCLK_GPIO_NUM      0
  #define SIOD_GPIO_NUM     26
  #define SIOC_GPIO_NUM     27
  #define Y9_GPIO_NUM       35
  #define Y8_GPIO_NUM       34
  #define Y7_GPIO_NUM       39
  #define Y6_GPIO_NUM       36
  #define Y5_GPIO_NUM       21
  #define Y4_GPIO_NUM       19
  #define Y3_GPIO_NUM       18
  #define Y2_GPIO_NUM        5
  #define VSYNC_GPIO_NUM    25
  #define HREF_GPIO_NUM     23
  #define PCLK_GPIO_NUM     22
#else
  #error "Camera model not selected"
#endif

// Raw capture size
#define RAW_W 320
#define RAW_H 240

// UART
static const int UART_RX_PIN = 13;
static const int UART_TX_PIN = 15;
static const uint32_t UART_BAUD = 115200;
static const uint32_t USB_BAUD  = 115200;

// Flash LED on ESP32-CAM AI Thinker is GPIO4
static const int FLASH_PIN = 4;
// how long to keep flash on before grabbing frame (lets exposure settle)
static const int FLASH_PRE_MS  = 40;
// how long to keep flash on after grabbing frame (optional)
static const int FLASH_POST_MS = 5;

static bool debug_nn = false;
static bool is_initialised = false;

// Buffers
static uint8_t* crop_rgb_buf  = nullptr;   // 160x240 RGB888
static uint8_t* infer_rgb_buf = nullptr;   // EI input RGB888

static void* malloc_8bit_psram_preferred(size_t bytes) {
  void* p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!p) p = heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
  return p;
}

// Camera config: RGB565
static camera_config_t camera_config = {
  .pin_pwdn       = PWDN_GPIO_NUM,
  .pin_reset      = RESET_GPIO_NUM,
  .pin_xclk       = XCLK_GPIO_NUM,
  .pin_sscb_sda   = SIOD_GPIO_NUM,
  .pin_sscb_scl   = SIOC_GPIO_NUM,

  .pin_d7         = Y9_GPIO_NUM,
  .pin_d6         = Y8_GPIO_NUM,
  .pin_d5         = Y7_GPIO_NUM,
  .pin_d4         = Y6_GPIO_NUM,
  .pin_d3         = Y5_GPIO_NUM,
  .pin_d2         = Y4_GPIO_NUM,
  .pin_d1         = Y3_GPIO_NUM,
  .pin_d0         = Y2_GPIO_NUM,

  .pin_vsync      = VSYNC_GPIO_NUM,
  .pin_href       = HREF_GPIO_NUM,
  .pin_pclk       = PCLK_GPIO_NUM,

  .xclk_freq_hz   = 20000000,
  .ledc_timer     = LEDC_TIMER_0,
  .ledc_channel   = LEDC_CHANNEL_0,

  .pixel_format   = PIXFORMAT_RGB565,
  .frame_size     = FRAMESIZE_QVGA,   // 320x240
  .jpeg_quality   = 12,               // unused for RGB565
  .fb_count       = 1,
  .fb_location    = CAMERA_FB_IN_PSRAM,
  .grab_mode      = CAMERA_GRAB_WHEN_EMPTY,
};

// EI get_data reads from infer_rgb_buf
static int ei_camera_get_data(size_t offset, size_t length, float *out_ptr) {
  size_t pixel_ix = offset * 3;
  size_t out_ix = 0;

  for (size_t i = 0; i < length; i++) {
    uint8_t r = infer_rgb_buf[pixel_ix];
    uint8_t g = infer_rgb_buf[pixel_ix + 1];
    uint8_t b = infer_rgb_buf[pixel_ix + 2];
    out_ptr[out_ix++] = (r << 16) | (g << 8) | b;
    pixel_ix += 3;
  }
  return 0;
}

static void flash_on() {
  digitalWrite(FLASH_PIN, HIGH);
}

static void flash_off() {
  digitalWrite(FLASH_PIN, LOW);
}

static bool ei_camera_init() {
  if (is_initialised) return true;

  esp_err_t err = esp_camera_init(&camera_config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed 0x%x\n", err);
    return false;
  }

  sensor_t *s = esp_camera_sensor_get();
  if (s && s->id.PID == OV3660_PID) {
    s->set_vflip(s, 1);
    s->set_brightness(s, 1);
    s->set_saturation(s, 0);
  }

  is_initialised = true;
  return true;
}

static void send_uart_char(char c) {
  Serial1.write((uint8_t)c);
}

// RGB565 -> RGB888
static inline void rgb565_to_rgb888(uint16_t p, uint8_t &r, uint8_t &g, uint8_t &b) {
  r = (uint8_t)(((p >> 11) & 0x1F) * 255 / 31);
  g = (uint8_t)(((p >> 5)  & 0x3F) * 255 / 63);
  b = (uint8_t)(((p)       & 0x1F) * 255 / 31);
}

// Capture RGB565 frame, crop half, convert to RGB888
static bool capture_crop_half_to_rgb888(bool left_half) {
  if (!is_initialised) return false;

  // Turn flash ON for capture
  flash_on();
  delay(FLASH_PRE_MS);

  camera_fb_t* fb = esp_camera_fb_get();

  delay(FLASH_POST_MS);
  flash_off();

  if (!fb || !fb->buf) {
    Serial.println("Camera capture failed");
    if (fb) esp_camera_fb_return(fb);
    return false;
  }

  if (fb->format != PIXFORMAT_RGB565) {
    Serial.printf("Unexpected fb format: %d\n", fb->format);
    esp_camera_fb_return(fb);
    return false;
  }

  const int half_w = RAW_W / 2;
  const int x0 = left_half ? 0 : half_w;

  const uint16_t* src = (const uint16_t*)fb->buf;

  for (int y = 0; y < RAW_H; y++) {
    for (int x = 0; x < half_w; x++) {
      uint16_t pix = src[y * RAW_W + (x0 + x)];
      uint8_t r, g, b;
      rgb565_to_rgb888(pix, r, g, b);

      size_t di = (size_t)(y * half_w + x) * 3;
      crop_rgb_buf[di + 0] = r;
      crop_rgb_buf[di + 1] = g;
      crop_rgb_buf[di + 2] = b;
    }
  }

  esp_camera_fb_return(fb);
  return true;
}

// Resize crop -> EI input
static void resize_to_infer() {
  const int crop_w = RAW_W / 2; // 160
  const int crop_h = RAW_H;     // 240

  ei::image::processing::crop_and_interpolate_rgb888(
    crop_rgb_buf,
    crop_w,
    crop_h,
    infer_rgb_buf,
    EI_CLASSIFIER_INPUT_WIDTH,
    EI_CLASSIFIER_INPUT_HEIGHT
  );
}

static char classify_and_respond(bool left_half) {
  if (!capture_crop_half_to_rgb888(left_half)) return 'X';

  resize_to_infer();

  ei::signal_t signal;
  signal.total_length = EI_CLASSIFIER_INPUT_WIDTH * EI_CLASSIFIER_INPUT_HEIGHT;
  signal.get_data = &ei_camera_get_data;

  ei_impulse_result_t result = {0};
  EI_IMPULSE_ERROR err = run_classifier(&signal, &result, debug_nn);
  if (err != EI_IMPULSE_OK) {
    Serial.printf("run_classifier failed: %d\n", err);
    return 'X';
  }

  Serial.printf("Timing: DSP %d ms, Class %d ms\n",
                result.timing.dsp, result.timing.classification);

#if EI_CLASSIFIER_OBJECT_DETECTION == 1
  float best_val = 0.0f;
  int best_ix = -1;
  for (size_t i = 0; i < result.bounding_boxes_count; i++) {
    auto bb = result.bounding_boxes[i];
    if (bb.value > best_val) {
      best_val = bb.value;
      best_ix = (int)i;
    }
  }

  if (best_ix < 0 || best_val <= 0.0f) {
    Serial.println("No objects detected.");
    return 'X';
  }

  auto bb = result.bounding_boxes[best_ix];
  Serial.printf("Best: %s (%.1f%%)\n", bb.label, bb.value * 100.0f);

  // EDIT THESE to match your Edge Impulse labels EXACTLY:
  const char* BOTTLE_LABEL = "bottle";
  const char* CAN_LABEL    = "can";

  if (left_half) {
    return (strcmp(bb.label, BOTTLE_LABEL) == 0) ? 'B' : 'I';
  } else {
    return (strcmp(bb.label, CAN_LABEL) == 0) ? 'C' : 'I';
  }
#else
  float best_val = 0.0f;
  int best_ix = -1;
  for (size_t i = 0; i < EI_CLASSIFIER_LABEL_COUNT; i++) {
    float v = result.classification[i].value;
    if (v > best_val) { best_val = v; best_ix = (int)i; }
  }
  if (best_ix < 0 || best_val <= 0.0f) return 'X';

  Serial.printf("Best class: %s (%.1f%%)\n",
                result.classification[best_ix].label,
                best_val * 100.0f);

  const char* BOTTLE_LABEL = "bottle";
  const char* CAN_LABEL    = "can";

  if (left_half) return (strcmp(result.classification[best_ix].label, BOTTLE_LABEL) == 0) ? 'B' : 'I';
  else           return (strcmp(result.classification[best_ix].label, CAN_LABEL) == 0) ? 'C' : 'I';
#endif
}

static bool read_command_char(char &cmd) {
  if (Serial1.available() > 0) { cmd = (char)Serial1.read(); return true; }
  if (Serial.available() > 0)  { cmd = (char)Serial.read();  return true; }
  return false;
}

void setup() {
  Serial.begin(USB_BAUD);
  Serial1.begin(UART_BAUD, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);

  // Flash pin
  pinMode(FLASH_PIN, OUTPUT);
  flash_off();

  Serial.println("\nBooting...");

  size_t crop_bytes  = (RAW_W / 2) * RAW_H * 3; // 160*240*3 = 115200
  size_t infer_bytes = (size_t)EI_CLASSIFIER_INPUT_WIDTH * EI_CLASSIFIER_INPUT_HEIGHT * 3;

  crop_rgb_buf  = (uint8_t*)malloc_8bit_psram_preferred(crop_bytes);
  infer_rgb_buf = (uint8_t*)malloc_8bit_psram_preferred(infer_bytes);

  Serial.printf("crop_rgb_buf : %p (%u bytes)\n", crop_rgb_buf,  (unsigned)crop_bytes);
  Serial.printf("infer_rgb_buf: %p (%u bytes)\n", infer_rgb_buf, (unsigned)infer_bytes);

  if (!crop_rgb_buf || !infer_rgb_buf) {
    Serial.println("ERR: buffer allocation failed.");
    while (true) delay(1000);
  }

  if (!ei_camera_init()) {
    Serial.println("ERR: Camera init failed.");
    while (true) delay(1000);
  }

  Serial.println("Camera initialized.");
  Serial.println("Send 'A' (LEFT->bottle) or 'B' (RIGHT->can) via UART or Serial Monitor.");
}

void loop() {
  char cmd = 0;
  if (!read_command_char(cmd)) { delay(5); return; }

  if (cmd == '\n' || cmd == '\r' || cmd == ' ') return;

  if (cmd == 'A' || cmd == 'a') {
    Serial.println("\nCMD A: scan LEFT half for bottle...");
    char resp = classify_and_respond(true);
    Serial.printf("Response: %c\n", resp);
    send_uart_char(resp);

  } else if (cmd == 'B' || cmd == 'b') {
    Serial.println("\nCMD B: scan RIGHT half for can...");
    char resp = classify_and_respond(false);
    Serial.printf("Response: %c\n", resp);
    send_uart_char(resp);

  } else {
    Serial.printf("Unknown cmd: %c\n", cmd);
  }
}

#if !defined(EI_CLASSIFIER_SENSOR) || EI_CLASSIFIER_SENSOR != EI_CLASSIFIER_SENSOR_CAMERA
#error "Invalid model for current sensor"
#endif