/*
 * T-Display-S3（原版, ST7789V 170x320）— 精緻時鐘 v2
 * 漸層背景 + Sprite 快存局部更新（零閃爍）+ 低視覺重量排版
 *
 * 依賴: TFT_eSPI(Setup206)、core 3.x。同資料夾 bg_gradient.h 已在。
 * PSRAM 開/關都能跑: 有 PSRAM 用 16-bit sprite 大面板; 無則直接畫螢幕局部(字級縮小)。
 * 端序已逐行核對 TFT_eSPI_ESP32_S3.c 原始碼:
 *   - pushSprite() 內部強制 swap=false, sprite RAM 為 native LE -> 顏色正確。
 *   - 背景 bitmap 為 big-endian RGB565 -> drawBackground 必須 setSwapBytes(true).
 *   - 實測照片: 直接畫螢幕的低亮度 UI 色(面板/邊框/底條)呈 R/B 交換的症狀
 *     (近黑藍變成中藍), 白色/琥珀不受影響 -> 這些色已預先做 R/B 交換, 線上剛好還原.
 *
 * 按鍵: BOOT(GPIO0)=切橫/直向, IO14=循環調背光。
 */

#include <WiFi.h>
#include <TFT_eSPI.h>
#include "bg_gradient.h"          // 170x320 RGB565 big-endian 漸層（make_bg.py 生成）

#define PWR_ON 15
#define BTN_A  0
#define BTN_B  14
#define TFT_BL_PIN 38

TFT_eSPI tft = TFT_eSPI();
TFT_eSprite face = TFT_eSprite(&tft);   // 時鐘面板快存
// true: 16-bit sprite 大面板(需 PSRAM OPI On); false: 薄面板直接畫螢幕局部.
// 在 setup() 最開頭偵測(static init 階段呼叫不穩), 這裡只給安全預設值
bool usePsram = false;

// ⚠ 開給別人看時這兩行會暴露你的 Wi-Fi 名稱與密碼
#ifndef WIFI_SSID
#define WIFI_SSID "TP-Link_595A"   // 可在 Arduino IDE 加 -DWIFI_SSID=\"xxx\" 覆蓋
#endif
#ifndef WIFI_PASS
#define WIFI_PASS "10811114"
#endif


// 調色盤: 深底、低飽和、少顏色 — 「精緻」靠克制不靠多色
#define C_TITLE  0x42CF   // 灰藍小字 rgb(70,90,120)
#define C_LINE   0x49A5   // 面板細邊框（比背景亮一階）
#define C_PANEL  0x1861   // 面板底（比背景再暗一階）
#define C_HOUR   0xF7BF   // 近白: 時
#define C_MIN    0xC67B   // 銀藍: 分
#define C_COLON  0x42F0   // 淡藍圓點冒號
#define C_SEC    0xECE7   // 琥珀: 秒
#define C_DATE   0x7C33   // 冷灰: 日期
#define C_BAR    0x0020   // 頂/底列底色 = RGB(0,0,8): 意圖近黑藍, 線上 RB 交換後呈極暗藍
#define C_OK     0x3DAD   // Wi-Fi 綠

bool landscape = true;
uint32_t blPct = 100;

void setBacklight(uint32_t pct) {
  static bool attached = false;
  if (!attached) { ledcAttach(TFT_BL_PIN, 20000, 8); attached = true; } // 20kHz: 無頻閃
  ledcWrite(TFT_BL_PIN, map(pct, 0, 100, 0, 255));
}

// ---- 背景（只在開機/轉方向時畫一次）----
uint16_t bgAt(int x, int y) {              // 直向原始座標 -> RGB565
  const uint8_t *p = bgBitmap + (y * BG_W + x) * 2;
  return (p[0] << 8) | p[1];
}

