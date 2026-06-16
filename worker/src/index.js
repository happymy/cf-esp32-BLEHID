/**
 * Cloudflare Worker - ESP32 BLE HID 遥控器 (v38)
 *
 * ========== 版本更新日志 ==========
 * v38 (2026-06-04): MAX_DRAIN_SIZE=3 心跳排空限流；修复 MAX_DRAIN_SIZE 定义顺序
 *                    （register 分支先于 if(espDeviceId) 块执行导致 ReferenceError）
 * v37: 活跃窗口高频拉取（10s 窗口 / 1s 间隔 / 最多 6 次 ping）
 * v36: alarm 触发 drainCommands 排空待发命令
 * v35: DO hibernation 后通过 heartbeat/ping 重新绑定 ws
 * v34: WebSocket 接收缓冲区增至 4096 字节
 * v33: 命令队列上限 MAX_CMD_QUEUE=20
 * v32: 多设备切换支持（基于 deviceId 路由命令）
 * v28: 入站消息摘要日志（type/from/to/cmd）
 * v25: deviceId 区分 ESP32 与浏览器消息
 * ================================
 */

import { DurableObject } from 'cloudflare:workers';

// ==================== 认证辅助 ====================

async function passwordFingerprint(password) {
  const encoder = new TextEncoder();
  const data = encoder.encode(password);
  const hashBuffer = await crypto.subtle.digest('SHA-256', data);
  const hashArray = Array.from(new Uint8Array(hashBuffer));
  return hashArray.slice(0, 4).map(b => b.toString(16).padStart(2, '0')).join('');
}

async function hmacSign(keyStr, message) {
  const encoder = new TextEncoder();
  const keyData = encoder.encode(keyStr);
  const msgData = encoder.encode(message);
  const key = await crypto.subtle.importKey(
    'raw', keyData, { name: 'HMAC', hash: 'SHA-256' }, false, ['sign']
  );
  const sig = await crypto.subtle.sign('HMAC', key, msgData);
  return Array.from(new Uint8Array(sig)).map(b => b.toString(16).padStart(2, '0')).join('');
}

async function makeAuthToken(password, secret) {
  const version = await passwordFingerprint(password);
  const signature = await hmacSign(secret, version);
  return version + ':' + signature;
}

function getAuthCookie(request) {
  const cookieHeader = request.headers.get('Cookie') || '';
  const pairs = cookieHeader.split(';').map(c => c.trim());
  for (const pair of pairs) {
    if (pair.startsWith('auth_token=')) {
      return decodeURIComponent(pair.substring(11));
    }
  }
  return null;
}

async function verifyCookieAuth(request, env) {
  const token = getAuthCookie(request);
  if (!token) return false;
  const colonIdx = token.indexOf(':');
  if (colonIdx < 0) return false;
  const version = token.substring(0, colonIdx);
  const signature = token.substring(colonIdx + 1);
  const expectedVersion = await passwordFingerprint(env.PASSWORD || 'admin');
  if (version !== expectedVersion) return false;
  const expectedSig = await hmacSign(env.COOKIE_SECRET || 'fallback-secret', version);
  return signature === expectedSig;
}

function verifyDeviceToken(request, env) {
  const url = new URL(request.url);
  const token = url.searchParams.get('token');
  if (!token) return false;
  const expected = env.DEVICE_SECRET || 'esp32-default-token';
  return token === expected;
}

async function verifyWsAuth(request, env) {
  if (verifyDeviceToken(request, env)) return true;
  return await verifyCookieAuth(request, env);
}

async function setAuthCookie(response, password, secret) {
  const token = await makeAuthToken(password, secret);
  response.headers.set(
    'Set-Cookie',
    `auth_token=${token}; Path=/; HttpOnly; Secure; SameSite=Strict; Max-Age=86400`
  );
  return response;
}

function corsResponse(body, status = 200) {
  return new Response(body, {
    status,
    headers: {
      'Content-Type': 'application/json; charset=utf-8',
      'Access-Control-Allow-Origin': '*',
      'Access-Control-Allow-Methods': 'POST, OPTIONS',
      'Access-Control-Allow-Headers': 'Content-Type',
    },
  });
}

// ==================== 登录频率限制 (Durable Object 全局计数器) ====================

// ==================== 登录页面 ====================
const LOGIN_PAGE = `<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1,minimum-scale=1,user-scalable=no,viewport-fit=cover">
<meta name="theme-color" content="#08080f">
<title>BLE HID</title>
<style>
*,*::before,*::after{margin:0;padding:0;box-sizing:border-box}
html,body{height:100%;height:100dvh;-webkit-text-size-adjust:100%;-ms-text-size-adjust:100%}
body{
  font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;
  background:#08080f;color:#e4e4e7;
  display:flex;align-items:center;justify-content:center;
  padding:env(safe-area-inset-top,0) 20px env(safe-area-inset-bottom,0);
}
.card{
  width:100%;max-width:340px;
  border:1px solid rgba(99,102,241,0.12);
  border-radius:20px;padding:36px 24px 28px;
  text-align:center;
  background:linear-gradient(160deg,rgba(99,102,241,0.06),rgba(139,92,246,0.02));
  animation:fadeUp .5s cubic-bezier(.4,0,.2,1);
}
@keyframes fadeUp{from{opacity:0;transform:translateY(16px)}to{opacity:1;transform:translateY(0)}}
.card .icon{font-size:2.4rem;margin-bottom:8px}
.card h1{font-size:1.1rem;font-weight:700;margin-bottom:2px;letter-spacing:-.2px}
.card .sub{font-size:.75rem;color:rgba(255,255,255,.28);margin-bottom:24px}
.card input{
  width:100%;padding:13px 14px;
  border:1.5px solid rgba(255,255,255,.06);
  border-radius:12px;
  background:rgba(255,255,255,.03);
  color:#fff;font-size:.9rem;outline:none;
  text-align:center;letter-spacing:5px;
  transition:all .2s;
}
.card input:focus{
  border-color:#818cf8;
  box-shadow:0 0 0 3px rgba(99,102,241,.12);
}
.card .err{color:#f87171;font-size:.72rem;margin-top:8px;min-height:18px}
.card button{
  width:100%;margin-top:12px;padding:13px;
  border:none;border-radius:12px;
  background:linear-gradient(135deg,#6366f1,#7c3aed);
  color:#fff;font-size:.9rem;font-weight:600;
  cursor:pointer;letter-spacing:.5px;
  box-shadow:0 4px 18px rgba(99,102,241,.25);
  transition:all .15s;
}
.card button:active{transform:scale(.97);opacity:.85}
.card button:disabled{opacity:.4;transform:none}
</style>
</head>
<body>
<div class="card">
  <div class="icon">🎮</div>
  <h1>BLE HID 遥控器</h1>
  <div class="sub">输入密码以继续</div>
  <form id="f">
    <input type="password" id="p" placeholder="••••••" autocomplete="current-password" autofocus>
    <div class="err" id="e"></div>
    <button type="submit" id="b">登 录</button>
  </form>
</div>
<script>
f.onsubmit=async e=>{
  e.preventDefault();
  var v=p.value.trim();
  if(!v){e.textContent='请输入密码';return}
  b.disabled=true;b.textContent='验证中...';e.textContent='';
  try{
    var r=await fetch('/api/auth',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({password:v})});
    if(r.ok)location='/';
    else{var d=await r.json();e.textContent=d.error||'密码错误';p.value='';p.focus()}
  }catch(ex){e.textContent='网络错误'}
  b.disabled=false;b.textContent='登录'
}
</script>
</body>
</html>`;

