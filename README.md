# ESP32 BLE HID 遥控器

通过 Cloudflare Worker 部署的安全网页，实时发送指令到 ESP32-C3，ESP32 以蓝牙 HID 模拟键盘/鼠标操控手机。

## 架构

```
浏览器 (登录 → Web 控制页面)
    ↕ WebSocket (Cookie 认证)
Cloudflare Worker (Durable Object 中继)
    ↕ WebSocket (设备令牌认证 ?token=xxx)
ESP32-C3 (WiFi + BLE)
    ↕ BLE HID
手机 / 平板 (被控端)
```

## 安全特性 (v5)

| 特性 | 说明 |
|------|------|
| 密码登录 | 浏览器访问须输入密码，验证通过后设置 HMAC 签名 Cookie |
| Cookie 签名 | HMAC-SHA256 + 密码版本指纹，密码变更后旧 Cookie 自动失效 |
| 设备令牌 | ESP32 通过 URL 查询参数 `?token=xxx` 认证，无需 Cookie |
| 登录频率限制 | 每 IP 每分钟最多 5 次尝试，超限返回 429 |
| 内容安全策略 | CSP + X-Content-Type-Options 头 |

## 文件清单

```
worker/
├── package.json          # Node 依赖 (wrangler)
├── wrangler.toml         # Cloudflare Worker 配置 + 环境变量
└── src/
    └── index.js          # Worker v5 (登录页 + 控制页 + DO 中继)

esp32/
└── ble_hid_controller/
    └── ble_hid_controller.ino   # ESP32-C3 v12 接收端
```

## 快速开始

### 1. 部署 Cloudflare Worker

```bash
cd worker
npm install
# 首次部署需要登录 Cloudflare:
# npx wrangler login
npm run deploy
```

部署后获得域名 `cf-esp32-blehid.YOUR_USER.workers.dev`。

### 2. 配置环境变量

在 Cloudflare Dashboard → Workers & Pages → cf-esp32-blehid → Settings → Variables 中设置：

| 变量 | 说明 | 示例 |
|------|------|------|
| `PASSWORD` | 网页登录密码 | `MySecurePass123` |
| `COOKIE_SECRET` | Cookie 签名密钥（至少 32 字符随机串） | `cbK4zO9lX7gM9lE7vQcM3J4i0eE6A9xC3` |
| `DEVICE_SECRET` | ESP32 设备令牌（须与 ESP32 代码中一致） | `2807C41E-ABFF-A081-64B5-30EDA26EE282` |

> **安全提示**：生产环境建议使用 `npx wrangler secret put <变量名>` 替代配置文件明文存储。

### 3. 烧录 ESP32 固件

使用 Arduino IDE：

1. **安装 ESP32 开发板支持**（首选项 → 附加开发板管理器网址）
   ```
   https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
   ```

2. **安装依赖库**（库管理器搜索）:
   - **NimBLE-Arduino** by h2zero
   - **ArduinoJson** by Benoit Blanchon
   - **arduinoWebSockets** by Markus Sattler

3. **修改 `esp32/ble_hid_controller/ble_hid_controller.ino` 中的配置**:
   ```cpp
   const char* WIFI_SSID = "你的WiFi名";
   const char* WIFI_PASS = "你的WiFi密码";
   const char* WS_HOST  = "cf-esp32-blehid.你的用户名.workers.dev";
   const char* WS_PATH  = "/ws?token=你的设备令牌";  // 须与 DEVICE_SECRET 一致
   ```

4. 选择开发板 **MakerGO ESP32-C3-SUPER-MINI**（或其他 ESP32开发板，推荐使用ESP32-C3-SUPER-MINI），**开启 USB CDC On Boot**，分区方案选择 **Huge APP**，编译上传。

5. 打开**串口监视器**（115200 波特率），观察连接状态。预期输出：
   ```
   ESP32 BLE HID v12 (WebSocket auth)
   [BLE] 广播已启动！
   [WiFi] 已连接, IP: x.x.x.x
   [WS] 开始连接 (含设备令牌)...
   [STAT] BLE=1 CONN=1 WS=1 ...   ← WS=1 表示 WebSocket 已连接
   ```

