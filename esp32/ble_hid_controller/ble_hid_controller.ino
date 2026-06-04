/*
 * ESP32 BLE HID 遥控器 - C3 v19 (蓝牙状态上报)
 *
 * v19 新增：
 *   - 蓝牙状态上报
 * v18 关键修复：
 *   - [关键] 恢复单输入报告特征 getInputReport(0) 模式：
 *     v16/v17 三特征独立 Report ID 与 BLE HOGP 规范冲突（报告首字节重复 ID → 数据错位）
 *     回到 v15 成熟方案：一个特征承载全部 Report ID (0x01/0x02/0x03)，report 数据首字节区分类型
 *   - [中等] 鼠标坐标 clamp 范围修正为 [-127, 127]，匹配报告描述符 Logical Min/Max
 *
 * v17 修复（保留）：
 *   - 开启 bonding (setSecurityAuth(true, false, true)) → ESP32 重启无需重新配对
 *   - 手机锁屏唤醒后自动重连已绑定设备（手机端主动扫描已绑定设备并恢复加密）
 *   - onAuthenticationComplete 回调记录配对/加密状态
 *
 * v16 修复（保留）：
 *   - pServer->start() 在全部 getInputReport() 之后调用，确保特征注册完整
 *
 * 库依赖（Arduino IDE 内安装）：
 *   NimBLE-Arduino v2.5.0 (h2zero)
 *   ArduinoJson v7.x (Benoit Blanchon)
 *   arduinoWebSockets (Markus Sattler)
 *
 * 开发板：MakerGO ESP32-C3-SUPER-MINI,按需开启 USB CDC On Boot,Partition Scheme = Huge APP
 */

#include <NimBLEDevice.h>
#include <NimBLEServer.h>
#include <NimBLEService.h>
#include <NimBLEAdvertising.h>
#include <NimBLEHIDDevice.h>
#include <NimBLEConnInfo.h>
#include <WiFi.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>

// ==================== 日志控制 ====================
#define VERBOSE_CMD 1

// ==================== 用户配置 ====================
const char* WIFI_SSID = "你的WiFi名";
const char* WIFI_PASS = "你的WiFi密码";
const char* WS_HOST  = "cf-esp32-blehid.你的用户名.workers.dev";   // Cloudflare Worker 域名
const int   WS_PORT  = 443;
const char* WS_PATH  = "/ws?token=你的设备令牌";  // 须与 DEVICE_SECRET 一致

// ==================== BLE HID 常量 ====================
#define BLE_DEVICE_NAME    "ESP32 BLE Remote" //设备名可自定义
#define REPORT_ID_KEYBOARD 1
#define REPORT_ID_MOUSE    2
#define REPORT_ID_CONSUMER 3

#define AC_HOME      0x0223
#define AC_BACK      0x0224
#define AC_TASK_VIEW 0x0296

// 报告描述符（键盘 + 鼠标 + 消费者控制 —— 与 v15 完全一致）
static const uint8_t HID_REPORT_MAP[] PROGMEM = {
  0x05,0x01,0x09,0x02,0xA1,0x01,0x85,0x02,0x09,0x01,
  0xA1,0x00,0x05,0x09,0x19,0x01,0x29,0x03,0x15,0x00,
  0x25,0x01,0x95,0x03,0x75,0x01,0x81,0x02,0x95,0x01,
  0x75,0x05,0x81,0x01,0x05,0x01,0x09,0x30,0x09,0x31,
  0x09,0x38,0x15,0x81,0x25,0x7F,0x75,0x08,0x95,0x03,
  0x81,0x06,0xC0,0xC0,

  0x05,0x01,0x09,0x06,0xA1,0x01,0x85,0x01,0x05,0x07,
  0x19,0xE0,0x29,0xE7,0x15,0x00,0x25,0x01,0x75,0x01,
  0x95,0x08,0x81,0x02,0x95,0x01,0x75,0x08,0x81,0x01,
  0x95,0x05,0x75,0x01,0x05,0x08,0x19,0x01,0x29,0x05,
  0x91,0x02,0x95,0x01,0x75,0x03,0x91,0x01,0x95,0x06,
  0x75,0x08,0x15,0x00,0x25,0x65,0x05,0x07,0x19,0x00,
  0x29,0x65,0x81,0x00,0xC0,

  0x05,0x0C,0x09,0x01,0xA1,0x01,0x85,0x03,0x15,0x01,
  0x26,0x9C,0x02,0x19,0x01,0x2A,0x9C,0x02,0x75,0x10,
  0x95,0x01,0x81,0x00,0xC0
};