void drawBackground() {
  int w = tft.width(), h = tft.height();
  static uint16_t line[320];
  uint8_t rot = tft.getRotation();
  tft.setSwapBytes(true);                  // big-endian 資料 + swap=true = 線上順序正確（已核對 pushPixels/tft_Write_16 原始碼）
  for (int sy = 0; sy < h; sy++) {
    for (int sx = 0; sx < w; sx++) {
      int ox, oy;                          // 螢幕座標 -> 直向原始座標
      switch (rot) {
        case 1:  ox = sy;              oy = BG_H - 1 - sx; break;  // 橫向 CW
        case 3:  ox = BG_W - 1 - sy;  oy = sx;             break;  // 橫向 CCW
        case 2:  ox = BG_W - 1 - sx;  oy = BG_H - 1 - sy; break;  // 直向 180
        default: ox = sx;              oy = sy;            break;  // 直向
      }
      line[sx] = bgAt(ox, oy);
    }
    tft.pushImage(0, sy, w, 1, line);
  }
}

// ---- 時鐘面板（畫在 sprite 快存裡, 內容變化才搬上螢幕）----
int FW, FH, FX, FY;                        // 面板尺寸與螢幕位置
int tszFor()  { return landscape ? (usePsram ? 6 : 4) : (usePsram ? 5 : 4); }
int gapFor()  { int g = 32 - (tszFor() == 6 ? 0 : tszFor() == 5 ? 6 : 12); return g < 18 ? 18 : g; }
char lastHM[6] = ""; int lastSec = -1; char lastDate[24] = "";
int secX = 0, secY = 0;

// GLCD 的 ':' 只有 1px 寬兩個小點, 視覺太弱; 手動畫隨字級放大的圓點
// x0=HH:MM 行起點, cw=單字元寬, gap=HH/MM 間距 -> 兩段空隙的正中
void drawColon(int x0, int cw, int gap, int y0, int h) {
  int cx2 = x0 + 2 * cw + gap / 2;                   // HH 與 MM 空隙的正中(冒號应在這裡)
  int d = max(4, h / 8), r = d / 2, rr = min(3, r);
  face.fillRoundRect(cx2 - r, y0 + (h * 5) / 16 - r, d, d, rr, C_COLON);   // 對齊字型 ':' 的兩點(row2.5/row4.5)
  face.fillRoundRect(cx2 - r, y0 + (h * 9) / 16 - r, d, d, rr, C_COLON);
}

void drawTimeRow() {                       // 重繪 HH:MM（每分鐘一次）
  time_t now = time(nullptr); tm *lt = localtime(&now);
  char hh[3], mm[3];
  snprintf(hh, 3, "%02d", lt->tm_hour); snprintf(mm, 3, "%02d", lt->tm_min);

  face.fillRoundRect(1, 1, FW - 2, FH - 2, 10, C_PANEL);   // 清面板(保留外框)
  face.drawRoundRect(0, 0, FW, FH, 12, C_LINE);

  int tsz = tszFor();
  face.setTextSize(tsz);
  int cwBig = tsz * 5 + (tsz - 1);
  int gap = gapFor();                      // HH 與 MM 之間距, 中央放冒號
  int rowW = 4 * cwBig + gap;
  int x = (FW - rowW) / 2, y = (FH - tsz * 8) / 2;

  face.setTextColor(C_HOUR); face.setCursor(x, y);                    face.print(hh);
  face.setTextColor(C_MIN);  face.setCursor(x + 2 * cwBig + gap, y);  face.print(mm);
  drawColon(x, cwBig, gap, y, tsz * 8);

  // 秒數預留區: 面板右下角小字（僅 PSRAM 大面板; 否則秒數畫在螢幕上）
  if (usePsram) {
    face.setTextSize(2);
    int scw = 2 * 5 + 1;
    secX = FW - 14 - 2 * scw; secY = FH - 20;
    face.fillRect(secX, secY, 2 * scw, 16, C_PANEL);
  }

  snprintf(lastHM, sizeof(lastHM), "%02d%02d", lt->tm_hour, lt->tm_min);
  lastSec = -1;                            // 強制重畫秒
}

int sTx = 0, sTy = 0;                      // 無 PSRAM 模式: 秒數在螢幕上的位置

