/*
 * ESP32 BLE HID 遥控器 - C3 v15d (新增退格/回车指令)
 *
 * v15d 新增:
 *   - 'd' 指令: 退格 (Backspace, HID 0x2A)
 *   - 'e' 指令: 回车 (Enter, HID 0x28)
 * v15c 修复:
 *   - pollBLEConnection: bleConnected 改为跟随实际 count 值，不再依赖边沿触发
 *   - restartBLEAdvertising: 重置 g_prevConnCount，避免状态卡死
 *   - 彻底移除看门狗（v15b 已做）
 * v15b: 移除误断连看门狗
 * v15a: 统一日志前缀、VERBOSE_CMD 开关
 * v15:  死连接检测、断连超时自动恢复
 */

#include <NimBLEDevice.h>
#include <NimBLEServer.h>
#include <NimBLEService.h>
#include <NimBLEAdvertising.h>
#include <NimBLEHIDDevice.h>
#include <WiFi.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>

// ==================== 日志控制 ====================
#define VERBOSE_CMD 1  // 1=每条指令详细日志，0=只显示异常和统计

// ==================== 用户配置 ====================
const char* WIFI_SSID = "YOUR_WIFI_SSID";
const char* WIFI_PASS = "YOUR_WIFI_PASSWORD";
const char* WS_HOST  = "your-worker.workers.dev";   // Cloudflare Worker 域名
const int   WS_PORT  = 443;
const char* WS_PATH  = "/ws?token=esp32-blehid-device-key-change-me";

// ==================== BLE HID 常量 ====================
#define BLE_DEVICE_NAME    "ESP32 BLE Remote"
#define REPORT_ID_KEYBOARD 1
#define REPORT_ID_MOUSE    2
#define REPORT_ID_CONSUMER 3

#define AC_HOME      0x0223
#define AC_BACK      0x0224
#define AC_TASK_VIEW 0x0296

// 报告描述符（键盘 + 鼠标 + 消费者控制）
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

// ==================== 对象 ====================
NimBLEServer*         pServer       = nullptr;
NimBLEHIDDevice*      pHID          = nullptr;
NimBLECharacteristic* pInputReport  = nullptr;
NimBLEAdvertising*    pAdvertising  = nullptr;
WebSocketsClient*     webSocket     = nullptr;

bool bleConnected   = false;
bool bleEncrypted   = false;
bool hidReady       = false;
bool wsConnected    = false;

uint32_t g_notifyCount    = 0;
uint32_t g_notifyFail     = 0;
uint32_t g_bleDisconnects = 0;
uint32_t g_cmdCount       = 0;
uint32_t g_cmdSkipped     = 0;
uint32_t g_prevConnCount  = 0;

uint32_t g_lastCmdTime        = 0;
uint32_t g_lastKeepAlive      = 0;
uint32_t g_lastBleUp          = 0;
uint8_t  g_consecutiveFail    = 0;
#define  MAX_CONSECUTIVE_FAIL 10
#define  BLE_DOWN_TIMEOUT     30000

// ==================== BLE 回调 ====================
class ServerCB : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* s) {
    Serial.println("[BLE] Connected (callback)");
    bleConnected = true;
    g_lastBleUp = millis();
    g_consecutiveFail = 0;
    NimBLEDevice::startAdvertising();
  }
  void onDisconnect(NimBLEServer* s) {
    Serial.println("[BLE] Disconnected (callback)");
    g_bleDisconnects++;
    bleConnected = false;
    bleEncrypted = false;
    g_consecutiveFail = 0;
    NimBLEDevice::startAdvertising();
  }
  void onAuthenticationComplete(ble_gap_conn_desc* desc) {
    bleEncrypted = desc->sec_state.encrypted;
    Serial.printf("[SEC] Encryption %s\n", bleEncrypted ? "enabled" : "not enabled");
  }
};

