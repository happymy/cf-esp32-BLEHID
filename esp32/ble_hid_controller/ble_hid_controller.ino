/*
 * ESP32 BLE HID 遥控器 (接收端)
 * 
 * 功能:
 *  连接 Cloudflare Worker WebSocket，接收指令，
 *  通过 NimBLE 模拟蓝牙键盘/鼠标控制手机。
 * 
 * 依赖库 (Arduino IDE 库管理器安装):
 *  - NimBLE-Arduino      by h2zero
 *  - ArduinoJson         by Benoit Blanchon
 *  - arduinoWebSockets   by Markus Sattler (Links2004)
 * 
 * 配置:
 *  修改下方 WIFI_SSID / WIFI_PASS / WS_HOST 三个宏。
 */

#include <NimBLEDevice.h>
#include <NimBLEServer.h>
#include <NimBLEService.h>
#include <NimBLEAdvertising.h>
#include <NimBLEHIDDevice.h>
#include <WiFi.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>

// ==================== 用户配置 ====================
const char* WIFI_SSID = "YOUR_WIFI_SSID";
const char* WIFI_PASS = "YOUR_WIFI_PASSWORD";
const char* WS_HOST  = "your-worker.workers.dev";   // Cloudflare Worker 域名
const int   WS_PORT  = 443;
const char* WS_PATH  = "/ws";

// ==================== BLE HID 常量 ====================
#define BLE_DEVICE_NAME    "ESP32 BLE HID"
#define REPORT_ID_KEYBOARD 1
#define REPORT_ID_MOUSE    2
#define REPORT_ID_CONSUMER 3

// 消费者控制码 (Usage Page 0x0C)
#define AC_HOME      0x0223
#define AC_BACK      0x0224
#define AC_TASK_VIEW 0x0296   // 部分 Android 设备支持

// HID 报告描述符 (键盘 + 鼠标 + 消费者控制)
static const uint8_t HID_REPORT_MAP[] PROGMEM = {

  // ---- 鼠标 (Report ID 2) ----
  0x05, 0x01,        // Usage Page (Generic Desktop)
  0x09, 0x02,        // Usage (Mouse)
  0xA1, 0x01,        // Collection (Application)
  0x85, 0x02,        //   Report ID (2)
  0x09, 0x01,        //   Usage (Pointer)
  0xA1, 0x00,        //   Collection (Physical)
  0x05, 0x09,        //     Usage Page (Button)
  0x19, 0x01,        //     Usage Minimum (1)
  0x29, 0x03,        //     Usage Maximum (3)
  0x15, 0x00,        //     Logical Minimum (0)
  0x25, 0x01,        //     Logical Maximum (1)
  0x95, 0x03,        //     Report Count (3)
  0x75, 0x01,        //     Report Size (1)
  0x81, 0x02,        //     Input (Data,Var,Abs)       - buttons
  0x95, 0x01,        //     Report Count (1)
  0x75, 0x05,        //     Report Size (5)
  0x81, 0x01,        //     Input (Const)              - padding
  0x05, 0x01,        //     Usage Page (Generic Desktop)
  0x09, 0x30,        //     Usage (X)
  0x09, 0x31,        //     Usage (Y)
  0x09, 0x38,        //     Usage (Wheel)
  0x15, 0x81,        //     Logical Minimum (-127)
  0x25, 0x7F,        //     Logical Maximum (127)
  0x75, 0x08,        //     Report Size (8)
  0x95, 0x03,        //     Report Count (3)
  0x81, 0x06,        //     Input (Data,Var,Rel)       - x, y, wheel
  0xC0,              //   End Collection
  0xC0,              // End Collection

  // ---- 键盘 (Report ID 1) ----
  0x05, 0x01,        // Usage Page (Generic Desktop)
  0x09, 0x06,        // Usage (Keyboard)
  0xA1, 0x01,        // Collection (Application)
  0x85, 0x01,        //   Report ID (1)
  0x05, 0x07,        //   Usage Page (Keyboard)
  0x19, 0xE0,        //   Usage Minimum (224)          - Left Ctrl
  0x29, 0xE7,        //   Usage Maximum (231)          - Right GUI
  0x15, 0x00,        //   Logical Minimum (0)
  0x25, 0x01,        //   Logical Maximum (1)
  0x75, 0x01,        //   Report Size (1)
  0x95, 0x08,        //   Report Count (8)
  0x81, 0x02,        //   Input (Data,Var,Abs)         - modifier byte
  0x95, 0x01,        //   Report Count (1)
  0x75, 0x08,        //   Report Size (8)
  0x81, 0x01,        //   Input (Const)                - reserved byte
  0x95, 0x05,        //   Report Count (5)
  0x75, 0x01,        //   Report Size (1)
  0x05, 0x08,        //   Usage Page (LEDs)
  0x19, 0x01,        //   Usage Minimum (1)
  0x29, 0x05,        //   Usage Maximum (5)
  0x91, 0x02,        //   Output (Data,Var,Abs)        - LED report
  0x95, 0x01,        //   Report Count (1)
  0x75, 0x03,        //   Report Size (3)
  0x91, 0x01,        //   Output (Const)               - padding
  0x95, 0x06,        //   Report Count (6)             - up to 6 keys
  0x75, 0x08,        //   Report Size (8)
  0x15, 0x00,        //   Logical Minimum (0)
  0x25, 0x65,        //   Logical Maximum (101)
  0x05, 0x07,        //   Usage Page (Keyboard)
  0x19, 0x00,        //   Usage Minimum (0)
  0x29, 0x65,        //   Usage Maximum (101)
  0x81, 0x00,        //   Input (Data,Array)
  0xC0,              // End Collection

  // ---- 消费者控制 (Report ID 3) ----
  0x05, 0x0C,        // Usage Page (Consumer Devices)
  0x09, 0x01,        // Usage (Consumer Control)
  0xA1, 0x01,        // Collection (Application)
  0x85, 0x03,        //   Report ID (3)
  0x15, 0x01,        //   Logical Minimum (1)
  0x26, 0x9C, 0x02,  //   Logical Maximum (668)
  0x19, 0x01,        //   Usage Minimum (1)
  0x2A, 0x9C, 0x02,  //   Usage Maximum (668)
  0x75, 0x10,        //   Report Size (16)
  0x95, 0x01,        //   Report Count (1)
  0x81, 0x00,        //   Input (Data,Array,Abs)
  0xC0               // End Collection
};