void drawSeconds() {                       // 每秒只蓋 SS 兩個字元（快存內或螢幕局部）
  time_t now = time(nullptr); tm *lt = localtime(&now);
  if (lt->tm_sec == lastSec) return;
  char sb[3]; snprintf(sb, 3, "%02d", lt->tm_sec);
  if (usePsram) {
    face.setTextSize(2);
    face.setTextColor(C_SEC); face.setCursor(secX, secY); face.print(sb);
  } else {                                 // 直接畫在螢幕: 先算好位置(跟 HH:MM 行尾對齊)
    int tsz = tszFor(), cwBig = tsz * 5 + (tsz - 1);
    int rowW = 4 * cwBig + gapFor();
    int x = FX + (FW - rowW) / 2, y = FY + (FH - tsz * 8) / 2;
    sTx = x + rowW + 6; sTy = y + tsz * 8 - 16;
    tft.fillRect(sTx, sTy, 2 * 11 + 2, 17, C_PANEL);
    tft.setTextSize(2);
    tft.setTextColor(C_SEC, C_PANEL);
    tft.setCursor(sTx, sTy); tft.print(sb);
  }
  lastSec = lt->tm_sec;
}

void pushFace() { face.pushSprite(FX, FY); }   // 整塊一次搬上螢幕（內部處理端序）

