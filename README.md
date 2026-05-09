# ESP32 BLE HID 遥控器

通过 Cloudflare Worker 部署的网页，实时发送指令到 ESP32，ESP32 以蓝牙 HID 模拟键盘/鼠标操控手机。

## 架构

```
浏览器 (Web 控制页面)
    ↕ WebSocket
Cloudflare Worker (Durable Object 中继)
    ↕ WebSocket
ESP32 (WiFi)
    ↕ BLE HID
手机 / 平板 (被控端)
```

## 文件清单

```
worker/
├── package.json          # Node 依赖 (wrangler)
├── wrangler.toml         # Cloudflare Worker 配置
└── src/
    └── index.js          # Worker 入口 + 嵌入式控制页面

esp32/
└── ble_hid_controller/
    └── ble_hid_controller.ino   # ESP32 接收端
```

## 快速开始

### 1. 部署 Cloudflare Worker

```bash
cd worker
npm install
# 需要登录 Cloudflare 账号, 首次运行:
# npx wrangler login
npm run deploy
```

部署后会得到域名如 `cf-esp32-blehid.YOUR_USER.workers.dev`。

### 2. 上传 ESP32 固件

使用 Arduino IDE:

1. **安装 ESP32 开发板支持** (首选项 → 附加开发板管理器网址)
   ```
   https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
   ```

2. **安装依赖库** (库管理器搜索):
   - **NimBLE-Arduino** by h2zero
   - **ArduinoJson** by Benoit Blanchon
   - **arduinoWebSockets** by Markus Sattler

3. **修改 `esp32/ble_hid_controller/ble_hid_controller.ino`**:
   ```cpp
   const char* WIFI_SSID = "你的WiFi名";
   const char* WIFI_PASS = "你的WiFi密码";
   const char* WS_HOST  = "cf-esp32-blehid.你的用户名.workers.dev";
   ```

4. 选择开发板 `ESP32 Dev Module`，编译上传。

5. 打开**串口监视器** (115200)，观察连接状态。

### 3. 配对被控设备

1. ESP32 上电后自动广播 BLE，设备名为 **"ESP32 BLE HID"**。
2. 在手机/平板蓝牙设置中配对连接。
3. 手机应显示已连接蓝牙键盘/鼠标。

### 4. 使用控制页面

手机或电脑浏览器打开 Worker 域名:

```
https://cf-esp32-blehid.你的用户名.workers.dev
```

页面状态显示 **已连接** 即可开始操控。

## 指令协议

Web 页面与 ESP32 之间通过 JSON 通信:

| 操作 | JSON | 说明 |
|------|------|------|
| 返回桌面 | `{"a":"h"}` | 消费者控制 Home |
| 返回/后退 | `{"a":"b"}` | 消费者控制 Back |
| 应用切换 | `{"a":"s"}` | 消费者 Task View + Win+Tab |
| 单击 | `{"a":"c"}` | 鼠标左键单击 |
| 鼠标移动 | `{"a":"m","x":dx,"y":dy}` | dx/dy 范围 -127~127 |
| 输入文字 | `{"a":"t","d":"hello"}` | 模拟键盘逐字符输入 |

## 注意事项

- **仅支持单 ESP32**——Worker 的 Durable Object 房间每次广播给所有连接的客户端。
- ESP32 需保持供电，WiFi 中途断开会自动重连。
- 触摸板在手机浏览器上支持触摸事件；桌面端支持鼠标拖拽。
- 个人项目默认跳过 WSS 证书校验 (`webSocket.setInsecure()`)。如需严格安全，请部署根证书校验。

## 常见问题

**Q: ESP32 串口显示 WiFi 连接失败？**
A: 检查 SSID/密码是否正确，确认路由器 2.4GHz 频段开启。

**Q: Worker 部署后页面打不开？**
A: 确认 `wrangler deploy` 无报错，域名未被墙。Cloudflare 在中国大陆可能需要备案域名。

**Q: 手机搜不到 ESP32 蓝牙？**
A: ESP32 上电后约 2-3 秒开始广播，确认串口显示 "BLE HID 服务已启动"。

**Q: 触摸板单击没反应？**
A: 轻点时间需 < 300ms 且移动距离 < 8px，快速轻点有效。也可用 "单击" 按钮。