// ==================== 全局对象 ====================
NimBLEServer*         pServer       = nullptr;
NimBLEHIDDevice*      pHID          = nullptr;
NimBLECharacteristic* pInputReport  = nullptr;

WebSocketsClient webSocket;

bool bleConnected = false;
bool wsConnected  = false;

// ==================== BLE 连接回调 ====================
// 不使用 override，兼容不同 NimBLE-Arduino 版本
class ServerCB : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* s) {
    bleConnected = true;
    Serial.println("[BLE] 已连接");
    NimBLEDevice::startAdvertising();
  }
  void onDisconnect(NimBLEServer* s) {
    bleConnected = false;
    Serial.println("[BLE] 断开，重新广播...");
    NimBLEDevice::startAdvertising();
  }
};

// ==================== WiFi ====================
bool connectWiFi() {
  Serial.print("[WiFi] 连接中: ");
  Serial.println(WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  int retry = 0;
  while (WiFi.status() != WL_CONNECTED && retry < 40) {
    delay(500);
    Serial.print(".");
    retry++;
  }
  bool ok = (WiFi.status() == WL_CONNECTED);
  if (ok) {
    Serial.println("\n[WiFi] 已连接, IP: " + WiFi.localIP().toString());
  } else {
    Serial.println("\n[WiFi] 连接失败!");
  }
  return ok;
}

// ==================== BLE HID 初始化 ====================
void setupBLE() {
  NimBLEDevice::init(BLE_DEVICE_NAME);
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);
  pServer = NimBLEDevice::createServer();
  pServer->setCallbacks(new ServerCB());

  pHID = new NimBLEHIDDevice(pServer);
  // 使用编译器认可的 setReportMap 方法
  pHID->setReportMap((uint8_t*)HID_REPORT_MAP, sizeof(HID_REPORT_MAP));
  // 使用编译器认可的 getInputReport 方法
  pInputReport  = pHID->getInputReport(0);
  // 使用编译器认可的 setHidInfo 方法
  pHID->setHidInfo(0x00, 0x02);
  // 使用编译器认可的 setManufacturer 方法
  pHID->setManufacturer("ESP32");
  pHID->startServices();

  // 广播参数
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->setAppearance(0x03C0);
  // 使用编译器认可的 getHidService 方法
  adv->addServiceUUID(pHID->getHidService()->getUUID());
  // 注意：setScanResponse 在当前版本可能不可用，已移除
  adv->start();

  Serial.println("[BLE] HID 服务已启动, 等待连接...");
}

// ==================== 底层报告发送 ====================
void sendHIDReport(const uint8_t* data, size_t len) {
  if (!bleConnected) return;
  pInputReport->setValue(data, len);
  pInputReport->notify();
  delayWS(5);
}

void sendKeyboard(uint8_t modifiers, const uint8_t* keys, uint8_t numKeys) {
  uint8_t report[9];
  report[0] = REPORT_ID_KEYBOARD;
  report[1] = modifiers;
  report[2] = 0;
  for (int i = 0; i < 6; i++) report[3 + i] = (i < numKeys) ? keys[i] : 0;
  sendHIDReport(report, 9);
}

void releaseAllKeys() {
  sendKeyboard(0, nullptr, 0);
}

void sendMouse(uint8_t buttons, int8_t x, int8_t y, int8_t wheel) {
  uint8_t report[5];
  report[0] = REPORT_ID_MOUSE;
  report[1] = buttons;
  report[2] = x;
  report[3] = y;
  report[4] = wheel;
  sendHIDReport(report, 5);
}

