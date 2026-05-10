/*
 * ESP32 BLE HID 遥控器 - C3 v9 (拖拽支持)
 *
 * v9 修复:
 *   - 鼠标移动指令支持按钮状态字段 b (0=抬起, 1=左键按下)
 *   - 配合 Web 前端锁定按钮实现拖拽功能
 *   - 保留 v8 所有修复（轮询 BLE 连接、安全配置、电池服务等）
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

// ==================== 动态分配对象 ====================
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

// ==================== BLE 连接回调（保留以兼容未来库版本） ====================
class ServerCB : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* s) {
    Serial.println("[BLE] [回调] onConnect 触发");
    bleConnected = true;
    NimBLEDevice::startAdvertising();
  }
  void onDisconnect(NimBLEServer* s) {
    Serial.println("[BLE] [回调] onDisconnect 触发");
    g_bleDisconnects++;
    bleConnected = false;
    bleEncrypted = false;
    NimBLEDevice::startAdvertising();
  }
};

// ==================== 内存保护 ====================
void checkHeap() {
  size_t free = esp_get_free_heap_size();
  if (free < 30000) {
    Serial.printf("[MEM] 堆内存过低 (%u)，重启...\n", free);
    ESP.restart();
  }
}

// ==================== WiFi 连接 ====================
bool connectWiFi() {
  Serial.println("[WiFi] 开始连接...");
  Serial.flush();
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  int retry = 0;
  while (WiFi.status() != WL_CONNECTED && retry < 40) {
    delay(500);
    Serial.print(".");
    retry++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\n[WiFi] 已连接, IP: %s\n", WiFi.localIP().toString().c_str());
    return true;
  }
  Serial.println("\n[WiFi] 连接失败!");
  return false;
}

// ==================== BLE 初始化 ====================
void setupBLE() {
  Serial.println("[BLE] 初始化...");
  Serial.flush();

  // ========== 安全配置 — 必须在 init() 之前！ ==========
  NimBLEDevice::setSecurityAuth(true, false, true);  // bonding=ON, MITM=OFF, SC=ON
  NimBLEDevice::setSecurityIOCap(3);                  // NoInputNoOutput
  Serial.println("[BLE] 安全: bonding=ON, MITM=OFF, SC=ON, IO=NoInputNoOutput");
  Serial.flush();

  NimBLEDevice::init(BLE_DEVICE_NAME);
  Serial.printf("[BLE] MAC: %s\n", NimBLEDevice::getAddress().toString().c_str());
  Serial.flush();

  pServer = NimBLEDevice::createServer();
  if (!pServer) { Serial.println("[BLE] ✗ 创建 Server 失败"); return; }
  pServer->setCallbacks(new ServerCB());
  Serial.println("[BLE] Server 已创建，回调已注册");

  pHID = new NimBLEHIDDevice(pServer);
  if (!pHID) { Serial.println("[BLE] ✗ 创建 HID 失败"); return; }

  pHID->setReportMap((uint8_t*)HID_REPORT_MAP, sizeof(HID_REPORT_MAP));
  pHID->setHidInfo(0x0111, 0x00);
  pHID->setManufacturer("ESP32");
  pHID->setBatteryLevel(100);
  Serial.println("[BLE] HID 设备配置完成 (含电池服务)");

  pHID->startServices();

  pInputReport = pHID->getInputReport(0);
  if (pInputReport) {
    hidReady = true;
    Serial.println("[BLE] 输入报告就绪");
  } else {
    Serial.println("[BLE] ✗ 错误：无法获取输入报告！");
  }

  // 广播配置
  pAdvertising = NimBLEDevice::getAdvertising();
  pAdvertising->setName(BLE_DEVICE_NAME);
  pAdvertising->setAppearance(0x03C0);
  pAdvertising->setMinInterval(32);
  pAdvertising->setMaxInterval(48);
  if (pHID->getHidService()) {
    pAdvertising->addServiceUUID(pHID->getHidService()->getUUID());
    Serial.println("[BLE] HID Service UUID 已加入广播数据");
  } else {
    Serial.println("[BLE] ✗ 警告: HID Service 不存在！");
  }

  if (pAdvertising->start()) {
    Serial.println("[BLE] 广播已启动！设备可见");
    Serial.printf("[BLE] 名称: %s\n", BLE_DEVICE_NAME);
  } else {
    Serial.println("[BLE] ✗ 广播启动失败！");
  }
  Serial.flush();
}

// ==================== 轮询检测 GATT 连接状态 ====================
void pollBLEConnection() {
  if (!pServer) return;
  
  int count = pServer->getConnectedCount();
  
  if (count > 0 && g_prevConnCount == 0) {
    bleConnected = true;
    Serial.printf("[BLE] ✓ GATT 已连接 (轮询, conn=%d)\n", count);
  } else if (count == 0 && g_prevConnCount > 0) {
    bleConnected = false;
    bleEncrypted = false;
    Serial.println("[BLE] GATT 断开 (轮询)");
  }
  
  g_prevConnCount = count;
}

// ==================== 延时 ====================
void delayWS(unsigned long ms) {
  unsigned long start = millis();
  while (millis() - start < ms) {
    if (webSocket) webSocket->loop();
    delay(1);
  }
}

// ==================== HID 报告发送 ====================
void sendHIDReport(const uint8_t* data, size_t len) {
  if (!bleConnected || !pInputReport || !hidReady) return;
  pInputReport->setValue(data, len);
  if (pInputReport->notify()) g_notifyCount++;
  else g_notifyFail++;
  delayWS(2);
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
  delayWS(40);
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
    delayWS(25);
    releaseKeys();
    delayWS(15);
  }
}

// ==================== 指令处理 ====================
void handleCommand(const char* json) {
  StaticJsonDocument<384> doc;
  if (deserializeJson(doc, json)) return;
  const char* a = doc["a"]; if (!a) return;

  g_cmdCount++;
  Serial.printf("[CMD] #%u: a=%s", g_cmdCount, a);
  if (doc.containsKey("x")) Serial.printf(" x=%d", doc["x"].as<int>());
  if (doc.containsKey("y")) Serial.printf(" y=%d", doc["y"].as<int>());
  if (doc.containsKey("b")) Serial.printf(" btn=%d", doc["b"].as<int>());
  if (doc.containsKey("d")) Serial.printf(" d=\"%s\"", doc["d"].as<const char*>());
  Serial.println();

  if (!hidReady) {
    g_cmdSkipped++;
    Serial.println("[CMD] ✗ 跳过: hidReady=false");
    return;
  }
  if (!bleConnected) {
    g_cmdSkipped++;
    Serial.println("[CMD] ✗ 跳过: BLE 未连接");
    return;
  }

  switch(a[0]) {
    case 'h': sendConsumer(AC_HOME); break;
    case 'b': sendConsumer(AC_BACK); break;
    case 's': sendConsumer(AC_TASK_VIEW); delayWS(80);
              { uint8_t t=0x2B; sendKeyboard(0x08, &t,1); delayWS(120); releaseKeys(); }
              break;
    case 'c': sendMouse(1,0,0,0); delayWS(50); sendMouse(0,0,0,0); break;
    case 'm': {
      int dx = doc["x"]|0, dy = doc["y"]|0;
      // v9: 支持按钮状态字段 b（锁定拖拽）
      uint8_t btn = doc["b"] | 0;
      if (dx>127) dx=127; if (dx<-127) dx=-127;
      if (dy>127) dy=127; if (dy<-127) dy=-127;
      sendMouse(btn, (int8_t)dx, (int8_t)dy, 0);
      break;
    }
    case 't': if (doc["d"]) typeString(doc["d"]); break;
  }
}

// ==================== WebSocket 事件 ====================
void onWsEvent(WStype_t type, uint8_t* payload, size_t length) {
  switch(type) {
    case WStype_DISCONNECTED: wsConnected=false; break;
    case WStype_CONNECTED: wsConnected=true; break;
    case WStype_TEXT: handleCommand((char*)payload); break;
    case WStype_ERROR: wsConnected=false; break;
    default: break;
  }
}

// ==================== setup ====================
void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println("\n\nESP32 BLE HID v9 (drag support)");
  Serial.printf("Chip: %s, Flash: %dMB, Heap: %d\n",
    ESP.getChipModel(), ESP.getFlashChipSize()/1048576, ESP.getFreeHeap());
  Serial.flush();

  // 1. BLE (+ 安全配置)
  setupBLE();
  checkHeap();

  // 2. WiFi
  if (!connectWiFi()) {
    Serial.println("WiFi 失败，重启");
    delay(3000);
    ESP.restart();
  }
  checkHeap();

  // 3. WebSocket
  webSocket = new WebSocketsClient();
  if (webSocket) {
    webSocket->beginSSL(WS_HOST, WS_PORT, WS_PATH);
    webSocket->onEvent(onWsEvent);
    webSocket->setReconnectInterval(3000);
    Serial.println("[WS] 开始连接...");
  } else {
    Serial.println("[E] WebSocket 分配失败");
  }

  Serial.println("初始化完成\n");
  Serial.flush();
}

void loop() {
  if (webSocket) webSocket->loop();
  checkHeap();

  // 每 1 秒轮询 GATT 连接数
  static unsigned long lastPoll = 0;
  if (millis() - lastPoll > 1000) {
    lastPoll = millis();
    pollBLEConnection();
  }

  // 每 15 秒输出状态
  static unsigned long lastStatus = 0;
  if (millis() - lastStatus > 15000) {
    lastStatus = millis();
    Serial.printf("[STAT] BLE=%d CONN=%d WS=%d CMD=%u 跳过=%u OK:%u FAIL:%u 断=%u 堆=%d\n",
      bleConnected,
      pServer ? pServer->getConnectedCount() : -1,
      wsConnected,
      g_cmdCount, g_cmdSkipped,
      g_notifyCount, g_notifyFail, g_bleDisconnects,
      ESP.getFreeHeap());
  }

  // WiFi 重连检查
  static unsigned long lastCheck = 0;
  if (millis() - lastCheck > 30000) {
    lastCheck = millis();
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("[WiFi] 断开，重连...");
      WiFi.reconnect();
    }
  }
}
