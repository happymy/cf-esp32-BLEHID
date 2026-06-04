# ESP32 BLE HID 遥控器

通过 Cloudflare Worker 部署的安全网页，实时发送指令到 ESP32-C3，ESP32 以蓝牙 HID 模拟键盘/鼠标操控手机。

## 架构

```
浏览器 (登录 → Web 控制页面 · 移动端优先 Dark UI)
    ↕ WebSocket (Cookie 认证)
Cloudflare Worker (Durable Object 中继)
    ↕ WebSocket (设备令牌认证 ?token=xxx)
ESP32-C3 (WiFi + BLE HID v18)
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
    └── ble_hid_controller.ino   # ESP32-C3 v18 接收端
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
| `PASSWORD` | 网页登录密码 | `admin` |
| `COOKIE_SECRET` | Cookie 签名密钥（至少 32 字符随机串） | `change-me-to-a-random-string-at-least-32-chars` |
| `DEVICE_SECRET` | ESP32 设备令牌（须与 ESP32 代码中一致） | `esp32-blehid-device-key-change-me` |

> **安全提示**：生产环境建议使用 `npx wrangler secret put <变量名>` 替代配置文件明文存储。

### 3. 烧录 ESP32 固件

使用 Arduino IDE：

1. **安装 ESP32 开发板支持**（首选项 → 附加开发板管理器网址）
   ```
   https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
   ```

2. **安装依赖库**:
   - **NimBLE-Arduino v2.5.0** by h2zero
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
   ESP32 BLE HID v18 (single HID report, bonding + auto-reconnect)
   [BLE] MAC: ...
   [BLE] Security: bonding=ON, MITM=OFF, SC=ON (Just Works)
   [BLE] HID ready: 1
   [BLE] Advertising started
   [WiFi] Connected, IP: x.x.x.x
   [SEC] Encryption enabled, bonded=1
   [WS] Connected
   [STAT] BLE=1 ENC=1 BOND=1 WS=1 Ok=… Fail=… Disc=… Cmd=… Skip=… Heap=…
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

## BLE 连接管理

| 机制 | 说明 |
|------|------|
| **Bonding 开启** | `setSecurityAuth(true, false, true)` — 存储 LTK/IRK 至 NVS，重启后手机凭 MAC 识别设备，使用已存储的 LTK 恢复加密，无需重新配对 |
| **Just Works 配对** | `setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT)` — 首次配对自动完成，无需输入密码，与市售蓝牙键鼠体验一致 |
| **自动重连** | 手机唤醒/解锁后主动扫描已绑定设备，检测到广播后自动连接并恢复加密，NimBLE 从 NVS 恢复 CCCD 订阅，HID 通道即时就绪 |
| **单输入报告特征 (HOGP 兼容)** | `getInputReport(0)` — 单一特征承载全部 Report ID (1/2/3)，数据首字节区分报告类型。符合 BLE HOGP 规范：Report Reference 描述符 Report ID = 0 表示数据含 ID 前缀 |
| **GATT 注册时序** | `pServer->start()` 在 `getInputReport()` 之后调用，确保特征及描述符完整注册到 NimBLE 属性表 |
| **回调参照官方示例** | `onConnect` 记录状态，`onDisconnect` 调用 `NimBLEDevice::startAdvertising()`，`onAuthenticationComplete` 记录加密/绑定结果 |
| **坐标范围对齐** | 鼠标位移 clamp `[-127, 127]`，严格匹配报告描述符 `Logical Minimum = -127, Maximum = 127` |
| **Keep‑alive** | 每 30 秒发送空鼠标报告，维持 BLE 连接活跃 |

> **关键修复说明**：早期版本的三特征独立 Report ID 与 BLE HOGP 协议存在兼容性问题——当 Report Reference 描述符已声明 Report ID，但数据又包含 ID 字节时，会导致手机端 HID 解析出现偏移，所有按键/移动均错乱。当前版本采用单特征 `getInputReport(0)` 的成熟方案，从根本上消除了数据错位问题。

## 版本历史

| 版本 | 日期 | 主要变更 |
|------|------|----------|
| v19 (ESP32) | 2026-06 | 增加蓝牙状态上报|
| v18 (ESP32) | 2026-05 | 修复 HOGP 报告格式 Bug（三特征→单特征 getInputReport(0)，消除数据一字节错位）；鼠标坐标 clamp 修正为 [-127,127] |
| v17 (ESP32) | 2026-05 | 开启 bonding 实现自动重连：重启无需重新配对，锁屏后自动恢复；GATT 三特征分离修复 |
| v16 (ESP32) | 2026-05 | 彻底重写：关闭 bonding，照抄官方示例回调，移除所有自定义 BLE 恢复逻辑 |
| v15l (ESP32) | 2026-05 | 3 分钟主动断开重连修复空闲假死，getPeerDevices() 替代 disconnect(0) |
| v15k (ESP32) | 2026-05 | 移除 setOwnAddrType/setOwnAddr（消除 NPL mutex 崩溃），保留 NimBLE 2.5.0 回调适配 |
| v15j (ESP32) | 2026-05 | 尝试自动随机静态地址 → setOwnAddrType 仍触发 NPL mutex 崩溃 |
| v15i (ESP32) | 2026-05 | 尝试安全配置后移至 init() 之后 → setOwnAddr 仍触发崩溃 |
| v15h (ESP32) | 2026-05 | NimBLE 2.5.0 适配 + 固定地址 → npl_freertos_mutex_pend 断言崩溃 |
| v15g (ESP32) | 2026-05 | 消除双重广播冲突、10 分钟空闲温和恢复 |
| v15f (ESP32) | 2026-05 | 安全设置 bonding=ON，温和重连保留 LTK |
| v15e (ESP32) | 2026-05 | 长时间无指令假连接检测 |
| v15d (ESP32) | 2026-05 | 新增退格 (d) 和回车 (e) 指令 |
| v15c (ESP32) | 2026-05 | 修复 poll 边沿触发死锁、移除看门狗 |
| v15b (ESP32) | 2026-05 | 移除 60s 无指令看门狗（误断正常 BLE 连接） |
| v15 (ESP32) | 2026-05 | 死连接检测、断连超时恢复、Keep‑alive |
| v14 (ESP32) | 2026-05 | 日志规范化、VERBOSE_CMD 开关 |
| v7b (Worker) | 2026-05 | 退格/回车按钮、3×3 网格、输入框保持焦点 |
| v7a (Worker) | 2026-05 | 移除标题栏消除 iOS 缩放、状态内嵌触控板 |
| v7 (Worker) | 2026-05 | 移动端优先 UI 重写、正方形触控板、AMOLED Dark 主题 |
| v6 (Worker) | 2026-05 | DO 转发 URL 剥离 token 参数 |
| v5 (Worker) | 2026-05 | 设备令牌认证、HMAC Cookie 签名、频率限制 |


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
A: v18 已通过 bonding + 单特征 HID 报告格式从根源解决。手机唤醒/解锁后自动重连已绑定设备，HID 报告格式与 BLE HOGP 规范完全一致，数据不再错位。Keep‑alive 每 30 秒发送空报告维持连接活跃。若手机长时间锁屏后仍无响应，检查手机省电策略是否限制了蓝牙后台活动。

**Q: 触摸板单击没反应？**
A: 轻点时间需 < 260ms 且移动距离 < 0.3px，快速轻点有效。也可使用 "单击" 按钮。

**Q: 登录提示"尝试次数过多"？**
A: 密码错误超过 5 次/分钟会触发频率限制，等待 1 分钟后重试。

**Q: 修改密码后旧浏览器如何？**
A: Cookie 含密码版本指纹，密码变更后旧 Cookie 自动失效，需重新登录。

**Q: 页面在 iPhone 上显示很小？**
A: v7a 已修复此问题（移除标题栏 + `-webkit-text-size-adjust:100%`）。若仍有问题，尝试刷新页面或清除缓存。

**Q: ESP32 重启后手机需要重新配对？**
A: v18 已开启 bonding，首次配对后 LTK/IRK 存储于手机安全芯片和 ESP32 NVS。重启后手机凭 MAC 地址识别设备，自动恢复加密，无需重新配对。如需清除绑定信息，在手机蓝牙设置中"忽略此设备"即可。

## 注意事项

- **仅支持单 ESP32**——Durable Object 房间广播给所有已连接的 WebSocket 客户端。
- ESP32 需保持供电，WiFi 断开会自动重连（30s 检测间隔）。
- BLE 连接通过 bonding + Keep‑alive（30s 空报告）维持，重启或锁屏后自动恢复，无需重新配对。
- 若手机端操作无响应，检查数据是否因 HOGP 报告格式不一致而错位——v18 已通过单特征 getInputReport(0) 从根源解决。
- 若手机端出现异常（如无法连接），可在蓝牙设置中"忽略此设备"后重新配对，NVS 中的旧绑定信息将被覆盖。
- 触摸板支持手机触摸事件和桌面端鼠标拖拽。
- 桌面端操作：拖拽锁定后滑动触摸板 = 拖拽文件/选区；解锁时自动释放鼠标左键。

## 许可证

MIT License