// ==================== 重启广播（防御性修复）====================
void restartBLEAdvertising() {
  Serial.println("[BLE] Restarting advertising");
  if (pServer) {
    pServer->disconnect(0);
  }
  bleConnected   = false;
  bleEncrypted   = false;
  g_consecutiveFail = 0;
  g_prevConnCount   = 0;  // v15c: 重置轮询基数，避免边沿触发死锁
  delay(500);
  if (pAdvertising) {
    pAdvertising->start();
    Serial.println("[BLE] Advertising restarted");
  } else {
    Serial.println("[BLE] pAdvertising is null");
  }
}

void checkHeap() {
  size_t free = esp_get_free_heap_size();
  if (free < 30000) {
    Serial.printf("[MEM] Low heap (%u), restarting\n", free);
    ESP.restart();
  }
}

// ==================== WiFi ====================
bool connectWiFi() {
  Serial.println("[WiFi] Connecting...");
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  int retry = 0;
  while (WiFi.status() != WL_CONNECTED && retry < 40) {
    delay(500);
    Serial.print(".");
    retry++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\n[WiFi] Connected, IP: %s\n", WiFi.localIP().toString().c_str());
    return true;
  }
  Serial.println("\n[WiFi] Connection failed");
  return false;
}

// ==================== BLE 初始化 ====================
void setupBLE() {
  Serial.println("[BLE] Init...");
  NimBLEDevice::setSecurityAuth(true, false, true);
  NimBLEDevice::setSecurityIOCap(3);
  Serial.println("[BLE] Security: bonding=ON, MITM=OFF, SC=ON");
  NimBLEDevice::init(BLE_DEVICE_NAME);
  Serial.printf("[BLE] MAC: %s\n", NimBLEDevice::getAddress().toString().c_str());

  pServer = NimBLEDevice::createServer();
  pServer->setCallbacks(new ServerCB());
  Serial.println("[BLE] Server created");

  pHID = new NimBLEHIDDevice(pServer);
  pHID->setReportMap((uint8_t*)HID_REPORT_MAP, sizeof(HID_REPORT_MAP));
  pHID->setHidInfo(0x0111, 0x00);
  pHID->setManufacturer("ESP32");
  pHID->setBatteryLevel(100);
  pHID->startServices();
  pInputReport = pHID->getInputReport(0);
  hidReady = (pInputReport != nullptr);
  Serial.printf("[BLE] HID ready: %d\n", hidReady);

  pAdvertising = NimBLEDevice::getAdvertising();
  pAdvertising->setName(BLE_DEVICE_NAME);
  pAdvertising->setAppearance(0x03C0);
  pAdvertising->addServiceUUID(pHID->getHidService()->getUUID());
  pAdvertising->start();
  Serial.printf("[BLE] Advertising started, name: %s\n", BLE_DEVICE_NAME);
}

// ==================== GATT 轮询（v15c: 完全重写）====================
void pollBLEConnection() {
  if (!pServer) return;
  int count = pServer->getConnectedCount();

  // 状态变化时打印日志
  if (count > 0 && g_prevConnCount == 0) {
    Serial.printf("[BLE] GATT connected (poll, conn=%d)\n", count);
  } else if (count == 0 && g_prevConnCount > 0) {
    Serial.println("[BLE] GATT disconnected (poll)");
  }

  // v15c 关键修复：直接跟随实际连接数，不再依赖边沿触发
  // 防止 restartBLEAdvertising 后 g_prevConnCount 残留导致 bleConnected 永久卡死
  bleConnected = (count > 0);

  if (count > 0) {
    g_lastBleUp = millis();
  } else {
    bleEncrypted = false;
  }

  g_prevConnCount = count;
}

// ==================== HID 发送 ====================
void sendHIDReport(const uint8_t* data, size_t len) {
  if (!bleConnected || !pInputReport || !hidReady) return;
  pInputReport->setValue(data, len);
  if (pInputReport->notify()) {
    g_notifyCount++;
    g_consecutiveFail = 0;
  } else {
    g_notifyFail++;
    g_consecutiveFail++;
  }
  delay(2);
}

void sendKeyboard(uint8_t mod, const uint8_t* keys, uint8_t count) {
  uint8_t r[9] = {REPORT_ID_KEYBOARD, mod, 0};
  for (int i=0;i<6;i++) r[3+i] = i<count ? keys[i] : 0;
  sendHIDReport(r, 9);
}

