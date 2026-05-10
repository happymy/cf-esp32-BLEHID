/**
 * Cloudflare Worker - ESP32 BLE HID 遥控器
 * 
 * 功能:
 *   - 提供嵌入式 Web 控制页面 (响应式, 跨设备)
 *   - 通过 Durable Object + WebSocket 在浏览器与 ESP32 之间实时转发指令
 *   - 单个房间设计 (单ESP32, 简单可靠)
 * 
 * v2 新增:
 *   - 鼠标锁定按钮 (拖拽支持)
 *   - 鼠标速度滑块
 */

// ==================== 嵌入式 Web 控制页面 ====================
const HTML_PAGE = `<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no, viewport-fit=cover">
<title>ESP32 BLE HID 遥控器</title>
<style>
*{margin:0;padding:0;box-sizing:border-box}
html,body{height:100%;overflow:hidden}
body{
  font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;
  background:linear-gradient(135deg,#0f0c29,#302b63,#24243e);
  color:#e0e0e0;
  display:flex;flex-direction:column;
  align-items:center;
  user-select:none;-webkit-user-select:none;
  touch-action:manipulation;
}
.header{
  width:100%;max-width:480px;
  padding:12px 16px;
  display:flex;align-items:center;justify-content:space-between;
  background:rgba(255,255,255,0.05);
  backdrop-filter:blur(10px);
  border-bottom:1px solid rgba(255,255,255,0.1);
}
.header h1{font-size:1.1rem;font-weight:600;letter-spacing:0.5px}
.status{display:flex;align-items:center;gap:6px;font-size:0.8rem}
.status-dot{
  width:10px;height:10px;border-radius:50%;
  background:#ff4444;transition:background 0.3s;
}
.status-dot.online{background:#00e676;box-shadow:0 0 8px #00e676}
.status-dot.connecting{background:#ffaa00;animation:pulse 1s infinite}
@keyframes pulse{0%,100%{opacity:1}50%{opacity:0.3}}
.container{
  width:100%;max-width:480px;flex:1;
  display:flex;flex-direction:column;
  padding:8px 12px;gap:10px;
  overflow-y:auto;
}
.touchpad-wrap{
  flex:1;min-height:180px;
  background:rgba(255,255,255,0.06);
  border:2px solid rgba(255,255,255,0.12);
  border-radius:16px;
  position:relative;overflow:hidden;
  display:flex;align-items:center;justify-content:center;
}
.touchpad-wrap.active{border-color:rgba(0,212,255,0.6);background:rgba(0,212,255,0.05)}
.touchpad-wrap.locked{border-color:rgba(255,107,107,0.8);background:rgba(255,107,107,0.08)}
.touchpad-hint{
  color:rgba(255,255,255,0.3);font-size:0.9rem;
  pointer-events:none;text-align:center;line-height:1.6;
}
.touchpad-dot{
  position:absolute;width:8px;height:8px;border-radius:50%;
  background:#00d4ff;pointer-events:none;
  transform:translate(-50%,-50%);opacity:0;transition:opacity 0.1s;
}
.row{display:flex;gap:10px;flex-wrap:wrap}
.btn{
  flex:1;min-width:0;
  padding:14px 8px;
  border:none;border-radius:12px;
  font-size:0.95rem;font-weight:600;
  cursor:pointer;
  background:rgba(255,255,255,0.1);
  color:#fff;letter-spacing:0.5px;
  transition:all 0.15s;
  display:flex;align-items:center;justify-content:center;gap:6px;
  white-space:nowrap;
}
.btn:active{transform:scale(0.96);opacity:0.8}
.btn.accent{background:linear-gradient(135deg,#00d4ff,#0090cc);color:#000}
.btn.danger{background:linear-gradient(135deg,#ff6b6b,#e94560)}
.btn.warn{background:linear-gradient(135deg,#ffaa00,#ff8800);color:#000}
.btn.success{background:linear-gradient(135deg,#00e676,#00b248);color:#000}
.btn.locked{background:linear-gradient(135deg,#ff6b6b,#c0392b);animation:pulse-lock 0.8s infinite}
@keyframes pulse-lock{0%,100%{box-shadow:0 0 0 0 rgba(255,107,107,0.6)}50%{box-shadow:0 0 0 8px rgba(255,107,107,0)}}
.btn-icon{font-size:1.4rem}
.input-row{display:flex;gap:10px;align-items:center}
.input-row input{
  flex:1;min-width:0;
  padding:14px 16px;
  border:2px solid rgba(255,255,255,0.12);
  border-radius:12px;
  background:rgba(255,255,255,0.06);
  color:#fff;font-size:0.95rem;outline:none;
  transition:border-color 0.2s;
}
.input-row input:focus{border-color:#00d4ff}
.speed-row{
  display:flex;align-items:center;gap:10px;
  padding:8px 12px;
  background:rgba(255,255,255,0.04);
  border-radius:12px;
}
.speed-label{font-size:1.2rem;flex:0 0 auto}
.speed-slider{
  flex:1;-webkit-appearance:none;appearance:none;
  height:6px;border-radius:3px;
  background:rgba(255,255,255,0.15);
  outline:none;
}
.speed-slider::-webkit-slider-thumb{
  -webkit-appearance:none;appearance:none;
  width:24px;height:24px;border-radius:50%;
  background:#00d4ff;cursor:pointer;
  box-shadow:0 0 6px rgba(0,212,255,0.4);
}
.speed-slider::-moz-range-thumb{
  width:24px;height:24px;border-radius:50%;
  background:#00d4ff;cursor:pointer;border:none;
}
.speed-value{
  flex:0 0 36px;text-align:center;
  font-size:0.85rem;font-weight:600;color:#00d4ff;
}
.footer{
  width:100%;max-width:480px;
  padding:6px 16px 12px;
  text-align:center;font-size:0.7rem;
  color:rgba(255,255,255,0.25);
}
@media(min-width:600px){
  .container{padding:12px 0;gap:12px}
  .touchpad-wrap{min-height:220px}
}
</style>
</head>
<body>
<div class="header">
  <h1>🎮 BLE HID 遥控器</h1>
  <div class="status">
    <div class="status-dot" id="statusDot"></div>
    <span id="statusText">未连接</span>
  </div>
</div>
<div class="container">
  <!-- 触摸板 -->
  <div class="touchpad-wrap" id="touchpad">
    <div class="touchpad-hint" id="touchHint">👆 滑动移动鼠标<br>轻点 = 单击</div>
    <div class="touchpad-dot" id="touchDot"></div>
  </div>
  <!-- 核心操作 -->
  <div class="row">
    <button class="btn" id="lockBtn" onclick="toggleLock()">🔓 拖拽</button>
    <button class="btn accent" onclick="send('c')">👆 单击</button>
    <button class="btn" onclick="send('h')">🏠 桌面</button>
  </div>
  <div class="row">
    <button class="btn warn" onclick="send('s')">🔄 应用切换</button>
    <button class="btn danger" onclick="send('b')">⬅ 返回</button>
  </div>
  <!-- 鼠标速度 -->
  <div class="speed-row">
    <span class="speed-label">🐢</span>
    <input type="range" id="speedSlider" min="0.25" max="3" step="0.25" value="1">
    <span class="speed-label">🐇</span>
    <span class="speed-value" id="speedValue">1×</span>
  </div>
  <!-- 文本输入 -->
  <div class="input-row">
    <input type="text" id="textInput" placeholder="✏️ 输入要发送的文字..." enterkeyhint="send" maxlength="200">
    <button class="btn success" onclick="sendText()" style="flex:0 0 auto;padding:14px 20px">发送</button>
  </div>
</div>
<div class="footer">ESP32 BLE HID Remote · Cloudflare Worker</div>

<script>
const TOUCHPAD = document.getElementById('touchpad');
const DOT = document.getElementById('touchDot');
const HINT = document.getElementById('touchHint');
const STATUS_DOT = document.getElementById('statusDot');
const STATUS_TEXT = document.getElementById('statusText');
const TEXT_INPUT = document.getElementById('textInput');
const LOCK_BTN = document.getElementById('lockBtn');
const SPEED_SLIDER = document.getElementById('speedSlider');
const SPEED_VALUE = document.getElementById('speedValue');

let ws = null;
let reconnectTimer = null;
let touchActive = false;
let lastX = 0, lastY = 0;
let touchStartTime = 0;
let touchMoved = false;
let mouseLocked = false;
let mouseSpeed = 1.0;
const TAP_THRESHOLD = 8;
const TAP_TIME = 300;
const SEND_INTERVAL = 40;

// ---------- WebSocket ----------
function connect(){
  if(ws && ws.readyState === WebSocket.OPEN) return;
  setStatus('connecting');
  const protocol = location.protocol === 'https:' ? 'wss:' : 'ws:';
  ws = new WebSocket(protocol + '//' + location.host + '/ws');

  ws.onopen = () => {
    setStatus('online');
    clearTimeout(reconnectTimer);
  };

  ws.onclose = () => {
    setStatus('offline');
    scheduleReconnect();
  };

  ws.onerror = () => {
    ws?.close();
  };
}

function scheduleReconnect(){
  clearTimeout(reconnectTimer);
  reconnectTimer = setTimeout(connect, 3000);
}

function setStatus(state){
  STATUS_DOT.className = 'status-dot';
  switch(state){
    case 'online':
      STATUS_DOT.classList.add('online');
      STATUS_TEXT.textContent = '已连接';
      break;
    case 'connecting':
      STATUS_DOT.classList.add('connecting');
      STATUS_TEXT.textContent = '连接中...';
      break;
    default:
      STATUS_TEXT.textContent = '未连接';
  }
}

function send(action, extra={}){
  if(!ws || ws.readyState !== WebSocket.OPEN){
    setStatus('offline');
    scheduleReconnect();
    return;
  }
  const msg = JSON.stringify({a:action, ...extra});
  ws.send(msg);
}

function sendText(){
  const text = TEXT_INPUT.value.trim();
  if(!text) return;
  send('t', {d:text});
  TEXT_INPUT.value = '';
}

// ---------- 鼠标锁定 (拖拽) ----------
function toggleLock(){
  mouseLocked = !mouseLocked;
  if(mouseLocked){
    LOCK_BTN.textContent = '🔒 拖拽中';
    LOCK_BTN.classList.add('locked');
    TOUCHPAD.classList.add('locked');
    HINT.innerHTML = '🔒 拖拽模式<br>滑动 = 拖拽 | 点🔒 = 释放';
  } else {
    LOCK_BTN.textContent = '🔓 拖拽';
    LOCK_BTN.classList.remove('locked');
    TOUCHPAD.classList.remove('locked');
    HINT.innerHTML = '👆 滑动移动鼠标<br>轻点 = 单击';
  }
}

// ---------- 鼠标速度 ----------
SPEED_SLIDER.addEventListener('input', () => {
  mouseSpeed = parseFloat(SPEED_SLIDER.value);
  SPEED_VALUE.textContent = mouseSpeed.toFixed(2) + '×';
});

// ---------- 触摸板 ----------
function getPos(e){
  if(e.touches) return {x:e.touches[0].clientX, y:e.touches[0].clientY};
  return {x:e.clientX, y:e.clientY};
}

function onDown(e){
  e.preventDefault();
  touchActive = true;
  touchMoved = false;
  const p = getPos(e);
  lastX = p.x; lastY = p.y;
  touchStartTime = Date.now();
  TOUCHPAD.classList.add('active');
  HINT.style.display = 'none';
  DOT.style.opacity = '1';
  DOT.style.left = p.x - TOUCHPAD.getBoundingClientRect().left + 'px';
  DOT.style.top = p.y - TOUCHPAD.getBoundingClientRect().top + 'px';
}

function onMove(e){
  if(!touchActive) return;
  e.preventDefault();
  const p = getPos(e);
  const dx = p.x - lastX;
  const dy = p.y - lastY;
  lastX = p.x; lastY = p.y;
  if(Math.abs(dx) > 0.5 || Math.abs(dy) > 0.5){
    touchMoved = true;
    DOT.style.left = p.x - TOUCHPAD.getBoundingClientRect().left + 'px';
    DOT.style.top = p.y - TOUCHPAD.getBoundingClientRect().top + 'px';
    throttledMouseMove(Math.round(dx), Math.round(dy));
  }
}

function onUp(e){
  if(!touchActive) return;
  e.preventDefault();
  touchActive = false;
  TOUCHPAD.classList.remove('active');
  DOT.style.opacity = '0';
  const elapsed = Date.now() - touchStartTime;
  if(!touchMoved && elapsed < TAP_TIME){
    send('c');
  }
}

let lastSendTime = 0;
function throttledMouseMove(dx, dy){
  const now = Date.now();
  if(now - lastSendTime < SEND_INTERVAL) return;
  lastSendTime = now;
  // 应用速度缩放
  const sx = Math.round(dx * mouseSpeed);
  const sy = Math.round(dy * mouseSpeed);
  // 跳过极小位移
  if(Math.abs(sx) < 2 && Math.abs(sy) < 2) return;
  // 锁定模式下附带鼠标左键按下
  const msg = {a:'m', x:sx, y:sy};
  if(mouseLocked) msg.b = 1;
  send('m', msg);
}

// Touch events
TOUCHPAD.addEventListener('touchstart', onDown, {passive:false});
TOUCHPAD.addEventListener('touchmove', onMove, {passive:false});
TOUCHPAD.addEventListener('touchend', onUp, {passive:false});
TOUCHPAD.addEventListener('touchcancel', onUp, {passive:false});

// Mouse events (desktop)
TOUCHPAD.addEventListener('mousedown', onDown);
window.addEventListener('mousemove', (e)=>{
  if(touchActive) onMove(e);
});
window.addEventListener('mouseup', (e)=>{
  if(touchActive) onUp(e);
});

// Keyboard shortcut: Enter in text input
TEXT_INPUT.addEventListener('keydown', (e)=>{
  if(e.key === 'Enter'){e.preventDefault();sendText();}
});

// Initial connection
connect();
</script>
</body>
</html>`;