// ==================== 全局对象 ====================
NimBLEServer*         pServer       = nullptr;
NimBLEHIDDevice*      pHID          = nullptr;
NimBLECharacteristic* pInputReport  = nullptr;   // v18: 单一输入报告特征 (Report ID 0 → 数据含 Report ID 前缀)
WebSocketsClient      webSocket;

bool bleConnected   = false;
bool bleEncrypted   = false;
bool bleBonded      = false;
bool hidReady       = false;
bool wsConnected    = false;

// 统计
uint32_t g_notifyCount    = 0;
uint32_t g_notifyFail     = 0;
uint32_t g_bleDisconnects = 0;
uint32_t g_cmdCount       = 0;
uint32_t g_cmdSkipped     = 0;

uint32_t g_lastKeepAlive  = 0;

// ==================== BLE 回调（v17: onAuthenticationComplete）====================
class ServerCB : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* s, NimBLEConnInfo& connInfo) override {
    Serial.println("[BLE] Connected");
    bleConnected = true;
  }

  void onDisconnect(NimBLEServer* s, NimBLEConnInfo& connInfo, int reason) override {
    Serial.printf("[BLE] Disconnected (reason=%d), restart advertising\n", reason);
    bleConnected = false;
    bleEncrypted = false;
    bleBonded    = false;
    g_bleDisconnects++;
    NimBLEDevice::startAdvertising();
  }

  void onAuthenticationComplete(NimBLEConnInfo& connInfo) override {
    bleEncrypted = connInfo.isEncrypted();
    bleBonded    = connInfo.isBonded();
    Serial.printf("[SEC] Encryption %s, bonded=%d\n",
      bleEncrypted ? "enabled" : "not enabled",
      bleBonded);
  }
};

// ==================== BLE 初始化 ====================
void setupBLE() {
  Serial.println("[BLE] Init ...");

  NimBLEDevice::init(BLE_DEVICE_NAME);
  Serial.printf("[BLE] MAC: %s\n", NimBLEDevice::getAddress().toString().c_str());

  // v17: 严格参照库示例，开启 bonding
  NimBLEDevice::setSecurityAuth(true, false, true);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
  Serial.println("[BLE] Security: bonding=ON, MITM=OFF, SC=ON (Just Works)");

  pServer = NimBLEDevice::createServer();
  pServer->setCallbacks(new ServerCB());

  pHID = new NimBLEHIDDevice(pServer);
  pHID->setReportMap((uint8_t*)HID_REPORT_MAP, sizeof(HID_REPORT_MAP));
  pHID->setHidInfo(0x00, 0x00);
  pHID->setManufacturer("ESP32-C3");

  // v18: 恢复单输入报告特征，Report ID 0 → 数据首字节区分报告类型 (1/2/3)
  pInputReport = pHID->getInputReport(0);
  hidReady = (pInputReport != nullptr);
  Serial.printf("[BLE] HID ready: %d\n", hidReady);

  // 所有特征创建完毕后启动服务
  pServer->start();

  NimBLEAdvertising* pAdv = NimBLEDevice::getAdvertising();
  pAdv->setName(BLE_DEVICE_NAME);
  pAdv->setAppearance(0x03C0);
  pAdv->addServiceUUID(pHID->getHidService()->getUUID());
  pAdv->start();
  Serial.println("[BLE] Advertising started");
}

// ==================== HID 发送（v18: 统一通过单特征发送，首字节为 Report ID）====================
void sendHIDReport(const uint8_t* data, size_t len) {
  if (!bleConnected || !pInputReport || !hidReady) return;
  pInputReport->setValue(data, len);
  if (pInputReport->notify()) {
    g_notifyCount++;
  } else {
    g_notifyFail++;
  }
  delay(2);
}

void sendKeyboard(uint8_t mod, const uint8_t* keys, uint8_t count) {
  uint8_t r[9] = {REPORT_ID_KEYBOARD, mod, 0};
  for (int i = 0; i < 6; i++) r[3 + i] = (i < count) ? keys[i] : 0;
  sendHIDReport(r, 9);
}