void releaseKeys() {
  uint8_t r[9] = {REPORT_ID_KEYBOARD,0,0,0,0,0,0,0,0};
  sendHIDReport(r,9);
}

void sendMouse(uint8_t btn, int8_t x, int8_t y, int8_t w) {
  uint8_t r[5] = {REPORT_ID_MOUSE, btn, (uint8_t)x, (uint8_t)y, (uint8_t)w};
  sendHIDReport(r, 5);
}

void sendConsumer(uint16_t usage) {
  uint8_t r[3] = {REPORT_ID_CONSUMER, (uint8_t)(usage&0xFF), (uint8_t)(usage>>8)};
  sendHIDReport(r, 3);
  delay(40);
  uint8_t z[3] = {REPORT_ID_CONSUMER,0,0};
  sendHIDReport(z, 3);
}

// ==================== 文字输入 ====================
uint8_t asciiToHID(char c) {
  if (c>='a' && c<='z') return c-'a'+4;
  if (c>='A' && c<='Z') return c-'A'+4;
  if (c>='1' && c<='9') return c-'1'+30;
  if (c=='0') return 39;
  switch(c) {
    case ' ':return 44; case '\n':case '\r':return 40;
    case '-':case '_':return 45; case '=':case '+':return 46;
    case '[':case '{':return 47; case ']':case '}':return 48;
    case '\\':case '|':return 49; case ';':case ':':return 51;
    case '\'':case '"':return 52; case '`':case '~':return 53;
    case ',':case '<':return 54; case '.':case '>':return 55;
    case '/':case '?':return 56; case '\t':return 43;
    default:return 0;
  }
}
bool needsShift(char c) {
  if (c>='A' && c<='Z') return true;
  switch(c) {
    case '!':case '@':case '#':case '$':case '%':case '^':case '&':
    case '*':case '(':case ')':case '_':case '+':case '{':case '}':
    case '|':case ':':case '"':case '~':case '<':case '>':case '?':
      return true;
  }
  return false;
}
void typeString(const char* text) {
  for (int i=0; text[i]; i++) {
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
  const char* a = doc["a"]; if (!a) return;

  g_cmdCount++;
  g_lastCmdTime = millis();

  if (VERBOSE_CMD) {
    Serial.printf("[CMD] #%u: %s", g_cmdCount, a);
    if (doc.containsKey("x")) Serial.printf(" x=%d", doc["x"].as<int>());
    if (doc.containsKey("y")) Serial.printf(" y=%d", doc["y"].as<int>());
    if (doc.containsKey("b")) Serial.printf(" btn=%d", doc["b"].as<int>());
    if (doc.containsKey("d")) Serial.printf(" d=\"%.20s\"", doc["d"].as<const char*>());
    Serial.println();
  }

  if (!hidReady) {
    g_cmdSkipped++;
    if (VERBOSE_CMD) Serial.println("[CMD] Skipped: hidReady=false");
    return;
  }
  if (!bleConnected) {
    g_cmdSkipped++;
    if (VERBOSE_CMD) Serial.println("[CMD] Skipped: BLE not connected");
    return;
  }

  switch(a[0]) {
    case 'h': sendConsumer(AC_HOME); break;
    case 'b': sendConsumer(AC_BACK); break;
    case 's': sendConsumer(AC_TASK_VIEW); delay(80);
              { uint8_t t=0x2B; sendKeyboard(0x08, &t,1); delay(120); releaseKeys(); }
              break;
    case 'c': sendMouse(1,0,0,0); delay(50); sendMouse(0,0,0,0); break;
    case 'd': { uint8_t k=0x2A; sendKeyboard(0,&k,1); delay(30); releaseKeys(); } break;
    case 'e': { uint8_t k=0x28; sendKeyboard(0,&k,1); delay(30); releaseKeys(); } break;
    case 'm': {
      int dx = doc["x"]|0, dy = doc["y"]|0;
      uint8_t btn = doc["b"].as<uint8_t>();
      if (dx>127) dx=127; if (dx<-127) dx=-127;
      if (dy>127) dy=127; if (dy<-127) dy=-127;
      sendMouse(btn, (int8_t)dx, (int8_t)dy, 0);
      break;
    }
    case 't': if (doc["d"]) typeString(doc["d"]); break;
  }
}

// ==================== WebSocket ====================
void onWsEvent(WStype_t type, uint8_t* payload, size_t length) {
  switch(type) {
    case WStype_DISCONNECTED: wsConnected=false; break;
    case WStype_CONNECTED:
      wsConnected=true;
      g_lastCmdTime = millis();
      Serial.println("[WS] Connected");
      break;
    case WStype_TEXT: handleCommand((char*)payload); break;
    case WStype_ERROR: wsConnected=false; break;
    default: break;
  }
}

// ==================== setup ====================
void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println("\n[SYS] ESP32 BLE HID v15d (+Backspace +Enter)");
  Serial.printf("[SYS] Chip: %s, Flash: %dMB, Heap: %d\n",
    ESP.getChipModel(), ESP.getFlashChipSize()/1048576, ESP.getFreeHeap());

  setupBLE();
  checkHeap();

  if (!connectWiFi()) {
    Serial.println("[WiFi] Connection failed, restarting");
    delay(3000);
    ESP.restart();
  }
  checkHeap();

  webSocket = new WebSocketsClient();
  webSocket->beginSSL(WS_HOST, WS_PORT, WS_PATH);
  webSocket->onEvent(onWsEvent);
  webSocket->setReconnectInterval(3000);
  Serial.println("[WS] Connecting...");

  g_lastCmdTime   = millis();
  g_lastKeepAlive = millis();
  g_lastBleUp     = millis();
  Serial.println("[SYS] Ready\n");
}