### 4. 配对被控设备

1. ESP32 上电后自动广播 BLE，设备名 **"ESP32 BLE Remote"**。
2. 在手机/平板蓝牙设置中配对连接。
3. 手机应显示蓝牙键盘/鼠标已连接。

### 5. 使用控制页面

手机或电脑浏览器打开 Worker 域名：

```
https://cf-esp32-blehid.你的用户名.workers.dev
```

1. 输入登录密码（在 `wrangler.toml` 或 Cloudflare 环境变量中设置）。
2. 登录成功后进入控制页面，状态显示 **已连接** 即可操控。

## 指令协议

Web 页面与 ESP32 之间通过 JSON 通信，Worker Durable Object 中继转发：

| 操作 | JSON | 说明 |
|------|------|------|
| 返回桌面 | `{"a":"h"}` | 消费者控制 Home |
| 返回/后退 | `{"a":"b"}` | 消费者控制 Back |
| 应用切换 | `{"a":"s"}` | Task View + Win+Tab |
| 单击 | `{"a":"c"}` | 鼠标左键单击 (按下→50ms→释放) |
| 鼠标移动 | `{"a":"m","x":dx,"y":dy}` | dx/dy 范围 -127~127 |
| 拖拽移动 | `{"a":"m","x":dx,"y":dy,"b":1}` | 锁定模式下左键保持按下 |
| 输入文字 | `{"a":"t","d":"hello"}` | 模拟键盘逐字符输入 (支持 ASCII) |

## 版本历史

| 版本 | 日期 | 主要变更 |
|------|------|----------|
| v5 (Worker) | 2026-05 | 设备令牌认证、HMAC Cookie 签名、频率限制、惰性清理 |
| v4 (Worker) | 2026-05 | Cookie HMAC-SHA256 签名、密码版本指纹 |
| v3 (Worker) | 2026-05 | 密码登录认证、解锁释放鼠标、重复 a 字段修复 |
| v12 (ESP32) | 2026-05 | 设备令牌查询参数、移除不存在的 setInsecure() |
| v11 (ESP32) | 2026-05 | 设备令牌认证支持 |
| v10 (ESP32) | 2026-05 | 类型安全转换 as<uint8_t>()、JSON 容量 512 字节 |

## 常见问题

**Q: ESP32 串口显示 WiFi 连接失败？**
A: 检查 SSID/密码是否正确，确认路由器 2.4GHz 频段开启。

**Q: Worker 部署后页面打不开？**
A: 确认 `wrangler deploy` 无报错。Cloudflare 在中国大陆可能需要备案域名。

**Q: 手机搜不到 ESP32 蓝牙？**
A: ESP32 上电后约 2-3 秒开始广播，确认串口显示 "BLE HID Service UUID 已加入广播数据"。

**Q: WebSocket 始终显示未连接 (WS=0)？**
A: 检查三点——① `DEVICE_SECRET` 环境变量是否与 ESP32 中 `WS_PATH` 的 token 一致；② 域名是否正确；③ 路由器是否允许 443 端口出站。

**Q: 触摸板单击没反应？**
A: 轻点时间需 < 300ms 且移动距离 < 0.5px，快速轻点有效。也可使用 "单击" 按钮。

**Q: 登录提示"尝试次数过多"？**
A: 密码错误超过 5 次/分钟会触发频率限制，等待 1 分钟后重试。

**Q: 修改密码后旧浏览器如何？**
A: Cookie 含密码版本指纹，密码变更后旧 Cookie 自动失效，需重新登录。

## 注意事项

- **仅支持单 ESP32**——Durable Object 房间广播给所有已连接的 WebSocket 客户端。
- ESP32 需保持供电，WiFi 断开会自动重连（30s 检测间隔）。
- 触摸板支持手机触摸事件和桌面端鼠标拖拽。
- 桌面端操作：拖拽锁定后滑动触摸板 = 拖拽文件/选区；解锁时自动释放鼠标左键。

## 许可证

MIT License