void releaseKeys() {
  uint8_t z[9] = {0};
  z[0] = REPORT_ID_KEYBOARD;
  sendHIDReport(z, 9);
}

void sendMouse(uint8_t btn, int8_t x, int8_t y, int8_t w) {
  uint8_t r[5] = {REPORT_ID_MOUSE, btn, (uint8_t)x, (uint8_t)y, (uint8_t)w};
  sendHIDReport(r, 5);
}

void sendConsumer(uint16_t usage) {
  uint8_t r[3] = {REPORT_ID_CONSUMER, (uint8_t)(usage & 0xFF), (uint8_t)(usage >> 8)};
  sendHIDReport(r, 3);
  delay(40);
  uint8_t z[3] = {REPORT_ID_CONSUMER, 0, 0};  // Usage = 0 表示释放
  sendHIDReport(z, 3);
}

// ==================== 文字输入 ====================
uint8_t asciiToHID(char c) {
  if (c >= 'a' && c <= 'z') return c - 'a' + 4;
  if (c >= 'A' && c <= 'Z') return c - 'A' + 4;
  if (c >= '1' && c <= '9') return c - '1' + 30;
  if (c == '0') return 39;
  switch (c) {
    case ' ': return 44;           case '\n': case '\r': return 40;
    case '-': case '_': return 45; case '=': case '+': return 46;
    case '[': case '{': return 47; case ']': case '}': return 48;
    case '\\': case '|': return 49;case ';': case ':': return 51;
    case '\'': case '"': return 52;case '`': case '~': return 53;
    case ',': case '<': return 54; case '.': case '>': return 55;
    case '/': case '?': return 56; case '\t': return 43;
    default: return 0;
  }
}

bool needsShift(char c) {
  if (c >= 'A' && c <= 'Z') return true;
  switch (c) {
    case '!': case '@': case '#': case '$': case '%': case '^': case '&':
    case '*': case '(': case ')': case '_': case '+': case '{': case '}':
    case '|': case ':': case '"': case '~': case '<': case '>': case '?':
      return true;
  }
  return false;
}

#define MAX_TYPE_STRING_LEN 200
void typeString(const char* text) {
  for (int i = 0; text[i] && i < MAX_TYPE_STRING_LEN; i++) {
    uint8_t code = asciiToHID(text[i]);
    if (!code) continue;
    sendKeyboard(needsShift(text[i]) ? 0x02 : 0, &code, 1);
    delay(25);
    releaseKeys();
    delay(15);
  }
}

// ==================== 指令处理 ====================
void handleCommand(const char* json) {
  StaticJsonDocument<512> doc;
  if (deserializeJson(doc, json)) return;
  const char* a = doc["a"];
  if (!a) return;

  g_cmdCount++;

  #if VERBOSE_CMD
    Serial.printf("[CMD] #%u: %s", g_cmdCount, a);
    if (doc.containsKey("x")) Serial.printf(" x=%d", doc["x"].as<int>());
    if (doc.containsKey("y")) Serial.printf(" y=%d", doc["y"].as<int>());
    if (doc.containsKey("b")) Serial.printf(" btn=%d", doc["b"].as<int>());
    if (doc.containsKey("d")) Serial.printf(" d=\"%.20s\"", doc["d"].as<const char*>());
    Serial.println();
  #endif

  if (!hidReady) { g_cmdSkipped++; return; }
  if (!bleConnected) { g_cmdSkipped++; return; }

  switch (a[0]) {
    case 'h': sendConsumer(AC_HOME); break;
    case 'b': sendConsumer(AC_BACK); break;
    case 's': sendConsumer(AC_TASK_VIEW); delay(80);
              { uint8_t k = 0x2B; sendKeyboard(0x08, &k, 1); delay(120); releaseKeys(); }
              break;
    case 'c': sendMouse(1,0,0,0); delay(50); sendMouse(0,0,0,0); break;
    case 'd': { uint8_t k = 0x2A; sendKeyboard(0, &k, 1); delay(30); releaseKeys(); } break;
    case 'e': { uint8_t k = 0x28; sendKeyboard(0, &k, 1); delay(30); releaseKeys(); } break;
    case 'm': {
      long dx = doc["x"] | 0L, dy = doc["y"] | 0L;
      uint8_t btn = doc["b"].as<uint8_t>();
      // v18: 修正 clamp 范围 [-127, 127]，匹配报告描述符 Logical Min/Max
      if (dx > 127) dx = 127; if (dx < -127) dx = -127;
      if (dy > 127) dy = 127; if (dy < -127) dy = -127;
      sendMouse(btn, (int8_t)dx, (int8_t)dy, 0);
      break;
    }
    case 't': if (doc.containsKey("d")) typeString(doc["d"]); break;
  }
}