// ==================== loop ====================
void loop() {
  if (webSocket) webSocket->loop();
  checkHeap();

  static unsigned long lastPoll = 0;
  if (millis() - lastPoll > 1000) {
    lastPoll = millis();
    pollBLEConnection();
  }

  // ---- 死连接检测 ----
  if (bleConnected && g_consecutiveFail >= MAX_CONSECUTIVE_FAIL) {
    Serial.printf("[DEAD] %u consecutive notify failures, restarting BLE\n", g_consecutiveFail);
    restartBLEAdvertising();
  }

  // ---- 断连超时恢复 ----
  static bool recoveryInProgress = false;
  if (!bleConnected && !recoveryInProgress && (millis() - g_lastBleUp > BLE_DOWN_TIMEOUT)) {
    recoveryInProgress = true;
    Serial.printf("[RECOV] BLE down %lu ms, restarting advertising\n", millis() - g_lastBleUp);
    restartBLEAdvertising();
  }
  if (bleConnected && recoveryInProgress) {
    recoveryInProgress = false;
    Serial.println("[RECOV] BLE reconnected");
  }

  // ---- Keep‑alive ----
  if (bleConnected && (millis() - g_lastKeepAlive > 30000)) {
    g_lastKeepAlive = millis();
    uint8_t zero[5] = {REPORT_ID_MOUSE, 0, 0, 0, 0};
    sendHIDReport(zero, 5);
  }

  // ---- 状态摘要 ----
  static unsigned long lastStatus = 0;
  if (millis() - lastStatus > 15000) {
    lastStatus = millis();
    Serial.printf("[STAT] BLE=%d ENC=%d CONN=%d WS=%d Cmd=%u Skip=%u Ok=%u Fail=%u Disc=%u CFail=%u Heap=%d\n",
      bleConnected,
      bleEncrypted,
      pServer ? pServer->getConnectedCount() : -1,
      wsConnected,
      g_cmdCount, g_cmdSkipped,
      g_notifyCount, g_notifyFail, g_bleDisconnects,
      g_consecutiveFail,
      ESP.getFreeHeap());
  }

  // ---- WiFi 重连 ----
  static unsigned long lastCheck = 0;
  if (millis() - lastCheck > 30000) {
    lastCheck = millis();
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("[WiFi] Reconnecting...");
      WiFi.reconnect();
    }
  }
}
