# ESP32 BLE HID 遥控器

通过 Cloudflare Worker 部署的安全网页，实时发送指令到 ESP32-C3，ESP32 以蓝牙 HID 模拟键盘/鼠标操控手机。

## 架构

```
浏览器 (登录 → Web 控制页面 · 移动端优先 Dark UI)
    ↕ WebSocket (Cookie 认证)
Cloudflare Worker (Durable Object 中继)
    ↕ WebSocket (设备令牌认证 ?token=xxx)
ESP32-C3 (WiFi + BLE HID v15d)
    ↕ BLE HID
手机 / 平板 (被控端)
```

## 安全特性

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
    └── index.js          # Worker v7b (登录页 + 控制页 + DO 中继)

esp32/
└── ble_hid_controller/
    └── ble_hid_controller.ino   # ESP32-C3 v15d 接收端
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

4. 推荐开发板使用 ESP32-C3-SUPER-MINI，选择开发板 **MakerGO ESP32-C3-SUPER-MINI**，**按需开启 USB CDC On Boot**，分区方案选择 **Huge APP**，编译上传。

5. 打开**串口监视器**（115200 波特率），观察连接状态。预期输出：
   ```
   ESP32 BLE HID v15d (+Backspace +Enter)
   [BLE] 广播已启动！
   [WiFi] 已连接, IP: x.x.x.x
   [WS] 开始连接 (含设备令牌)...
   [STAT] BLE=1 ENC=0 CONN=1 WS=1 …   ← WS=1 表示 WebSocket 已连接
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

1. 输入登录密码（在 Cloudflare 环境变量中设置）。
2. 登录成功后进入控制页面，触控板右上角状态指示**已连接**即可操控。

## WEB 控制页面

采用移动端优先 Dark 主题设计，无标题栏避免 iOS 动态缩放：

| 区域 | 说明 |
|------|------|
| 正方形触控板 | `aspect-ratio:1`，自适应屏幕尺寸。滑动 = 移动光标，轻点 = 单击 |
| 状态指示 | 触控板右上角半透明胶囊，显示 BLE 连接状态（未连接 / 连接中 / 已连接） |
| 操作按钮 | 3×3 Grid 布局，拇指友好 |
| 速度滑块 | 0.25× ~ 3×，调节光标移动灵敏度 |
| 文字输入栏 | 底部固定，支持退格/回车按钮辅助编辑，发送后保持焦点 |

### 按钮功能

| 按钮 | 功能 | 指令 |
|------|------|------|
| 🏠 桌面 | 返回桌面 (Home) | `h` |
| ⬅ 返回 | 后退 (Back) | `b` |
| 🔄 切换 | 应用切换器 (Task View) | `s` |
| 👆 单击 | 鼠标左键单击 | `c` |
| 🔓/🔒 拖拽 | 切换拖拽锁定模式 | 切换 `b` 标志 |
| ⌫ 退格 | 键盘 Backspace | `d` |
| ↵ 回车 | 键盘 Enter | `e` |

## 指令协议

Web 页面与 ESP32 之间通过 JSON 通信，Worker Durable Object 中继转发：

| 操作 | JSON | 说明 |
|------|------|------|
| 返回桌面 | `{"a":"h"}` | 消费者控制 Home |
| 返回/后退 | `{"a":"b"}` | 消费者控制 Back |
| 应用切换 | `{"a":"s"}` | Task View + Win+Tab |
| 单击 | `{"a":"c"}` | 鼠标左键单击 (按下→50ms→释放) |
| 退格 | `{"a":"d"}` | 键盘 Backspace (按下→30ms→释放) |
| 回车 | `{"a":"e"}` | 键盘 Enter (按下→30ms→释放) |
| 鼠标移动 | `{"a":"m","x":dx,"y":dy}` | dx/dy 范围 -127~127 |
| 拖拽移动 | `{"a":"m","x":dx,"y":dy,"b":1}` | 锁定模式下左键保持按下 |
| 输入文字 | `{"a":"t","d":"hello"}` | 模拟键盘逐字符输入 (支持 ASCII) |

## BLE 连接管理 (v15)

| 机制 | 说明 |
|------|------|
| Keep‑alive | 每 30 秒发送空鼠标报告，维持 BLE 连接活跃 |
| 死连接检测 | 连续 10 次 `notify()` 失败 → 自动重启 BLE 广播 |
| 断连超时恢复 | BLE 断开超过 30 秒 → 自动重启广播尝试恢复 |
| GATT 轮询 | 每秒检查实际连接数，电平触发避免状态死锁 |

## 版本历史

| 版本 | 日期 | 主要变更 |
|------|------|----------|
| v7b (Worker) | 2026-05 | 退格/回车按钮、3×3 网格、输入框保持焦点 |
| v7a (Worker) | 2026-05 | 移除标题栏消除 iOS 缩放、状态内嵌触控板 |
| v7 (Worker) | 2026-05 | 移动端优先 UI 重写、正方形触控板、AMOLED Dark 主题 |
| v6 (Worker) | 2026-05 | DO 转发 URL 剥离 token 参数 |
| v5 (Worker) | 2026-05 | 设备令牌认证、HMAC Cookie 签名、频率限制 |
| v15d (ESP32) | 2026-05 | 新增退格 (d) 和回车 (e) 指令 |
| v15c (ESP32) | 2026-05 | 修复 poll 边沿触发死锁、移除看门狗 |
| v15b (ESP32) | 2026-05 | 移除 60s 无指令看门狗（误断正常 BLE 连接） |
| v15 (ESP32) | 2026-05 | 死连接检测、断连超时恢复、Keep‑alive |
| v14 (ESP32) | 2026-05 | 日志规范化、VERBOSE_CMD 开关 |

## 常见问题

**Q: ESP32 串口显示 WiFi 连接失败？**
A: 检查 SSID/密码是否正确，确认路由器 2.4GHz 频段开启。

**Q: Worker 部署后页面打不开？**
A: 确认 `wrangler deploy` 无报错。Cloudflare 在中国大陆可能需要备案域名。

**Q: 手机搜不到 ESP32 蓝牙？**
A: ESP32 上电后约 2-3 秒开始广播，确认串口显示 "BLE HID Service UUID 已加入广播数据"。

**Q: WebSocket 始终显示未连接 (WS=0)？**
A: 检查三点——① `DEVICE_SECRET` 环境变量是否与 ESP32 中 `WS_PATH` 的 token 一致；② 域名是否正确；③ 路由器是否允许 443 端口出站。

**Q: 一段时间不操作后手机无反应？**
A: v15 已修复此问题。若仍出现，检查串口 `[STAT]` 行 BLE= 是否为 1。若为 0 且 CONN=1，可能是加密未完成，等待手机端重新配对。

**Q: 触摸板单击没反应？**
A: 轻点时间需 < 260ms 且移动距离 < 0.3px，快速轻点有效。也可使用 "单击" 按钮。

**Q: 登录提示"尝试次数过多"？**
A: 密码错误超过 5 次/分钟会触发频率限制，等待 1 分钟后重试。

**Q: 修改密码后旧浏览器如何？**
A: Cookie 含密码版本指纹，密码变更后旧 Cookie 自动失效，需重新登录。

**Q: 页面在 iPhone 上显示很小？**
A: v7a 已修复此问题（移除标题栏 + `-webkit-text-size-adjust:100%`）。若仍有问题，尝试刷新页面或清除缓存。

## 注意事项

- **仅支持单 ESP32**——Durable Object 房间广播给所有已连接的 WebSocket 客户端。
- ESP32 需保持供电，WiFi 断开会自动重连（30s 检测间隔）。
- BLE 连接通过 Keep‑alive 维持，闲置不会主动断开。
- 触摸板支持手机触摸事件和桌面端鼠标拖拽。
- 桌面端操作：拖拽锁定后滑动触摸板 = 拖拽文件/选区；解锁时自动释放鼠标左键。

## 许可证

MIT License