// ==================== 主控制页面 v8 (多设备切换) ====================
const HTML_PAGE = `<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1,minimum-scale=1,user-scalable=no,viewport-fit=cover,shrink-to-fit=no">
<meta name="theme-color" content="#08080f">
<meta name="apple-mobile-web-app-capable" content="yes">
<meta name="apple-mobile-web-app-status-bar-style" content="black-translucent">
<title>BLE HID</title>
<style>
*,*::before,*::after{margin:0;padding:0;box-sizing:border-box}
:root{
  --bg:#08080f;
  --surface:#101018;
  --surface2:#181828;
  --border:rgba(255,255,255,.05);
  --accent:#818cf8;
  --accent2:#6366f1;
  --success:#34d399;
  --danger:#f87171;
  --text:#e4e4e7;
  --text2:rgba(255,255,255,.55);
  --text3:rgba(255,255,255,.25);
  --rad:16px;
  --rad-sm:10px;
  --safe-b:env(safe-area-inset-bottom,0px);
  --safe-t:env(safe-area-inset-top,0px);
}
html,body{
  height:100%;height:100dvh;
  overflow:hidden;
  overscroll-behavior:none;
  -webkit-text-size-adjust:100%;
  -ms-text-size-adjust:100%;
}
body{
  font-family:-apple-system,BlinkMacSystemFont,'SF Pro Text','Segoe UI',Roboto,sans-serif;
  background:var(--bg);
  color:var(--text);
  display:flex;flex-direction:column;
  user-select:none;-webkit-user-select:none;
  touch-action:manipulation;
  -webkit-tap-highlight-color:transparent;
  -webkit-font-smoothing:antialiased;
}

/* ===== DEVICE BAR ===== */
.devbar{
  display:flex;align-items:center;gap:6px;
  width:min(calc(100vw - 20px), 380px);
  margin:calc(2px + var(--safe-t)) auto 0;
  padding:5px 10px;
  border-radius:var(--rad-sm);
  background:var(--surface);
  border:1px solid var(--border);
  flex-shrink:0;
}
.devbar-label{font-size:.68rem;color:var(--text3);flex:0 0 auto;white-space:nowrap}
.devbar-select{
  flex:1;padding:5px 8px;
  border:1.5px solid var(--border);
  border-radius:8px;
  background:var(--surface2);
  color:var(--text);font-size:.7rem;
  outline:none;
  -webkit-appearance:none;appearance:none;
}
.devbar-select:focus{border-color:rgba(129,140,248,.4)}
.devbar-select option{background:var(--surface2);color:var(--text)}
.devbar-indicator{font-size:.65rem;flex:0 0 auto;min-width:16px;text-align:center}

/* ===== MAIN ===== */
.main{
  flex:1;
  display:flex;flex-direction:column;
  align-items:center;justify-content:center;
  padding:4px 10px 4px;
  gap:6px;
  min-height:0;
}

/* ===== TOUCHPAD (正方形) ===== */
.tpad{
  width:min(calc(100vw - 20px), calc(100dvh - 300px));
  max-width:380px;
  aspect-ratio:1;
  background:radial-gradient(ellipse at center, rgba(99,102,241,.04) 0%, transparent 70%);
  border:2px solid var(--border);
  border-radius:var(--rad);
  position:relative;overflow:hidden;
  flex-shrink:1;min-height:0;
  transition:border-color .2s, background .2s;
}
.tpad.active{border-color:rgba(129,140,248,.45)}
.tpad.locked{border-color:rgba(248,113,113,.35)}
.tpad.flash{animation:flash .25s ease}
@keyframes flash{
  0%{box-shadow:inset 0 0 0 0 rgba(129,140,248,0)}
  50%{box-shadow:inset 0 0 60px 15px rgba(129,140,248,.08)}
  100%{box-shadow:inset 0 0 0 0 rgba(129,140,248,0)}
}

/* ---- 状态指示 (触控板内部右上角) ---- */
.tpad-status{
  position:absolute;top:10px;right:12px;
  display:flex;align-items:center;gap:5px;
  padding:3px 8px;border-radius:10px;
  background:rgba(8,8,15,.7);
  backdrop-filter:blur(8px);-webkit-backdrop-filter:blur(8px);
  border:1px solid var(--border);
  pointer-events:none;z-index:5;
}
.tpad-dot{
  width:6px;height:6px;border-radius:50%;
  background:var(--danger);transition:all .3s;
}
.tpad-dot.on{background:var(--success);box-shadow:0 0 8px rgba(52,211,153,.5)}
.tpad-dot.connecting{background:#fbbf24;animation:blink .8s infinite}
@keyframes blink{0%,100%{opacity:1}50%{opacity:.2}}
.tpad-stxt{font-size:.6rem;font-weight:600;color:var(--text2);white-space:nowrap}

/* ---- 触摸提示 ---- */
.tpad-hint{
  position:absolute;inset:0;
  display:flex;align-items:center;justify-content:center;
  color:var(--text3);font-size:.72rem;
  text-align:center;line-height:1.8;
  pointer-events:none;
  transition:opacity .2s;
}

/* ---- 触摸光斑 ---- */
.tpad-dot-cursor{
  position:absolute;pointer-events:none;
  width:50px;height:50px;border-radius:50%;
  background:radial-gradient(circle,rgba(129,140,248,.3) 0%,transparent 70%);
  transform:translate(-50%,-50%);
  opacity:0;transition:opacity .1s;
}
.tpad-dot-cursor::after{
  content:'';position:absolute;top:50%;left:50%;
  width:9px;height:9px;border-radius:50%;
  background:var(--accent);
  transform:translate(-50%,-50%);
  box-shadow:0 0 10px rgba(129,140,248,.6);
}

/* ---- 拖拽标签 ---- */
.lock-tag{
  position:absolute;top:10px;left:10px;
  padding:3px 7px;border-radius:8px;
  background:rgba(248,113,113,.18);
  color:var(--danger);font-size:.58rem;font-weight:700;
  pointer-events:none;
  opacity:0;transition:opacity .2s;
}
.tpad.locked .lock-tag{opacity:1}

/* ===== ACTION GRID ===== */
.actions{
  display:grid;
  grid-template-columns:repeat(3,1fr);
  gap:6px;
  width:min(calc(100vw - 20px), 380px);
  flex-shrink:0;
}
.btn{
  padding:10px 4px;
  border:1px solid var(--border);
  border-radius:var(--rad-sm);
  background:var(--surface);
  color:var(--text);
  font-size:.72rem;font-weight:600;
  cursor:pointer;
  text-align:center;
  letter-spacing:-.1px;
  transition:all .15s;
  white-space:nowrap;
  -webkit-tap-highlight-color:transparent;
}
.btn:active{transform:scale(.95);background:var(--surface2)}
.btn.accent{
  background:linear-gradient(135deg,var(--accent2),#7c3aed);
  color:#fff;border:none;
  box-shadow:0 2px 10px rgba(99,102,241,.2);
}
.btn.accent:active{opacity:.85}
.btn.danger{color:var(--danger);border-color:rgba(248,113,113,.15)}
.btn.danger:active{background:rgba(248,113,113,.08)}
.btn.ctr{grid-column:2/3}

/* ===== SPEED ===== */
.speed{
  display:flex;align-items:center;gap:8px;
  width:min(calc(100vw - 20px), 380px);
  padding:4px 10px;
  border-radius:var(--rad-sm);
  background:var(--surface);
  border:1px solid var(--border);
  flex-shrink:0;
}
.speed .ico{font-size:.85rem;flex:0 0 auto;opacity:.6}
.speed input[type=range]{
  flex:1;-webkit-appearance:none;appearance:none;
  height:4px;border-radius:2px;
  background:rgba(255,255,255,.06);
  outline:none;
}
.speed input[type=range]::-webkit-slider-thumb{
  -webkit-appearance:none;appearance:none;
  width:20px;height:20px;border-radius:50%;
  background:var(--accent);
  border:2px solid var(--bg);
  box-shadow:0 0 8px rgba(129,140,248,.35);
  cursor:pointer;
  transition:transform .12s;
}
.speed input[type=range]::-webkit-slider-thumb:active{transform:scale(1.2)}
.speed-val{
  flex:0 0 38px;text-align:center;
  font-size:.72rem;font-weight:700;color:var(--accent);
  font-variant-numeric:tabular-nums;
}

/* ===== BOTTOM INPUT ===== */
.bottom{
  flex:0 0 auto;
  padding:6px 10px calc(6px + var(--safe-b));
  display:flex;gap:8px;align-items:center;
  background:rgba(8,8,15,.85);
  backdrop-filter:blur(16px);-webkit-backdrop-filter:blur(16px);
  border-top:1px solid var(--border);
}
.bottom input{
  flex:1;padding:10px 12px;
  border:1.5px solid var(--border);
  border-radius:var(--rad-sm);
  background:var(--surface);
  color:var(--text);font-size:.82rem;outline:none;
  transition:all .2s;
}
.bottom input:focus{
  border-color:rgba(129,140,248,.4);
  box-shadow:0 0 0 3px rgba(99,102,241,.1);
}
.bottom input::placeholder{color:var(--text3)}
.bottom .send{
  flex:0 0 auto;
  padding:10px 18px;
  border:none;border-radius:var(--rad-sm);
  background:linear-gradient(135deg,var(--accent2),#7c3aed);
  color:#fff;font-size:.82rem;font-weight:700;
  cursor:pointer;letter-spacing:.3px;
  box-shadow:0 2px 10px rgba(99,102,241,.2);
  transition:all .12s;
}
.bottom .send:active{transform:scale(.95);opacity:.85}

/* ===== RESPONSIVE ===== */
@media(min-width:500px){
  .devbar{max-width:420px}
  .tpad{max-width:420px}
  .actions{max-width:420px}
  .speed{max-width:420px}
}
@media(max-height:650px){
  .tpad{width:min(calc(100vw - 20px), calc(100dvh - 270px));max-width:300px}
  .actions{max-width:300px;gap:4px}
  .speed{max-width:300px}
  .devbar{max-width:300px}
  .btn{padding:8px 3px;font-size:.68rem}
  .main{gap:4px}
}
@media(prefers-reduced-motion:reduce){
  *,*::before,*::after{animation-duration:.01ms!important;transition-duration:.01ms!important}
}
</style>
</head>
<body>

<!-- DEVICE SELECTOR BAR (永远可见) -->
<div class="devbar" id="dv">
  <span class="devbar-label">设备:</span>
  <select class="devbar-select" id="ds"><option value="">未选择设备</option></select>
  <span class="devbar-indicator" id="di"></span>
</div>

<!-- MAIN (无 Header) -->
<div class="main">

  <!-- SQUARE TOUCHPAD -->
  <div class="tpad" id="tp">
    <div class="tpad-status">
      <div class="tpad-dot" id="sd"></div>
      <span class="tpad-stxt" id="st">未连接</span>
    </div>
    <div class="lock-tag" id="lt">🔒 拖拽</div>
    <div class="tpad-hint" id="th">滑动 = 光标<br>轻点 = 单击</div>
    <div class="tpad-dot-cursor" id="td"></div>
  </div>

  <!-- ACTION GRID (3×3, 7 buttons) -->
  <div class="actions">
    <button class="btn" onclick="SC('h')">🏠 桌面</button>
    <button class="btn" onclick="SC('b')">⬅ 返回</button>
    <button class="btn" onclick="SC('s')">🔄 切换</button>
    <button class="btn accent" onclick="SC('c')">👆 单击</button>
    <button class="btn" id="lb" onclick="TL()">🔓 拖拽</button>
    <button class="btn" onclick="SC('d')">⌫ 退格</button>
    <button class="btn ctr" onclick="SC('e')">↵ 回车</button>
  </div>

  <!-- SPEED -->
  <div class="speed">
    <span class="ico">🐢</span>
    <input type="range" id="ss" min="0.25" max="10" step="0.25" value="1">
    <span class="ico">🐇</span>
    <span class="speed-val" id="sv">1×</span>
  </div>

</div>

<!-- BOTTOM -->
<div class="bottom">
  <input type="text" id="ti" placeholder="输入文字..." enterkeyhint="send" maxlength="200" autocomplete="off" autocorrect="off" autocapitalize="off">
  <button class="send" id="sb" onclick="ST()">发送</button>
</div>

<script>
// DOM
var TP=document.getElementById('tp'),TD=document.getElementById('td'),
    TH=document.getElementById('th'),SD=document.getElementById('sd'),
    ST_=document.getElementById('st'),LT=document.getElementById('lt'),
    LB=document.getElementById('lb'),TI=document.getElementById('ti'),
    SS=document.getElementById('ss'),SV=document.getElementById('sv'),
    DV=document.getElementById('dv'),DS=document.getElementById('ds'),
    DI=document.getElementById('di');

// State
var ws=null,rt=null,ta=false,lx=0,ly=0,ts=0,tm=false,ml=false,sp=1,rc=0;
var TAP=260,INT=35,lst=0;
var selectedDeviceId=null,devices={};

// Device selector
DS.onchange=function(){selectedDeviceId=DS.value||null;refreshStatus()};
function updateDeviceList(list){
  devices={};
  if(!list)list=[];
  var cur=selectedDeviceId,hasCur=false;
  DS.innerHTML='<option value="">未选择设备</option>';
  DV.style.display='flex';
  for(var i=0;i<list.length;i++){
    var d=list[i];devices[d.id]=d;
    var s=(d.online?'🟢 ':'🔴 ')+d.name+(d.connected?' [BLE 已连接]':' [BLE 未连接]');
    var o=document.createElement('option');o.value=d.id;o.textContent=s;DS.appendChild(o);
    if(d.id===cur)hasCur=true;
  }
  // v8-fix: 收到空列表时保留已选设备 (DO 重启后 ESP32 尚未注册的窗口期)
  if(list.length===0)return;
  if(!hasCur){
    // 之前选中的设备不在列表中，自动选第一个可用设备
    selectedDeviceId=list[0].id;
    DS.value=selectedDeviceId;
  }else{
    DS.value=cur;
  }
  DI.textContent=(Object.keys(devices).length||'0')+'台';
  refreshStatus();
}
function removeDevice(id){
  delete devices[id];var o=DS.querySelector('option[value="'+id+'"]');if(o)o.remove();
  if(selectedDeviceId===id){selectedDeviceId=null;DS.value=''}
  DI.textContent=(Object.keys(devices).length||'0')+'台';
  refreshStatus();
}

// WebSocket
var _wsOpenTs=0,_sentCount=0;  // v28: 诊断用
function CN(){
  // v8-fix: 检测旧连接状态，DO 可能已被回收但浏览器端仍认为 OPEN
  if(ws&&(ws.readyState===WebSocket.OPEN||ws.readyState===WebSocket.CONNECTING)){
    try{ws.send(JSON.stringify({type:'list-devices'}));}catch(_){console.error('[WS] Stale ws send failed, reconnecting');ws=null;SR()}
    return;
  }
  US('connecting');
  var p=location.protocol==='https:'?'wss:':'ws:';
  ws=new WebSocket(p+'//'+location.host+'/ws');
  _wsOpenTs=0;_sentCount=0;rc=0;
  ws.onopen=function(){
    _wsOpenTs=Date.now();
    console.log('[WS] Opened');
    refreshStatus();if(rt){clearTimeout(rt);rt=null}
    ws.send(JSON.stringify({type:'list-devices'}));
  };
  ws.onclose=function(e){
    var aliveMs=_wsOpenTs?(Date.now()-_wsOpenTs):0;
    console.warn('[WS] Closed code='+e.code+' reason="'+e.reason+'" alive='+(aliveMs/1000).toFixed(0)+'s sent='+_sentCount);
    _wsOpenTs=0;
    refreshStatus();SR();
  };
  ws.onerror=function(){
    console.error('[WS] Error readyState='+(ws?ws.readyState:'null'));
    try{ws.close()}catch(_){}ws=null;SR();
  };
  ws.onmessage=function(e){
    try{var m=JSON.parse(e.data);
      if(m.type==='device-list'){updateDeviceList(m.devices);}
      else if(m.type==='device-offline'){console.warn('[WS] Device offline: '+m.deviceId);removeDevice(m.deviceId);}
    }catch(_){console.warn('[WS] Bad message: '+(typeof e.data==='string'?e.data.slice(0,60):'binary'))}
  };
}
function SR(){if(!rt){var d=Math.min(3000*Math.pow(2,rc),30000);rc++;rt=setTimeout(CN,d)}}
function US(s){
  SD.className='tpad-dot';
  if(s==='online'){
    SD.classList.add('on');
    ST_.textContent=selectedDeviceId?'已连接':'在线(未选设备)';
  }else if(s==='connecting'){
    SD.classList.add('connecting');ST_.textContent='连接中';
  }else{
    ST_.textContent='未连接';
  }
}
function refreshStatus(){
  if(!ws||ws.readyState!==WebSocket.OPEN){US('offline');return}
  US('online');
}
function SC(a,x){
  x=x||{};
  if(!ws||ws.readyState!==WebSocket.OPEN){
    console.warn('[CMD] ws not OPEN (readyState='+(ws?ws.readyState:'null')+'), reconnecting...');
    US('offline');SR();return;
  }
  if(!selectedDeviceId){
    TH.innerHTML='请先选择设备';TH.style.color='rgba(248,113,113,.7)';TH.style.opacity='1';
    setTimeout(function(){TH.innerHTML='滑动 = 光标<br>轻点 = 单击';TH.style.color='';TH.style.opacity='1'},1500);
    console.warn('[CMD] No device selected');
    return;
  }
  x.to=selectedDeviceId;
  var cmd=Object.assign({a:a},x);
  try{
    ws.send(JSON.stringify(cmd));
    _sentCount++;
  }catch(e){
    console.error('[CMD] Send failed: '+e.message);
    ws=null;SR();
    return;
  }
  TP.classList.add('flash');
  setTimeout(function(){TP.classList.remove('flash')},250);
}
function ST(){
  var t=TI.value.trim();if(!t)return;
  SC('t',{d:t});TI.value='';TI.focus();
}

// Drag lock
function TL(){
  ml=!ml;
  if(ml){
    LB.textContent='🔒 拖拽';LB.classList.add('danger');
    TP.classList.add('locked');
    TH.innerHTML='拖拽模式<br>再点解锁';TH.style.color='rgba(248,113,113,.5)';
  }else{
    SC('m',{x:0,y:0,b:0});
    LB.textContent='🔓 拖拽';LB.classList.remove('danger');
    TP.classList.remove('locked');
    TH.innerHTML='滑动 = 光标<br>轻点 = 单击';TH.style.color='';
  }
}

// Speed
SS.oninput=function(){sp=parseFloat(SS.value);SV.textContent=sp.toFixed(2)+'\u00D7'};

// Touchpad
function GP(e){
  if(e.touches&&e.touches.length>0)return{x:e.touches[0].clientX,y:e.touches[0].clientY};
  return{x:e.clientX,y:e.clientY};
}
function UD(p){
  var r=TP.getBoundingClientRect();
  TD.style.left=(p.x-r.left)+'px';TD.style.top=(p.y-r.top)+'px';
}
function DN(e){
  e.preventDefault();ta=true;tm=false;
  var p=GP(e);lx=p.x;ly=p.y;ts=Date.now();
  TP.classList.add('active');TH.style.opacity='0';TD.style.opacity='1';UD(p);
}
function MV(e){
  if(!ta)return;e.preventDefault();
  var p=GP(e),dx=p.x-lx,dy=p.y-ly;
  lx=p.x;ly=p.y;
  if(Math.abs(dx)>.3||Math.abs(dy)>.3){tm=true;UD(p);TM(Math.round(dx),Math.round(dy))}
}
function UP(e){
  if(!ta)return;e.preventDefault();ta=false;
  TP.classList.remove('active');TD.style.opacity='0';TH.style.opacity='1';
  if(ml)TH.style.color='rgba(248,113,113,.5)';else TH.style.color='';
  var el=Date.now()-ts;
  if(!tm&&el<TAP)SC('c');
}
function TM(dx,dy){
  var n=Date.now();if(n-lst<INT)return;lst=n;
  var sx=Math.round(dx*sp),sy=Math.round(dy*sp);
  if(Math.abs(sx)<1&&Math.abs(sy)<1)return;
  var x={x:sx,y:sy};if(ml)x.b=1;
  SC('m',x);
}

// Events
TP.addEventListener('touchstart',DN,{passive:false});
TP.addEventListener('touchmove',MV,{passive:false});
TP.addEventListener('touchend',UP,{passive:false});
TP.addEventListener('touchcancel',UP,{passive:false});
TP.addEventListener('mousedown',DN);
window.addEventListener('mousemove',function(e){if(ta)MV(e)});
window.addEventListener('mouseup',function(e){if(ta)UP(e)});
TI.addEventListener('keydown',function(e){if(e.key==='Enter'){e.preventDefault();ST()}});

// Init
CN();

// Cleanup on page unload
window.addEventListener('beforeunload',function(){if(rt){clearTimeout(rt);rt=null}});
</script>
</body>
</html>`;