// ==================== WebSocket ====================
void sendBleStatus() {
  if (!wsConnected) return;
  StaticJsonDocument<64> doc;
  doc["a"] = "status";
  doc["ble"] = bleConnected;
  String out;
  serializeJson(doc, out);
  webSocket.sendTXT(out);
}

void onWsEvent(WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_DISCONNECTED: wsConnected = false; break;
    case WStype_CONNECTED:
      wsConnected = true;
      Serial.println("[WS] Connected");
      sendBleStatus();
      break;
    case WStype_TEXT: handleCommand((char*)payload); break;
    case WStype_ERROR: wsConnected = false; break;
    default: break;
  }
}

// ==================== WiFi ====================
void connectWiFiBlocking() {
  Serial.print("[WiFi] Connecting to "); Serial.println(WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  int retry = 0;
  while (WiFi.status() != WL_CONNECTED && retry < 40) {
    delay(500); Serial.print("."); retry++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\n[WiFi] Connected, IP: %s\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("\n[WiFi] Failed!");
    ESP.restart();
  }
}

// ==================== setup ====================
void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println();
  Serial.println("[SYS] ESP32 BLE HID v19 - Bluetooth Status Reporting");
  Serial.printf("[SYS] Chip: %s, Heap: %u\n", ESP.getChipModel(), ESP.getFreeHeap());

  setupBLE();
  connectWiFiBlocking();

  webSocket.beginSSL(WS_HOST, WS_PORT, WS_PATH);
  webSocket.onEvent(onWsEvent);
  webSocket.setReconnectInterval(3000);
  Serial.println("[WS] Connecting ...");

  g_lastKeepAlive = millis();
  Serial.println("[SYS] Ready.");
}

// ==================== loop ====================
void loop() {
  webSocket.loop();

  // 内存监控
  static unsigned long lastHeapCheck = 0;
  if (millis() - lastHeapCheck > 30000) {
    lastHeapCheck = millis();
    size_t free = ESP.getFreeHeap();
    if (free < 20000) {
      Serial.printf("[MEM] Low heap (%u), restarting\n", free);
      ESP.restart();
    }
  }

  // Keep‑alive
  if (bleConnected && millis() - g_lastKeepAlive > 30000) {
    g_lastKeepAlive = millis();
    uint8_t zero[5] = {REPORT_ID_MOUSE, 0, 0, 0, 0};
    sendHIDReport(zero, 5);
  }

  // 状态摘要 + 蓝牙状态上报
  static bool lastBleState = false;
  if (bleConnected != lastBleState) {
    lastBleState = bleConnected;
    if (wsConnected) {
      StaticJsonDocument<64> doc;
      doc["a"] = "status";
      doc["ble"] = bleConnected;
      String out;
      serializeJson(doc, out);
      webSocket.sendTXT(out);
      Serial.printf("[STAT] Sent BLE status: %d\n", bleConnected);
    }
  }
  static unsigned long lastStatus = 0;
  if (millis() - lastStatus > 15000) {
    lastStatus = millis();
    Serial.printf("[STAT] BLE=%d ENC=%d BOND=%d WS=%d Ok=%u Fail=%u Disc=%u Cmd=%u Skip=%u Heap=%u\n",
      bleConnected,
      bleEncrypted,
      bleBonded,
      wsConnected,
      g_notifyCount, g_notifyFail, g_bleDisconnects,
      g_cmdCount, g_cmdSkipped,
      ESP.getFreeHeap());
  }

  // WiFi 自动重连
  static unsigned long lastWifiCheck = 0;
  if (millis() - lastWifiCheck > 30000) {
    lastWifiCheck = millis();
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("[WiFi] Reconnecting ...");
      WiFi.reconnect();
    }
  }
}