// 非阻塞延时：在 delay 期间持续处理 WebSocket，防止长时间阻塞导致连接超时断开
void delayWS(unsigned long ms) {
  unsigned long start = millis();
  while (millis() - start < ms) {
    webSocket.loop();
    delay(1);
  }
}

void sendConsumer(uint16_t usage) {
  uint8_t report[3];
  report[0] = REPORT_ID_CONSUMER;
  report[1] = usage & 0xFF;
  report[2] = (usage >> 8) & 0xFF;
  sendHIDReport(report, 3);
  delayWS(50);
  // 立即释放
  uint8_t release[3] = {REPORT_ID_CONSUMER, 0, 0};
  sendHIDReport(release, 3);
}

// ==================== 文字输入 ====================
static uint8_t asciiToHID(char c) {
  if (c >= 'a' && c <= 'z') return c - 'a' + 0x04;
  if (c >= 'A' && c <= 'Z') return c - 'A' + 0x04;
  if (c >= '1' && c <= '9') return c - '1' + 0x1E;
  if (c == '0') return 0x27;
  switch (c) {
    case ' ':  return 0x2C;
    case '\n': case '\r': return 0x28;
    case '-': case '_': return 0x2D;
    case '=': case '+': return 0x2E;
    case '[': case '{': return 0x2F;
    case ']': case '}': return 0x30;
    case '\\': case '|': return 0x31;
    case ';': case ':': return 0x33;
    case '\'': case '"': return 0x34;
    case '`': case '~': return 0x35;
    case ',': case '<': return 0x36;
    case '.': case '>': return 0x37;
    case '/': case '?': return 0x38;
    case '\t': return 0x2B;
    default:   return 0x00;
  }
}

static bool needsShift(char c) {
  if (c >= 'A' && c <= 'Z') return true;
  switch (c) {
    case '!': case '@': case '#': case '$': case '%':
    case '^': case '&': case '*': case '(': case ')':
    case '_': case '+': case '{': case '}': case '|':
    case ':': case '"': case '~': case '<': case '>':
    case '?': return true;
  }
  return false;
}

void typeString(const char* text) {
  for (size_t i = 0; text[i]; i++) {
    uint8_t code = asciiToHID(text[i]);
    if (code == 0) continue;
    uint8_t mod = needsShift(text[i]) ? 0x02 : 0;
    sendKeyboard(mod, &code, 1);
    delayWS(25);
    releaseAllKeys();
    delayWS(15);
  }
}

// ==================== 指令处理 ====================
void handleCommand(const char* json) {
  StaticJsonDocument<512> doc;
  DeserializationError err = deserializeJson(doc, json);
  if (err) {
    Serial.printf("[CMD] JSON 解析失败: %s\n", err.c_str());
    return;
  }

  const char* a = doc["a"];
  if (!a) return;

  Serial.printf("[CMD] %s\n", a);

  switch (a[0]) {
    case 'h':
      sendConsumer(AC_HOME);
      break;

    case 'b':
      sendConsumer(AC_BACK);
      break;

    case 's': {
      sendConsumer(AC_TASK_VIEW);
      delayWS(80);
      uint8_t tab = 0x2B;
      sendKeyboard(0x08, &tab, 1);
      delayWS(120);
      releaseAllKeys();
      break;
    }

    case 'c':
      sendMouse(0x01, 0, 0, 0);
      delayWS(50);
      sendMouse(0x00, 0, 0, 0);
      break;

    case 'm': {
      int dx = doc["x"] | 0;
      int dy = doc["y"] | 0;
      if (dx >  127) dx =  127;
      if (dx < -127) dx = -127;
      if (dy >  127) dy =  127;
      if (dy < -127) dy = -127;
      sendMouse(0x00, (int8_t)dx, (int8_t)dy, 0);
      break;
    }

    case 't': {
      const char* text = doc["d"];
      if (text) typeString(text);
      break;
    }
  }
}

// ==================== WebSocket 事件 ====================
void webSocketEvent(WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_DISCONNECTED:
      Serial.println("[WS] 断开");
      wsConnected = false;
      break;
    case WStype_CONNECTED:
      Serial.println("[WS] 已连接");
      wsConnected = true;
      break;
    case WStype_TEXT:
      handleCommand((char*)payload);
      break;
    case WStype_ERROR:
      Serial.println("[WS] 错误");
      wsConnected = false;
      break;
    default:
      break;
  }
}

// ==================== 主程序 ====================
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n=== ESP32 BLE HID 遥控器 ===");

  if (!connectWiFi()) {
    delay(3000);
    ESP.restart();
  }

  setupBLE();

  webSocket.beginSSL(WS_HOST, WS_PORT, WS_PATH);
  webSocket.onEvent(webSocketEvent);
  webSocket.setReconnectInterval(5000);
  // 注意：setInsecure() 在当前版本可能不可用，若不校验证书，默认行为即跳过
}

void loop() {
  webSocket.loop();

  // 定期检查 WiFi
  static unsigned long lastCheck = 0;
  if (millis() - lastCheck > 15000) {
    lastCheck = millis();
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("[WiFi] 连接丢失，重连...");
      WiFi.reconnect();
    }
  }
}