void drawDateLine() {                      // 面板下方日期: 每天才重畫
  int w = tft.width();
  time_t now = time(nullptr); tm *lt = localtime(&now);
  static const char *WD[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
  char date[24]; snprintf(date, sizeof(date), "%04d.%02d.%02d  %s",
      lt->tm_year + 1900, lt->tm_mon + 1, lt->tm_mday, WD[lt->tm_wday]);
  if (!strcmp(date, lastDate)) return;
  int y = FY + FH + 8, dh = 20;
  tft.fillRect(0, y, w, dh, C_BAR);        // 低亮度底條
  tft.setTextSize(2);
  tft.setTextColor(C_DATE);
  tft.setCursor((w - (int)strlen(date) * 12) / 2, y + 3);
  tft.print(date);
  strcpy(lastDate, date);
}

void drawHeader() {                        // 頂部小字 + Wi-Fi 指示點
  int w = tft.width();
  tft.fillRect(0, 0, w, 18, C_BAR);
  tft.setTextSize(1);
  tft.setTextColor(C_TITLE);
  tft.setCursor(6, 5);
  tft.print("T-DISPLAY S3");
  bool on = (WiFi.status() == WL_CONNECTED && time(nullptr) > 100000);
  tft.fillCircle(w - 10, 9, 3, on ? C_OK : TFT_RED);
}

void fullRedraw() {
  int w = tft.width(), h = tft.height();
  drawBackground();
  FW = w - 24;                                             // 面板尺寸(兩種模式同寬)
  FH = usePsram ? (landscape ? 118 : 96)                   // PSRAM: 大面板(sprite)
                : (landscape ? 52  : 44);                  // 無 PSRAM: 薄面板(直接畫螢幕)
  FX = 12;     FY = (h - FH) / 2 + 8;
  if (usePsram) {
    face.setColorDepth(16);
    if (face.created()) face.deleteSprite();
    if (!face.createSprite(FW, FH)) while (true) delay(100);   // OOM: 停在黑畫面
    face.setSwapBytes(true);
    face.fillSprite(TFT_BLACK);
    face.fillRoundRect(0, 0, FW, FH, 12, C_PANEL);
    face.drawRoundRect(0, 0, FW, FH, 12, C_LINE);
  } else {                                                 // 無 sprite: 面板直接畫在螢幕上
    tft.fillRoundRect(FX, FY, FW, FH, 12, C_PANEL);
    tft.drawRoundRect(FX, FY, FW, FH, 12, C_LINE);
  }
  lastHM[0] = lastDate[0] = '\0'; lastSec = -1;
}

void setup() {
  usePsram = psramFound();                               // PSRAM 開關決定精緻度(見頂部註解)
  pinMode(PWR_ON, OUTPUT); digitalWrite(PWR_ON, HIGH);   // ← 關鍵: 週邊電源
  pinMode(BTN_A, INPUT_PULLUP);
  pinMode(BTN_B, INPUT_PULLUP);

  tft.init();
  tft.setRotation(1);
  setBacklight(blPct);
  fullRedraw();
  drawHeader();

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  for (int i = 0; i < 60 && WiFi.status() != WL_CONNECTED; i++) delay(500);
  if (WiFi.status() == WL_CONNECTED)
    configTime(8 * 3600, 0, "pool.ntp.org", "time.google.com");  // 台北 UTC+8
}

void loop() {
  static uint32_t lastBtn = 0;
  uint32_t ms = millis();
  if (ms - lastBtn > 250) {                 // 按鍵去抖
    if (!digitalRead(BTN_A)) {
      landscape = !landscape;
      if (usePsram) face.deleteSprite();
      tft.setRotation(landscape ? 1 : 3);
      fullRedraw();                                    // 重畫背景+面板; last* 已重置, 內容自動補畫
      lastBtn = ms;
    }
    if (!digitalRead(BTN_B)) {
      blPct = (blPct >= 100) ? 20 : min<uint32_t>(blPct + 20, 100);
      setBacklight(blPct);
      lastBtn = ms;
    }
  }

  time_t now = time(nullptr);
  if (now < 100000) { delay(500); return; }               // NTP 未校時: 不動畫

  tm *lt = localtime(&now);
  char hm[6]; snprintf(hm, sizeof(hm), "%02d%02d", lt->tm_hour, lt->tm_min);
  bool dirty = false;
  if (usePsram) {
    if (strcmp(hm, lastHM) != 0) { drawTimeRow(); dirty = true; }   // 分鐘級: 重繪列
    drawSeconds();                                     // 秒級: 只蓋 SS(快存內)
  } else {
    if (strcmp(hm, lastHM) != 0) {                     // 無 PSRAM: HH:MM 直接畫螢幕(每分鐘一次)
      int tsz = tszFor(), cwBig = tsz * 5 + (tsz - 1), gap = gapFor();
      int rowW = 4 * cwBig + gap;
      int x = FX + (FW - rowW) / 2, y = FY + (FH - tsz * 8) / 2;
      tft.fillRect(x - 2, y - 2, rowW + 4, tsz * 8 + 4, C_PANEL);
      tft.setTextSize(tsz);
      char hh[3], mm[3]; snprintf(hh,3,"%02d",lt->tm_hour); snprintf(mm,3,"%02d",lt->tm_min);
      tft.setTextColor(C_HOUR, C_PANEL);  tft.setCursor(x, y); tft.print(hh);
      tft.setTextColor(C_MIN, C_PANEL);   tft.setCursor(x + 2 * cwBig + gap, y); tft.print(mm);
      int h = tsz * 8, cx2 = x + 2 * cwBig + gap / 2;        // 圓點冒號(直接畫螢幕, 同 drawColon 公式)
      int d = max(4, h / 8), r = d / 2, rr = min(3, r);
      tft.fillRoundRect(cx2 - r, y + (h * 5) / 16 - r, d, d, rr, C_COLON);   // 同上: 對齊字型 ':' 兩點
      tft.fillRoundRect(cx2 - r, y + (h * 9) / 16 - r, d, d, rr, C_COLON);
      strcpy(lastHM, hm); lastSec = -1; dirty = true;
    }
    drawSeconds();                                     // 秒級: 只蓋 SS(螢幕局部)
  }
  static int pushedSec = -1;
  if (usePsram && (dirty || lt->tm_sec != pushedSec)) { pushFace(); pushedSec = lt->tm_sec; }

  drawDateLine();

  static uint32_t lastHdr = 0;
  if (ms - lastHdr > 5000) { drawHeader(); lastHdr = ms; }         // Wi-Fi 點每 5s
  delay(100);
}