// ==================== Durable Object - WebSocket 房间 ====================
export class WebSocketDurableObject {
  constructor(state, env) {
    this.state = state;
    this.env = env;
  }

  // 处理初始 HTTP 请求 -> WebSocket 升级
  async fetch(request) {
    const pair = new WebSocketPair();
    const [client, server] = Object.values(pair);
    this.state.acceptWebSocket(server);
    return new Response(null, { status: 101, webSocket: client });
  }

  // 收到消息 -> 广播给所有其他连接
  async webSocketMessage(ws, message) {
    const sessions = this.state.getWebSockets();
    for (const session of sessions) {
      if (session !== ws) {
        try {
          session.send(message);
        } catch (_) {
          // 连接可能已断开, 忽略
        }
      }
    }
  }

  async webSocketClose(ws, code, reason, wasClean) {
    // 连接关闭, 无需额外清理
  }

  async webSocketError(ws, error) {
    // 连接错误, 忽略
  }
}

// ==================== Worker 入口 ====================
export default {
  async fetch(request, env) {
    const url = new URL(request.url);

    // 根路径: 返回 Web 控制页面
    if (url.pathname === '/' || url.pathname === '/index.html') {
      return new Response(HTML_PAGE, {
        headers: {
          'Content-Type': 'text/html; charset=utf-8',
          'Cache-Control': 'no-cache',
          'Content-Security-Policy': "default-src 'self'; script-src 'self' 'unsafe-inline'; style-src 'self' 'unsafe-inline'; connect-src 'self' wss: ws:;",
          'X-Content-Type-Options': 'nosniff',
        },
      });
    }

    // /ws: WebSocket 升级, 交给 Durable Object 处理
    if (url.pathname === '/ws') {
      const id = env.WS_DO.idFromName('default-room');
      const stub = env.WS_DO.get(id);
      return stub.fetch(request);
    }

    // 其他路径: 404
    return new Response('Not Found', { status: 404 });
  },
};
