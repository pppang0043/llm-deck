#include "usb_drive.h"

#include <SD.h>
#include <USB.h>
#include <USBMSC.h>
#include <esp_attr.h>
#include <lvgl.h>

#include "camera_ui.h"
#include "gallery.h"
#include "soc/rtc_cntl_reg.h"
#include "storage.h"

LV_FONT_DECLARE(font_ko_14);
LV_FONT_DECLARE(font_ko_20);

#define MAGIC 0x55534244  // "USBD"
// 재시작해도 남는 곳. 켤 때 USB 드라이브 모드였는지 본다 (전원을 새로 켜면 값이 흐트러져 MAGIC 이 아니다)
static RTC_NOINIT_ATTR uint32_t was_drive;

static USBMSC msc;
static bool active = false, started = false, confirm_exit = false;
static volatile bool ejected = false;
static uint32_t sectors = 0;
static lv_obj_t *screen = nullptr, *state_lbl = nullptr, *exit_lbl = nullptr;

// PC 가 카드를 읽고 쓴다 (USB 태스크에서 불린다). 화면과 같은 SPI 선이지만 SD 라이브러리가 선 잠금을 잡는다.
static int32_t on_read(uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize) {
  if (offset) return -1;  // 장(512바이트) 단위로만 온다
  for (uint32_t done = 0; done < bufsize; done += 512, lba++)
    if (!SD.readRAW((uint8_t *)buffer + done, lba)) return -1;
  return bufsize;
}

static int32_t on_write(uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize) {
  if (offset) return -1;
  for (uint32_t done = 0; done < bufsize; done += 512, lba++)
    if (!SD.writeRAW(buffer + done, lba)) return -1;
  return bufsize;
}

static bool on_start_stop(uint8_t power_condition, bool start, bool load_eject) {
  if (load_eject && !start) ejected = true;  // PC 에서 꺼내기
  return true;
}

bool usb_drive_boot() {
  if (was_drive != MAGIC) return false;
  was_drive = 0;
  // USB 를 저장장치(OTG) 쪽으로 돌려 둔 설정은 RTC 쪽이라 재시작해도 남는다. 시리얼(USB-Serial-JTAG)로 되돌린다.
  CLEAR_PERI_REG_MASK(RTC_CNTL_USB_CONF_REG, RTC_CNTL_SW_HW_USB_PHY_SEL | RTC_CNTL_SW_USB_PHY_SEL | RTC_CNTL_USB_PAD_ENABLE);
  return true;
}

static void exit_clicked(lv_event_t *) {
  // PC 에서 꺼내지 않고 끝내면 쓰던 파일이 망가질 수 있어 한 번 더 묻는다
  if (started && USB && !ejected && !confirm_exit) {
    confirm_exit = true;
    lv_label_set_text(exit_lbl, "꺼내지 않았습니다. 한 번 더 누르면 끝냅니다");
    return;
  }
  Serial.println("[drive] restarting to leave USB drive mode");
  delay(100);
  esp_restart();
}

