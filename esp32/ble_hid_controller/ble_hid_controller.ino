/*
 * ESP32 BLE HID 遥控器 - C3 v39 (LED 状态指示)
 *
 * ========== 版本更新日志 ==========
 * v39 (2026-06-06): 板载蓝色 LED 状态指示 — WiFi/WS/BLE 连接状态
 *                    快闪(200ms)=WiFi断开; 慢闪(1000ms)=服务器断开;
 *                    双闪=BLE断开; 常亮=全部正常
 * v38 (2026-06-04): register-ack 后立即 ping 活跃窗口拉取命令；
 *                    移除 g_cmdCount>0 条件；
 *                    WebSocket 缓冲区 WEBSOCKETS_MAX_DATA_SIZE=4096
 * v37: 活跃窗口高频拉取（10s / 1s / 最多 10 次 ping）
 * v36: cmd 从 hb-ack/ping-ack 提取执行，移除独立 ws 拉取
 * v35: 心跳间隔 15s→30s，适应 BLE keep-alive 30s
 * v34: register-ack 后立即 ping 一次
 * v33: 命令队列上限 MAX_CMD_QUEUE=20
 * v32: 多设备切换支持
 * ================================
 *
 * 库依赖（Arduino IDE 内安装）：
 *   NimBLE-Arduino v2.5.0 (h2zero)
 *   ArduinoJson v7.x (Benoit Blanchon)
 *   arduinoWebSockets (Markus Sattler)
 *
 * 开发板：MakerGO ESP32-C3-SUPER-MINI, Partition Scheme = Huge APP
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

// v34: 增大 WebSocket 接收缓冲区，防止 hb-ack/ping-ack 带命令时 JSON 被截断
// arduinoWebSockets 默认缓冲区可能不足，导致长 JSON 解析失败 → bad JSON → CMD=0
#define WEBSOCKETS_MAX_DATA_SIZE 4096

// v33: 命令队列上限（与 DO 端 MAX_QUEUE_SIZE 保持一致）
#define MAX_CMD_QUEUE 20

// ==================== 用户配置 ====================
//const char* WIFI_SSID = "你的WiFi名";
//const char* WIFI_PASS = "你的WiFi密码";
//const char* WS_HOST  = "cf-esp32-blehid.你的用户名.workers.dev";   // Cloudflare Worker 域名
//const int   WS_PORT  = 443;
//const char* WS_PATH  = "/ws?token=你的设备令牌";  // 须与 DEVICE_SECRET 一致

const char* WIFI_SSID = "你的WiFi名";
const char* WIFI_PASS = "你的WiFi密码";
const char* WS_HOST  = "cf-esp32-blehid.你的用户名.workers.dev";   // Cloudflare Worker 域名
const int   WS_PORT  = 443;
const char* WS_PATH  = "/ws?token=你的设备令牌";  // 须与 DEVICE_SECRET 一致

// ==================== LED 状态指示 ====================
// GPIO 8 板载蓝色 LED，反相逻辑 (LOW=亮 HIGH=灭)
#define LED_BUILTIN 8

// LED 模式（优先级从高到低）
enum {
  LED_SOLID_ON,     // 全部正常 → 常亮
  LED_DOUBLE_BLINK, // BLE 未连接 → 双闪
  LED_SLOW_BLINK,   // 服务器未连接 → 慢闪
  LED_FAST_BLINK,   // WiFi 未连接 → 快闪
  LED_COUNT
};

// ==================== BLE HID 常量 ====================
#define BLE_DEVICE_NAME    "ESP32 BLE Remote 001" //设备名可自定义
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

// ==================== 全局对象 ====================
NimBLEServer*         pServer       = nullptr;
NimBLEHIDDevice*      pHID          = nullptr;
NimBLECharacteristic* pInputReport  = nullptr;
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
uint32_t g_wsMsgCount     = 0;  // v21: WebSocket 消息总数
uint32_t g_lastCmdTime     = 0;  // v21: 上次收到命令的时间
uint32_t g_lastWsMsgTime   = 0;  // v21: 上次收到任何 WS 消息的时间

// v37: 活跃窗口高频拉取 — 收到命令后 10 秒内每秒 ping 一次，延迟 <1s
// 根因：fix1 (v36 fast drain) 有 bug：onWsEvent 中 ping-ack 0 cmd 分支重复设置
// g_lastFastDrainPing，导致 loop() 中 timer 永远被重置 → 无限 ping 循环。
// fix2：缩短拉取间隔到 1s，10 秒窗口，确保用户操作间隙延迟 <1s。
uint32_t g_lastActivePing   = 0;       // 上次活跃窗口 ping 的时间
#define ACTIVE_WINDOW_MS         10000  // 收到命令后 10 秒内保持活跃
#define ACTIVE_PING_INTERVAL     1000   // 活跃窗口内每 1 秒 ping 一次
#define ACTIVE_PING_MAX          10     // 最多 10 次 (覆盖 10 秒窗口)
uint8_t  g_activePingCount    = 0;      // 活跃窗口内已 ping 次数

// v19: 多设备支持 - 设备标识
String g_deviceId   = "";
String g_deviceName = "";

// v20: register ACK 重试机制
bool     g_registerAcked     = false;   // 是否已收到 DO 的 register-ack
uint8_t  g_registerRetries   = 0;       // 已重试次数
uint32_t g_lastRegisterTime  = 0;       // 上次发送 register 的时间
#define REGISTER_RETRY_MAX      5       // 最多重试 5 次
#define REGISTER_RETRY_INTERVAL 2000    // 2 秒无 ACK 则重试

// v25: 前置声明，供 BLE 回调使用
void sendDeviceMessage(const char* msgType);

// LED 状态变量
uint8_t  g_ledMode      = LED_FAST_BLINK;
uint8_t  g_ledBlinkStep = 0;
uint32_t g_ledLastToggle = 0;

// v25: BLE 回调延迟标志 - NimBLE 回调在 FreeRTOS 任务上下文中运行，
// 不能直接调用 WebSocket (sendTXT)，否则与主 loop() 中的 webSocket.loop()
// 产生竞态。使用标志位延迟到 loop() 中发送。
volatile bool g_pendingBleStatus = false;

// ==================== BLE 回调 ====================
class ServerCB : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* s, NimBLEConnInfo& connInfo) override {
    Serial.println("[BLE] Connected");
    bleConnected = true;
    // v25: 设置延迟标志，在 loop() 中安全发送
    g_pendingBleStatus = true;
  }

  void onDisconnect(NimBLEServer* s, NimBLEConnInfo& connInfo, int reason) override {
    Serial.printf("[BLE] Disconnected (reason=%d), restart advertising\n", reason);
    bleConnected = false;
    bleEncrypted = false;
    bleBonded    = false;
    g_bleDisconnects++;
    // v25: 设置延迟标志，在 loop() 中安全发送
    g_pendingBleStatus = true;
    NimBLEDevice::startAdvertising();
  }

  void onAuthenticationComplete(NimBLEConnInfo& connInfo) override {
    bleEncrypted = connInfo.isEncrypted();
    bleBonded    = connInfo.isBonded();
    Serial.printf("[SEC] Encryption %s, bonded=%d\n",
      bleEncrypted ? "enabled" : "not enabled",
      bleBonded);
    // v25: 设置延迟标志，在 loop() 中安全发送
    g_pendingBleStatus = true;
  }
};

// ==================== BLE 初始化 ====================
void setupBLE() {
  Serial.println("[BLE] Init ...");

  NimBLEDevice::init(BLE_DEVICE_NAME);
  Serial.printf("[BLE] MAC: %s\n", NimBLEDevice::getAddress().toString().c_str());

  NimBLEDevice::setSecurityAuth(true, false, true);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
  Serial.println("[BLE] Security: bonding=ON, MITM=OFF, SC=ON (Just Works)");

  pServer = NimBLEDevice::createServer();
  pServer->setCallbacks(new ServerCB());

  pHID = new NimBLEHIDDevice(pServer);
  pHID->setReportMap((uint8_t*)HID_REPORT_MAP, sizeof(HID_REPORT_MAP));
  pHID->setHidInfo(0x00, 0x00);
  pHID->setManufacturer("ESP32-C3");

  pInputReport = pHID->getInputReport(0);
  hidReady = (pInputReport != nullptr);
  Serial.printf("[BLE] HID ready: %d\n", hidReady);

  pServer->start();

  NimBLEAdvertising* pAdv = NimBLEDevice::getAdvertising();
  pAdv->setName(BLE_DEVICE_NAME);
  pAdv->setAppearance(0x03C0);
  pAdv->addServiceUUID(pHID->getHidService()->getUUID());
  pAdv->start();
  Serial.println("[BLE] Advertising started");
}

// ==================== HID 发送 ====================
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
  uint8_t z[3] = {REPORT_ID_CONSUMER, 0, 0};
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
  g_lastCmdTime = millis();

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
      long dx = doc["x"].as<long>(), dy = doc["y"].as<long>();
      uint8_t btn = doc["b"].as<uint8_t>();
      if (dx > 127) dx = 127; if (dx < -127) dx = -127;
      if (dy > 127) dy = 127; if (dy < -127) dy = -127;
      sendMouse(btn, (int8_t)dx, (int8_t)dy, 0);
      break;
    }
    case 't': {
      const char* text = doc["d"].as<const char*>();
      if (text) typeString(text);
      break;
    }
  }
}

// ==================== WebSocket ====================
// v19: 发送设备信息消息 (注册/心跳共用)
void sendDeviceMessage(const char* msgType) {
  if (!wsConnected) return;
  StaticJsonDocument<512> doc;
  doc["type"]       = msgType;
  doc["deviceId"]   = g_deviceId;
  doc["deviceName"] = g_deviceName;
  JsonObject info = doc.createNestedObject("info");
  info["bleConnected"] = bleConnected;
  info["bleEncrypted"] = bleEncrypted;
  info["bleBonded"]    = bleBonded;
  JsonObject stats = info.createNestedObject("stats");
  stats["notifyOk"]   = g_notifyCount;
  stats["notifyFail"] = g_notifyFail;
  stats["disconn"]    = g_bleDisconnects;
  stats["cmdCount"]   = g_cmdCount;
  stats["cmdSkip"]    = g_cmdSkipped;
  stats["heap"]       = ESP.getFreeHeap();
  String out;
  serializeJson(doc, out);
  if (!webSocket.sendTXT(out)) {
    Serial.printf("[WS] sendTXT failed for %s\n", msgType);
  }
}

void onWsEvent(WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_DISCONNECTED:
      wsConnected = false;
      g_registerAcked = false;
      Serial.printf("[WS] Disconnected (total msgs received=%u)\n", g_wsMsgCount);
      break;
    case WStype_CONNECTED:
      wsConnected = true;
      g_registerAcked = false;
      g_registerRetries = 0;
      g_wsMsgCount = 0;          // 新连接重置消息计数
      g_lastWsMsgTime = millis();
      Serial.println("[WS] Connected");
      sendDeviceMessage("register");
      g_lastRegisterTime = millis();
      break;
    case WStype_TEXT: {
      g_wsMsgCount++;
      g_lastWsMsgTime = millis();

      // v21: 对非 JSON 消息记录前缀，帮助诊断
      #if VERBOSE_CMD
        char preview[33];
        size_t pl = length < 32 ? length : 32;
        memcpy(preview, payload, pl);
        preview[pl] = '\0';
        // 清理换行符
        for (size_t i = 0; i < pl; i++) if (preview[i] < 32 && preview[i] > 0) preview[i] = '.';
      #endif

      // v20/v21/v32: 解析系统消息（register-ack, ping, hb-ack, ping-ack）
      // v32: StaticJsonDocument<512> 确保容纳包含命令数组的 hb-ack/ping-ack
      StaticJsonDocument<512> docAck;
      if (!deserializeJson(docAck, (char*)payload)) {
        const char* msgType = docAck["type"];

        // register-ack (v32: 也提取 cmds 数组)
        if (msgType && strcmp(msgType, "register-ack") == 0) {
          const char* ackDev = docAck["deviceId"];
          if (ackDev && String(ackDev) == g_deviceId) {
            g_registerAcked = true;
            g_registerRetries = 0;
            Serial.printf("[WS] Register ACK received for %s\n", g_deviceId.c_str());
            // v32: 提取嵌入的命令队列（替换旧版独立 ws.send 排空）
            JsonArray regcmds = docAck["cmds"].as<JsonArray>();
            if (regcmds.size() > 0) {
              Serial.printf("[WS] register-ack: %u cmd(s) received\n", (unsigned)regcmds.size());
              for (JsonObject cmdObj : regcmds) {
                String cmdStr;
                serializeJson(cmdObj, cmdStr);
                handleCommand((char*)cmdStr.c_str());
              }
            }
            // v38: 注册确认后立即发 ping 拉取 DO 中可能残留的待处理命令
            // 根因：如果 regcmds 为空（或排空了 MAX_DRAIN_SIZE 条但还有剩余），
            // ESP32 不会主动 ping → 剩余命令等到 15s heartbeat 才被拉取 → 高延迟
            g_lastCmdTime = millis();
            g_activePingCount = 0;
            g_lastActivePing = millis();
            Serial.println("[WS] Register ACK: sending ping to drain any pending commands");
            webSocket.sendTXT("{\"type\":\"ping\",\"deviceId\":\"" + g_deviceId + "\"}");
            g_lastWsMsgTime = millis();
            break;
          }
        }

        // v21: ping-pong 协议 — DO 可在休眠时发送 ping 测试连通性
        if (msgType && strcmp(msgType, "ping") == 0) {
          String pong = "{\"type\":\"pong\",\"deviceId\":\"" + g_deviceId + "\"}";
          webSocket.sendTXT(pong);
          #if VERBOSE_CMD
            Serial.println("[WS] Ping received, pong sent");
          #endif
          break;
        }

        // v37: hb-ack / ping-ack — DO 将命令队列嵌入回复，ESP32 从 cmds 数组提取执行
        if (msgType && (strcmp(msgType, "hb-ack") == 0 || strcmp(msgType, "ping-ack") == 0)) {
          JsonArray cmds = docAck["cmds"].as<JsonArray>();
          unsigned int cmdCount = cmds.size();
          if (cmdCount > 0) {
            Serial.printf("[WS] %s: %u cmd(s) received\n", msgType, cmdCount);
            for (JsonObject cmdObj : cmds) {
              String cmdStr;
              serializeJson(cmdObj, cmdStr);
              handleCommand((char*)cmdStr.c_str());
            }
            // 收到命令 → 启动/重置活跃窗口 + 立即发 ping 排空下一批
            g_activePingCount = 0;
            g_lastActivePing = millis();
            Serial.printf("[WS] Received %u cmd(s), sending ping for next batch\n", cmdCount);
            webSocket.sendTXT("{\"type\":\"ping\",\"deviceId\":\"" + g_deviceId + "\"}");
            g_lastWsMsgTime = millis();
          } else {
            // ping-ack 返回 0 条 — 不做任何事，由 loop() 中的活跃窗口 timer 负责重试
            #if VERBOSE_CMD
              Serial.printf("[WS] %s: 0 cmd(s)\n", msgType);
            #endif
          }
          break;  // 不 fall through 到 handleCommand
        }

        // v21: DO 可能因队列排空发送多条命令，记录非命令消息
        if (msgType) {
          Serial.printf("[WS] Msg #%u: type=%s\n", g_wsMsgCount, msgType);
        }
        // 不是已知系统消息 → 作为直接命令解析 (浏览器 UI 发来的 {"a":"c"} 等)
        handleCommand((char*)payload);
      } else {
        // v34: JSON 解析失败 → 不 fall through 到 handleCommand
        // 截断/损坏的消息无法解析为命令，直接跳过
        Serial.printf("[WS] Msg #%u: bad JSON, raw=\"%s\"\n", g_wsMsgCount, preview);
      }
      break;
    }
    case WStype_ERROR:
      wsConnected = false;
      g_registerAcked = false;
      Serial.printf("[WS] Error (total msgs received=%u)\n", g_wsMsgCount);
      break;
    default:
      break;
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

// ==================== LED 状态更新 ====================
void updateLED() {
  unsigned long now = millis();

  // 确定当前应有的 LED 模式（优先级：WiFi > WS > BLE > 全正常）
  uint8_t desired;
  if (WiFi.status() != WL_CONNECTED) {
    desired = LED_FAST_BLINK;
  } else if (!wsConnected) {
    desired = LED_SLOW_BLINK;
  } else if (!bleConnected) {
    desired = LED_DOUBLE_BLINK;
  } else {
    desired = LED_SOLID_ON;
  }

  // 模式切换时重置状态机
  if (desired != g_ledMode) {
    g_ledMode = desired;
    g_ledBlinkStep = 0;
    g_ledLastToggle = now;
  }

  switch (g_ledMode) {
    case LED_SOLID_ON:
      digitalWrite(LED_BUILTIN, LOW); // 常亮
      break;

    case LED_FAST_BLINK: // 快闪 200ms 周期
      if (now - g_ledLastToggle >= 100) {
        digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
        g_ledLastToggle = now;
      }
      break;

    case LED_SLOW_BLINK: // 慢闪 1000ms 周期
      if (now - g_ledLastToggle >= 500) {
        digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
        g_ledLastToggle = now;
      }
      break;

    case LED_DOUBLE_BLINK: { // 双闪 (100亮+100灭+100亮+1700灭)
      static const uint16_t durations[] = {100, 100, 100, 1700};
      if (now - g_ledLastToggle >= durations[g_ledBlinkStep]) {
        g_ledBlinkStep = (g_ledBlinkStep + 1) % 4;
        digitalWrite(LED_BUILTIN, (g_ledBlinkStep % 2) ? HIGH : LOW);
        g_ledLastToggle = now;
      }
      break;
    }
  }
}

// ==================== setup ====================
void setup() {
  Serial.begin(115200);
  delay(2000);

  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, HIGH); // 初始灭

  Serial.println();
  Serial.println("[SYS] ESP32 BLE HID v39（板载 LED 状态指示）");
  Serial.printf("[SYS] Chip: %s, Heap: %u\n", ESP.getChipModel(), ESP.getFreeHeap());

  setupBLE();
  connectWiFiBlocking();

  // v19: 基于 WiFi MAC 地址设置设备 ID（eFuse 硬件地址，稳定唯一）
  // 不使用 BLE MAC，因为 NimBLE 可能使用随机静态地址，NVS 擦除后会变化
  String wifiMac = WiFi.macAddress();
  g_deviceId = wifiMac;
  g_deviceName = String(BLE_DEVICE_NAME) + " (" + wifiMac + ")";
  Serial.printf("[SYS] WiFi MAC: %s\n", wifiMac.c_str());
  Serial.printf("[SYS] DeviceID (WiFi MAC): %s\n", g_deviceId.c_str());
  Serial.printf("[SYS] DeviceName: %s\n", g_deviceName.c_str());

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

  updateLED();

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

  // v25: 处理 BLE 回调延迟的状态更新（线程安全）
  if (g_pendingBleStatus) {
    g_pendingBleStatus = false;
    sendDeviceMessage("status");
  }

  // Keep-alive
  if (bleConnected && millis() - g_lastKeepAlive > 30000) {
    g_lastKeepAlive = millis();
    uint8_t zero[5] = {REPORT_ID_MOUSE, 0, 0, 0, 0};
    sendHIDReport(zero, 5);
  }

  // v37: 活跃窗口高频拉取 — 距离上次收到命令 10 秒内，每 1 秒 ping 一次
  // 确保用户短暂停止后恢复操作时，延迟 <1s（vs 原来等 15s heartbeat）
  unsigned long sinceCmd = (millis() - g_lastCmdTime);
  // v38: 移除 g_cmdCount > 0 条件
  // 根因：register-ack 设置 g_lastCmdTime 后，如果之前从未收到命令 (g_cmdCount=0)，
  // g_cmdCount > 0 阻止活跃窗口启动 → 剩余命令等到 15s heartbeat → 高延迟
  if (wsConnected && g_registerAcked && sinceCmd < ACTIVE_WINDOW_MS && g_activePingCount < ACTIVE_PING_MAX) {
    if (millis() - g_lastActivePing > ACTIVE_PING_INTERVAL) {
      g_activePingCount++;
      g_lastActivePing = millis();
      Serial.printf("[WS] Active ping #%d/%d (last cmd %lu ms ago)\n", g_activePingCount, ACTIVE_PING_MAX, sinceCmd);
      webSocket.sendTXT("{\"type\":\"ping\",\"deviceId\":\"" + g_deviceId + "\"}");
      g_lastWsMsgTime = millis();
    }
  }

  // v21: idle ping — WS 已连接但超过 30 秒未收到任何消息，发送 ping 唤醒 DO
  // DO hibernation 后 ws 绑定丢失，ping 触发 webSocketMessage 重新绑定 ws
  if (wsConnected && g_registerAcked && g_activePingCount >= ACTIVE_PING_MAX && millis() - g_lastWsMsgTime > 30000) {
    Serial.printf("[WS] Idle %lu sec: sending ping to re-establish DO ws binding\n",
      (unsigned long)((millis() - g_lastWsMsgTime) / 1000));
    String ping = "{\"type\":\"ping\",\"deviceId\":\"" + g_deviceId + "\"}";
    webSocket.sendTXT(ping);
    g_lastWsMsgTime = millis();  // 防止 ping 风暴
  }

  // 状态摘要 & 心跳
  static unsigned long lastStatus = 0;
  if (millis() - lastStatus > 15000) {
    lastStatus = millis();
    // v21: 检测命令饥饿 — WS 已连接但长时间无命令
    unsigned long secSinceCmd = (millis() - g_lastCmdTime) / 1000;
    bool cmdStarving = wsConnected && (g_cmdCount > 0) && (secSinceCmd > 30);

    Serial.printf("[STAT] BLE=%d ENC=%d BOND=%d WS=%d Ok=%u Fail=%u Disc=%u Cmd=%u Skip=%u Heap=%u%s\n",
      bleConnected,
      bleEncrypted,
      bleBonded,
      wsConnected,
      g_notifyCount, g_notifyFail, g_bleDisconnects,
      g_cmdCount, g_cmdSkipped,
      ESP.getFreeHeap(),
      cmdStarving ? " [WARN: cmd starving!]" : "");

    if (cmdStarving) {
      Serial.printf("[WARN] WS connected %u sec ago, last cmd %lu sec ago, msgs received=%u. "
                     "DO hibernation may have broken ws binding.\n",
        (unsigned long)((millis() - (g_lastWsMsgTime > 0 ? g_lastWsMsgTime : millis())) / 1000),
        secSinceCmd, g_wsMsgCount);
    }

    // v19: 发送心跳到 DO
    if (wsConnected) sendDeviceMessage("heartbeat");
  }

  // v20: register ACK 重试 (最多 REGISTER_RETRY_MAX 次)
  if (wsConnected && !g_registerAcked && g_registerRetries < REGISTER_RETRY_MAX) {
    if (millis() - g_lastRegisterTime > REGISTER_RETRY_INTERVAL) {
      g_registerRetries++;
      g_lastRegisterTime = millis();
      Serial.printf("[WS] Register retry #%d/%d\n", g_registerRetries, REGISTER_RETRY_MAX);
      sendDeviceMessage("register");
    }
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