// ==================== 输入净化 ====================
// deviceId: 仅允许十六进制字符和冒号 (MAC 地址格式)，最长 20 字符
function sanitizeDeviceId(id) {
  if (!id || typeof id !== 'string') return null;
  const cleaned = id.replace(/[^0-9A-Fa-f:]/g, '').slice(0, 20);
  if (cleaned.length < 12) return null; // 至少 12 字符 (如 11:22:33:AA:BB:CC 不含冒号 = 12)
  return cleaned;
}

// deviceName: 限制长度 60 字符，移除 HTML 特殊字符防 XSS
function sanitizeDeviceName(name) {
  if (!name || typeof name !== 'string') return 'ESP32 Device';
  return name
    .replace(/[<>&"']/g, '')
    .replace(/javascript:/gi, '')
    .slice(0, 60)
    .trim() || 'ESP32 Device';
}

// ==================== Durable Object - WebSocket 房间 (多设备路由) ====================
const HEARTBEAT_TIMEOUT = 90000; // 90 秒无心跳视为离线 (v21: 45→90，适应 BLE keep-alive 30s 间隔)
const STORAGE_KEY_DEVICES = 'persisted_devices'; // v21: DO 状态持久化键
const ALARM_INTERVAL = 30000;       // alarm 间隔 30s

export class WebSocketDurableObject extends DurableObject {
  constructor(ctx, env) {
    super(ctx, env);
    this.devices = new Map();        // deviceId → { ws, name, info, lastSeen }
    this.controllers = new Set();    // 浏览器 ws 集合
    this.storageLoaded = false;      // v21: 持久化数据是否已加载
    this.pendingCommands = new Map(); // v27: 命令队列 deviceId → [{cmd, ts}] DO休眠时缓存
  }

  // v21: 从 DO Storage 恢复设备列表（含元信息，不含 ws 引用）
  async loadDevicesFromStorage() {
    if (this.storageLoaded) return;
    try {
      const raw = await this.ctx.storage.get(STORAGE_KEY_DEVICES);
      if (raw && typeof raw === 'object') {
        for (const [deviceId, meta] of Object.entries(raw)) {
          this.devices.set(deviceId, {
            ws: null,
            name: meta.name || 'ESP32 Device',
            info: meta.info || {},
            lastSeen: meta.lastSeen || 0,
          });
        }
        console.log(`[DO] Restored ${this.devices.size} device(s) from storage`);
      }
      // v31: 恢复待排空命令队列（hibernation 后持久化恢复）
      const rawCmds = await this.ctx.storage.get('pending_cmds');
      if (rawCmds && typeof rawCmds === 'object') {
        for (const [deviceId, queue] of Object.entries(rawCmds)) {
          if (Array.isArray(queue) && queue.length > 0) {
            this.pendingCommands.set(deviceId, queue);
          }
        }
        const total = Array.from(this.pendingCommands.values()).reduce((s, q) => s + q.length, 0);
        if (total > 0) console.log(`[DO] Restored ${total} pending command(s) from storage`);
      }
    } catch (e) {
      console.error(`[DO] loadDevicesFromStorage failed: ${e.message || e}`);
    }
    this.storageLoaded = true;
  }

  // v31: 持久化命令队列到 DO Storage
  async persistPendingCommands() {
    const data = {};
    for (const [deviceId, queue] of this.pendingCommands) {
      if (queue.length > 0) data[deviceId] = queue;
    }
    try {
      await this.ctx.storage.put('pending_cmds', data);
    } catch (e) {
      console.error(`[DO] persistPendingCommands failed: ${e.message || e}`);
    }
  }

  // v21: 将设备元信息持久化到 DO Storage（仅 name/info/lastSeen，不含 ws）
  async persistDevices() {
    const data = {};
    for (const [deviceId, dev] of this.devices) {
      data[deviceId] = {
        name: dev.name,
        info: dev.info,
        lastSeen: dev.lastSeen,
      };
    }
    try {
      await this.ctx.storage.put(STORAGE_KEY_DEVICES, data);
    } catch (e) {
      console.error(`[DO] persistDevices failed: ${e.message || e}`);
    }
  }

  async fetch(request) {
    if (request.headers.get('Upgrade') !== 'websocket') {
      return new Response('Expected WebSocket', { status: 426 });
    }
    const authed = request.headers.get('X-Authed');
    if (authed !== '1') {
      return new Response('Unauthorized', { status: 401 });
    }
    const pair = new WebSocketPair();
    const [client, server] = Object.values(pair);
    this.ctx.acceptWebSocket(server);
    return new Response(null, { status: 101, webSocket: client });
  }

  broadcastDeviceList() {
    const list = [];
    for (const [id, dev] of this.devices) {
      list.push({
        id: id,
        name: dev.name,
        online: dev.ws !== null,
        connected: dev.info?.bleConnected || false,
        encrypted: dev.info?.bleEncrypted || false,
        lastSeen: dev.lastSeen,
        stats: dev.info?.stats || null,
      });
    }
    const msg = JSON.stringify({ type: 'device-list', devices: list });
    for (const ctrl of this.controllers) {
      try { ctrl.send(msg); } catch (_) { }
    }
  }

  notifyDeviceOffline(deviceId) {
    const msg = JSON.stringify({ type: 'device-offline', deviceId });
    for (const ctrl of this.controllers) {
      try { ctrl.send(msg); } catch (_) { }
    }
  }

  // 移除设备（内部统一方法）
  async removeDevice(deviceId, reason) {
    if (!this.devices.has(deviceId)) return;
    this.devices.delete(deviceId);
    console.log(`[DO] Device ${reason}: ${deviceId}`);
    await this.persistDevices();
    this.notifyDeviceOffline(deviceId);
    this.broadcastDeviceList();
  }

  // 心跳超时检查 (每 ALARM_INTERVAL 由 alarm 触发)
  async checkHeartbeatTimeout() {
    const now = Date.now();
    const expired = [];
    for (const [deviceId, dev] of this.devices) {
      // v21: 仅检查有 ws 引用的设备（说明 ESP32 当前连接）；无 ws 的是从存储恢复的僵尸记录
      if (dev.ws && (now - dev.lastSeen) > HEARTBEAT_TIMEOUT) {
        expired.push(deviceId);
      }
    }
    for (const deviceId of expired) {
      const dev = this.devices.get(deviceId);
      if (dev) {
        try { dev.ws.close(4002, 'Heartbeat timeout'); } catch (_) { }
        await this.removeDevice(deviceId, 'timeout');
      }
    }
    // v22: 清理持久化中无 ws 且超过 30 分钟未更新的僵尸记录
    // 从 5 分钟延长到 30 分钟，防止用户刷新页面后因短时间未重连导致设备丢失
    const staleThreshold = now - 1800000; // 30 分钟
    const stale = [];
    for (const [deviceId, dev] of this.devices) {
      if (!dev.ws && dev.lastSeen < staleThreshold) {
        stale.push(deviceId);
      }
    }
    for (const deviceId of stale) {
      this.devices.delete(deviceId);
      console.warn(`[DO] Device stale-cleaned: ${deviceId}`);
    }
    if (stale.length > 0) {
      await this.persistDevices();
      this.broadcastDeviceList();
    }
  }

  async alarm() {
    await this.loadDevicesFromStorage();
    await this.checkHeartbeatTimeout();
    // v25: 仅在有活跃设备 ws 连接时才自维持 alarm
    // 如果没有任何设备连接，允许 DO hibernation 以节省资源
    let hasActiveConnection = false;
    for (const [, dev] of this.devices) {
      if (dev.ws) { hasActiveConnection = true; break; }
    }
    if (hasActiveConnection || this.controllers.size > 0) {
      try {
        await this.ctx.storage.setAlarm(Date.now() + ALARM_INTERVAL);
      } catch (e) {
        console.error(`[DO] setAlarm err: ${e.message || e}`);
      }
    }
  }

  async webSocketOpen(ws) {
    // v21: 从持久化恢复设备列表，确保冷启动后控制器能立即看到历史设备
    await this.loadDevicesFromStorage();
    this.broadcastDeviceList();
  }

  // v39: WS 消息频率限制 — per-device, 1 秒滑动窗口最多 20 条
  checkWsRateLimit(ws) {
    const now = Date.now();
    if (!this._wsRateMap) this._wsRateMap = new Map();
    const entry = this._wsRateMap.get(ws) || { count: 0, windowStart: now };
    if (now - entry.windowStart > 1000) {
      entry.count = 1;
      entry.windowStart = now;
    } else {
      entry.count++;
    }
    this._wsRateMap.set(ws, entry);
    if (entry.count > 20) return false;
    return true;
  }

  async webSocketMessage(ws, message) {
    if (!this.checkWsRateLimit(ws)) return;

    let parsed;
    try {
      parsed = JSON.parse(message);
    } catch (_) {
      console.warn(`[DO] Bad JSON: ${typeof message === 'string' ? message.slice(0, 80) : 'binary'}`);
      return;
    }

    // v23: 关键修复 - DO Hibernation 后 Map 为空，必须先恢复持久化设备列表
    const wasMapEmpty = this.devices.size === 0;
    await this.loadDevicesFromStorage();
    if (wasMapEmpty && this.devices.size > 0) {
      console.warn(`[DO] Woke from hibernation: restored ${this.devices.size} device(s)`);
    }

    const type = parsed.type || '?';

    // v38: 每次最多排空命令数 — 防止 JSON 超过 ESP32 接收缓冲区上限
    // arduinoWebSockets 默认缓冲区 512B，每条 ~60B，3 条 ≈200B 安全
    const MAX_DRAIN_SIZE = 3;

    // v25: 获取 deviceId（ESP32 消息有，浏览器消息没有）
    const espDeviceId = sanitizeDeviceId(parsed.deviceId);

    // 设备注册 (ESP32) - 最高优先级，立即回复 ACK 避免 ESP32 重连
    if (type === 'register') {
      const deviceId = espDeviceId;
      if (!deviceId) {
        console.warn(`[DO] Register rejected: invalid deviceId`);
        return;
      }
      const deviceName = sanitizeDeviceName(parsed.deviceName);

      // v30: 利用 DO 原生 serializeAttachment 标记 ws (生存于 DO 生命周期)
      try { ws.serializeAttachment({ deviceId }); } catch (e) {
        console.error(`[DO] serializeAttachment failed: ${e.message || e}`);
      }

      // v38: register-ack 限制 cmds 数量与 hb-ack 一致 (MAX_DRAIN_SIZE=3)
      // 防止积压过多时 JSON 超过 ESP32 接收缓冲区上限 → 截断 → bad JSON
      let regCmds = [];
      const regQueue = this.pendingCommands.get(deviceId);
      if (regQueue && regQueue.length > 0) {
        const now = Date.now();
        const valid = regQueue.filter(item => (now - item.ts) <= 30000);
        if (valid.length > MAX_DRAIN_SIZE) {
          regCmds = valid.slice(0, MAX_DRAIN_SIZE).map(item => item.cmd);
          this.pendingCommands.set(deviceId, valid.slice(MAX_DRAIN_SIZE));
          console.log(`[DO] register-ack with ${regCmds.length} cmd(s) for ${deviceId} (${valid.length - MAX_DRAIN_SIZE} remaining in queue)`);
        } else {
          regCmds = valid.map(item => item.cmd);
          this.pendingCommands.delete(deviceId);
          if (regCmds.length > 0) {
            console.log(`[DO] register-ack with ${regCmds.length} cmd(s) for ${deviceId}`);
          }
        }
        await this.persistPendingCommands();
      }
      try { ws.send(JSON.stringify({ type: 'register-ack', deviceId, cmds: regCmds })); } catch (_) { }

      const wasNew = !this.devices.has(deviceId);
      if (!wasNew) {
        const oldDev = this.devices.get(deviceId);
        if (oldDev.ws && oldDev.ws !== ws) {
          try { oldDev.ws.close(4001, 'Replaced'); } catch (_) { }
        }
      }
      this.devices.set(deviceId, {
        ws: ws,
        name: deviceName,
        info: parsed.info || {},
        lastSeen: Date.now(),
      });
      this.controllers.delete(ws);
      console.log(`[DO] Device registered: ${deviceId} (${deviceName})${wasNew ? ' NEW' : ' reconnected'}`);

      await this.persistDevices();
      this.broadcastDeviceList();
      if (this.devices.size === 1) {
        await this.ctx.storage.setAlarm(Date.now() + ALARM_INTERVAL);
      }
      return;
    }

    // v30: ESP32 非 register 消息 — 更新 lastSeen + 排空命令队列
    if (espDeviceId) {
      const dev = this.devices.get(espDeviceId);
      if (dev) {
        dev.ws = ws;
        dev.lastSeen = Date.now();
        this.controllers.delete(ws);
        if (parsed.info) dev.info = parsed.info;
      }
      // v32: 命令嵌入消息回复，不再逐条 ws.send()
      // 根因: DO hibernation 恢复后 ws.send() 在 webSocketMessage 回调中不报错，
      // 但 CF 内部静默丢弃 → ESP32 永远收不到。
      // 解决: 在 heartbeat/ping 回复时将命令列表嵌入 JSON，ESP32 从回复中提取并执行。
      // 总是回复 hb-ack/ping-ack，让 ESP32 端重置空闲计时器
      // v35: 每次最多排空 3 条命令（从 5 降至 3），防止 hb-ack JSON 过大被 WebSocket 截断
      // 根因：arduinoWebSockets 默认接收缓冲区可能不足，长 JSON 被截断 → bad JSON → ESP32 CMD=0
      // 配合 ESP32 端 WEBSOCKETS_MAX_DATA_SIZE 4096 + chain drain (cmdCount>0 立即 ping)
      // 每条 ~60B，3 条 ≈200B，在默认 512B 缓冲区安全范围内
      let cmds = [];
      const pendingQueue = this.pendingCommands.get(espDeviceId);
      if (pendingQueue && pendingQueue.length > 0) {
        const now = Date.now();
        const valid = pendingQueue.filter(item => (now - item.ts) <= 30000);
        if (valid.length > MAX_DRAIN_SIZE) {
          // 截取前 MAX_DRAIN_SIZE 条，剩余放回队列
          cmds = valid.slice(0, MAX_DRAIN_SIZE).map(item => item.cmd);
          this.pendingCommands.set(espDeviceId, valid.slice(MAX_DRAIN_SIZE));
          console.log(`[DO] ${type} reply with ${cmds.length} cmd(s) to ${espDeviceId} (${valid.length - MAX_DRAIN_SIZE} remaining in queue)`);
        } else {
          cmds = valid.map(item => item.cmd);
          this.pendingCommands.delete(espDeviceId);
          if (cmds.length > 0) {
            console.log(`[DO] ${type} reply with ${cmds.length} cmd(s) to ${espDeviceId}`);
          }
        }
        await this.persistPendingCommands();
      }
      const ackType = type === 'heartbeat' ? 'hb-ack' : 'ping-ack';
      try { ws.send(JSON.stringify({ type: ackType, cmds })); } catch (e) {
        console.error(`[DO] ${ackType} send err: ${e.message || e}`);
      }
      if (type === 'heartbeat' || type === 'status') {
        this.broadcastDeviceList();
      }
      return;
    }

    // 请求设备列表 (浏览器)
    if (type === 'list-devices') {
      this.controllers.add(ws);
      // v21: 确保已加载持久化数据
      await this.loadDevicesFromStorage();
      this.broadcastDeviceList();
      return;
    }

    // 控制指令路由
    const targetId = parsed.to ? sanitizeDeviceId(parsed.to) : null;
    if (targetId) {
      const dev = this.devices.get(targetId);
      const cmd = {};
      if (typeof parsed.a === 'string' && parsed.a.length <= 2) cmd.a = parsed.a;
      if (parsed.x !== undefined) cmd.x = Math.round(Number(parsed.x) || 0);
      if (parsed.y !== undefined) cmd.y = Math.round(Number(parsed.y) || 0);
      if (parsed.b !== undefined) cmd.b = Number(parsed.b) || 0;
      if (parsed.d !== undefined && typeof parsed.d === 'string') cmd.d = parsed.d.slice(0, 200);
      if (!cmd.a) return;

      // v31: 命令永远入队，不通过 dev.ws.send() 直接发送
      // 根因：Cloudflare DO hibernation 后 dev.ws 引用指向新 DO 实例的空壳 WebSocket，
      // send() 不抛异常但数据在 CF 内部被静默丢弃 → ESP32 永远收不到 → CMD=0。
      // 解决：命令入队并持久化到 storage，由 ESP32 消息回调中的 ws 参数排空。
      const queue = this.pendingCommands.get(targetId) || [];
      if (queue.length >= 20) queue.shift();
      queue.push({ cmd, ts: Date.now() });
      if (!this.pendingCommands.has(targetId)) {
        this.pendingCommands.set(targetId, queue);
      }
      if (queue.length >= 15) console.warn(`[DO] Cmd queue high (${queue.length}) for ${targetId}`);
      // 必须持久化：DO hibernation 时内存 Map 会清空，不保存就永远丢失
      await this.persistPendingCommands();
      // 如果设备不在 Map 中 (已过期)，提示用户
      if (!dev) {
        console.log(`[DO] Cmd queued but target device '${targetId}' not found, will drop on next drain`);
        try { ws.send(JSON.stringify({ type: 'device-list', devices: [] })); } catch (_) { }
      }
      this.controllers.add(ws);
      return;
    }

    // 无 to 字段 → 请求控制器刷新设备列表
    this.controllers.add(ws);
  }

  async webSocketClose(ws, code, reason, wasClean) {
    this.controllers.delete(ws);
    this._wsRateMap?.delete(ws);
    let deviceRemoved = false;
    for (const [deviceId, dev] of this.devices) {
      if (dev.ws === ws) {
        dev.ws = null;  // v21: 保留元信息但标记 ws 为 null，心跳超时后再彻底清理
        dev.lastSeen = Date.now();
        console.warn(`[DO] Device WS closed: ${deviceId}`);
        deviceRemoved = true;
        break;
      }
    }
    // v21: 不立即移除设备，让心跳超时机制统一清理
    // 避免 DO 短暂回收导致频繁 add/remove 抖动
    if (deviceRemoved) {
      await this.persistDevices();
      this.broadcastDeviceList();
    }
  }

  async webSocketError(ws, error) {
    this.controllers.delete(ws);
    this._wsRateMap?.delete(ws);
    let deviceRemoved = false;
    for (const [deviceId, dev] of this.devices) {
      if (dev.ws === ws) {
        dev.ws = null;
        dev.lastSeen = Date.now();
        console.warn(`[DO] Device WS error: ${deviceId}`);
        deviceRemoved = true;
        break;
      }
    }
    if (deviceRemoved) {
      await this.persistDevices();
      this.broadcastDeviceList();
    }
  }
}

// ==================== Durable Object - 全局频率限制计数器 ====================
export class RateLimiterDO extends DurableObject {
  constructor(ctx, env) {
    super(ctx, env);
  }

  async fetch(request) {
    const url = new URL(request.url);
    const ip = url.searchParams.get('ip');
    const max = parseInt(url.searchParams.get('max') || '5', 10);
    const windowMs = parseInt(url.searchParams.get('window') || '60000', 10);
    if (!ip) return new Response('missing ip', { status: 400 });

    const now = Date.now();
    const key = `rl:${ip}`;
    let entry = await this.ctx.storage.get(key);

    if (!entry || now > entry.resetTime) {
      entry = { count: 1, resetTime: now + windowMs };
      await this.ctx.storage.put(key, entry, { expirationTtl: Math.ceil(windowMs / 1000) + 10 });
      return new Response(JSON.stringify({ allowed: true }));
    }

    if (entry.count >= max) {
      const retryAfter = Math.ceil((entry.resetTime - now) / 1000);
      return new Response(JSON.stringify({ allowed: false, retryAfter }));
    }

    entry.count++;
    await this.ctx.storage.put(key, entry, { expirationTtl: Math.ceil((entry.resetTime - now) / 1000) + 10 });
    return new Response(JSON.stringify({ allowed: true }));
  }
}

// ==================== Worker 入口 ====================
export default {
  async fetch(request, env) {
    const url = new URL(request.url);

    // POST /api/auth
    if (url.pathname === '/api/auth' && request.method === 'POST') {
      try {
        const ip = request.headers.get('CF-Connecting-IP') || 'unknown';
        // Rate limiter 失败时 fallback 为放行，防止 DO 异常导致用户无法登录
        try {
          const rlId = env.RATE_LIMITER.idFromName('global');
          const rlStub = env.RATE_LIMITER.get(rlId);
          const rlUrl = new URL(`http://rl/check?ip=${encodeURIComponent(ip)}&max=5&window=60000`);
          const rlRes = await rlStub.fetch(rlUrl);
          const rlData = await rlRes.json();
          if (!rlData.allowed) {
            return corsResponse(JSON.stringify({ error: '尝试次数过多，请稍后再试' }), 429);
          }
        } catch (_) { /* rate limiter unavailable, allow */ }
        const body = await request.json();
        const password = body.password || '';
        if (password === (env.PASSWORD || 'admin')) {
          return setAuthCookie(corsResponse(JSON.stringify({ ok: true })), password, env.COOKIE_SECRET || 'fallback-secret');
        }
        return corsResponse(JSON.stringify({ error: '密码错误' }), 401);
      } catch (_) {
        return corsResponse(JSON.stringify({ error: '无效请求' }), 400);
      }
    }

    // OPTIONS /api/auth
    if (url.pathname === '/api/auth' && request.method === 'OPTIONS') {
      return corsResponse('');
    }

    // /ws — WebSocket 升级
    if (url.pathname === '/ws') {
      if (request.headers.get('Upgrade') !== 'websocket') {
        return new Response('Expected WebSocket', { status: 426 });
      }
      const authed = await verifyWsAuth(request, env);
      if (!authed) {
        return new Response('Unauthorized', { status: 401 });
      }
      const doUrl = new URL(url.origin + '/ws');
      const modifiedRequest = new Request(doUrl.toString(), request);
      modifiedRequest.headers.set('X-Authed', '1');
      const id = env.WS_DO.idFromName('default-room');
      const stub = env.WS_DO.get(id);
      return stub.fetch(modifiedRequest);
    }

    // / 或 /index.html
    if (url.pathname === '/' || url.pathname === '/index.html') {
      const authed = await verifyCookieAuth(request, env);
      if (!authed) {
        return new Response(LOGIN_PAGE, {
          headers: { 'Content-Type': 'text/html; charset=utf-8', 'Cache-Control': 'no-cache' },
        });
      }
      return new Response(HTML_PAGE, {
        headers: {
          'Content-Type': 'text/html; charset=utf-8',
          'Cache-Control': 'no-cache',
          'Content-Security-Policy': "default-src 'self'; script-src 'self' 'unsafe-inline'; style-src 'self' 'unsafe-inline'; connect-src 'self' wss: ws:;",
          'X-Content-Type-Options': 'nosniff',
        },
      });
    }

    return new Response('Not Found', { status: 404 });
  },
};