static void build_screen() {
  screen = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(screen);
  lv_obj_set_size(screen, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(screen, lv_color_hex(0x0E141A), 0);
  lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_all(screen, 16, 0);
  lv_obj_set_style_pad_row(screen, 12, 0);
  lv_obj_add_flag(screen, LV_OBJ_FLAG_CLICKABLE);  // 아래 화면을 누르지 못하게
  lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
  lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(screen, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  lv_obj_t *title = lv_label_create(screen);
  lv_label_set_text(title, "USB 드라이브");
  lv_obj_set_style_text_font(title, &font_ko_20, 0);
  lv_obj_set_style_text_color(title, lv_color_hex(0xE9EDF1), 0);

  state_lbl = lv_label_create(screen);
  lv_obj_set_width(state_lbl, LV_PCT(100));
  lv_label_set_long_mode(state_lbl, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_align(state_lbl, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_font(state_lbl, &font_ko_14, 0);
  lv_obj_set_style_text_color(state_lbl, lv_color_hex(0xB8D4F0), 0);
  lv_label_set_text(state_lbl, "");

  lv_obj_t *note = lv_label_create(screen);
  lv_obj_set_width(note, LV_PCT(100));
  lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_align(note, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_font(note, &font_ko_14, 0);
  lv_obj_set_style_text_color(note, lv_color_hex(0x8A939E), 0);
  lv_label_set_text(note, "끝내기 전에 PC 에서 먼저 꺼내기(추출)를 해 주세요.\n"
                          "그동안 PC 도우미는 블루투스·Wi-Fi 로 이어집니다.");

  lv_obj_t *exit = lv_btn_create(screen);
  lv_obj_remove_style_all(exit);
  lv_obj_set_size(exit, LV_PCT(100), 52);
  lv_obj_set_style_bg_color(exit, lv_color_hex(0x1F3045), 0);
  lv_obj_set_style_bg_opa(exit, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(exit, lv_color_hex(0x2B4560), LV_STATE_PRESSED);
  lv_obj_set_style_radius(exit, 8, 0);
  exit_lbl = lv_label_create(exit);
  lv_obj_set_width(exit_lbl, LV_PCT(90));
  lv_label_set_long_mode(exit_lbl, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_align(exit_lbl, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_font(exit_lbl, &font_ko_14, 0);
  lv_obj_set_style_text_color(exit_lbl, lv_color_hex(0xB8D4F0), 0);
  lv_label_set_text(exit_lbl, "끝내고 다시 시작");
  lv_obj_center(exit_lbl);
  lv_obj_add_event_cb(exit, exit_clicked, LV_EVENT_CLICKED, nullptr);
}

const char *usb_drive_start() {
  if (active) return nullptr;
  if (camera_ui_open() || gallery_open()) return "카메라와 갤러리를 닫은 뒤에 켜 주세요";
  if (!storage_lend()) return "SD 카드가 없거나 쓰는 중입니다";
  active = true;
  sectors = SD.numSectors();
  was_drive = MAGIC;  // 이제부터는 어떻게 다시 켜지든 USB 를 시리얼로 되돌린다
  USB.VID(0x303A);
  USB.PID(0x4002);  // Espressif TinyUSB 저장장치 번호. 평소 시리얼(0x1001)과 달라야 PC 가 헷갈리지 않는다
  USB.productName("LLM Deck SD");
  USB.manufacturerName("LLM Deck");
  msc.vendorID("LLMDeck");
  msc.productID("SD card");
  msc.productRevision("1.0");
  msc.onRead(on_read);
  msc.onWrite(on_write);
  msc.onStartStop(on_start_stop);
  msc.mediaPresent(true);
  started = msc.begin(sectors, SD.sectorSize()) && USB.begin();
  Serial.printf("[drive] USB drive %s, %lu sectors\n", started ? "on" : "failed", (unsigned long)sectors);
  build_screen();
  usb_drive_loop();
  return nullptr;
}

bool usb_drive_active() {
  return active;
}

void usb_drive_loop() {
  static uint32_t shown_ms = 0;
  if (!active || millis() - shown_ms < 500) return;
  shown_ms = millis();
  char t[96];
  if (!started) {
    snprintf(t, sizeof(t), "USB 를 바꾸지 못했습니다.\n다시 시작해 주세요.");
  } else if (ejected) {
    snprintf(t, sizeof(t), "PC 에서 꺼냈습니다. 이제 끝내도 됩니다.");
  } else if (USB) {
    snprintf(t, sizeof(t), "PC 에 연결됨 · 카드 %lu MB", (unsigned long)(sectors / 2048));
  } else {
    snprintf(t, sizeof(t), "PC 연결을 기다리는 중 (USB 케이블)");
  }
  if (strcmp(lv_label_get_text(state_lbl), t) != 0) lv_label_set_text(state_lbl, t);
}
