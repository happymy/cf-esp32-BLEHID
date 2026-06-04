/**
 * Cloudflare Worker - ESP32 BLE HID 遥控器 (v7c + 蓝牙状态上报)
 *
 * v7c 新增:
 *   - 蓝牙状态（连接/断开/连接中），触控板右上角显示状态指示灯和文字
 * v7b 新增:
 *   - UI 新增 ⌫ 退格 (Backspace) 和 ↵ 回车 (Enter) 按钮，3×3 网格布局
 *   - 调整触控板高度约束以容纳新按钮行
 * v7a 修复:
 *   - 移除左上角标题，消除 iOS Safari 动态缩放
 *   - 状态指示移至触控板内部右上角
 *   - 添加 -webkit-text-size-adjust: 100%
 * v7 改进:
 *   - 正方形触控板 (aspect-ratio:1)，自适应屏幕
 *   - Dark 主题 AMOLED 优化
 */

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

// ==================== 登录频率限制 ====================
const rateLimitMap = new Map();
const RATE_LIMIT_MAX = 5;
const RATE_LIMIT_WINDOW = 60000;

function checkRateLimit(ip) {
  const now = Date.now();
  let entry = rateLimitMap.get(ip);
  if (!entry || now > entry.resetTime) {
    entry = { count: 1, resetTime: now + RATE_LIMIT_WINDOW };
    rateLimitMap.set(ip, entry);
    return true;
  }
  if (entry.count >= RATE_LIMIT_MAX) return false;
  entry.count++;
  return true;
}

function cleanupRateLimit() {
  const now = Date.now();
  for (const [ip, entry] of rateLimitMap) {
    if (now > entry.resetTime) rateLimitMap.delete(ip);
  }
}

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

// ==================== 主控制页面 v7b (退格/回车按钮 + iOS zoom fix) ====================
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

/* ===== MAIN ===== */
.main{
  flex:1;
  display:flex;flex-direction:column;
  align-items:center;justify-content:center;
  padding:calc(4px + var(--safe-t)) 10px 4px;
  gap:6px;
  min-height:0;
}

/* ===== TOUCHPAD (正方形) ===== */
.tpad{
  width:min(calc(100vw - 20px), calc(100dvh - 260px));
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
  .tpad{max-width:420px}
  .actions{max-width:420px}
  .speed{max-width:420px}
}
@media(max-height:650px){
  .tpad{width:min(calc(100vw - 20px), calc(100dvh - 230px));max-width:300px}
  .actions{max-width:300px;gap:4px}
  .speed{max-width:300px}
  .btn{padding:8px 3px;font-size:.68rem}
  .main{gap:4px}
}
@media(prefers-reduced-motion:reduce){
  *,*::before,*::after{animation-duration:.01ms!important;transition-duration:.01ms!important}
}
</style>
</head>
<body>

<!-- MAIN (无 Header，触控板从顶部安全区开始) -->
<div class="main">

  <!-- SQUARE TOUCHPAD -->
  <div class="tpad" id="tp">

    <!-- 状态指示 (触控板右上角) -->
    <div class="tpad-status">
      <div class="tpad-dot" id="sd"></div>
      <span class="tpad-stxt" id="st">ESP32：未连接</span>
    </div>

    <!-- 拖拽标签 (触控板左上角) -->
    <div class="lock-tag" id="lt">🔒 拖拽</div>

    <!-- 触摸提示 (中心) -->
    <div class="tpad-hint" id="th">滑动 = 光标<br>轻点 = 单击</div>

    <!-- 触摸光斑 -->
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
    <input type="range" id="ss" min="0.25" max="3" step="0.25" value="1">
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
    SS=document.getElementById('ss'),SV=document.getElementById('sv');

// State
var ws=null,rt=null,ta=false,lx=0,ly=0,ts=0,tm=false,ml=false,sp=1;
var bleOk=false;
var TAP=260,INT=35,lst=0;

// WebSocket
function CN(){
  if(ws&&ws.readyState===WebSocket.OPEN)return;
  US('connecting');
  var p=location.protocol==='https:'?'wss:':'ws:';
  ws=new WebSocket(p+'//'+location.host+'/ws');
  ws.onopen=function(){US('online');if(rt){clearTimeout(rt);rt=null}};
  ws.onclose=function(){bleOk=false;US('offline');SR()};
  ws.onerror=function(){ws=null};
  ws.onmessage=function(e){
    try{
      var m=JSON.parse(e.data);
      if(m.a==='status'){
        bleOk=!!m.ble;
        US(ws&&ws.readyState===WebSocket.OPEN?'online':'offline');
      }
    }catch(_){}
  };
}
function SR(){if(!rt)rt=setTimeout(CN,3000)}
function US(s){
  SD.className='tpad-dot';
  if(s==='online'){
    if(bleOk){
      SD.classList.add('on');
      ST_.textContent='ESP32：已连接，蓝牙已连接';
    }else{
      SD.classList.add('connecting');
      ST_.textContent='ESP32：已连接，蓝牙断开';
    }
  }else if(s==='connecting'){
    SD.classList.add('connecting');ST_.textContent='ESP32：连接中';
  }else{
    ST_.textContent='ESP32：未连接';
  }
}
function SC(a,x){
  x=x||{};
  if(!ws||ws.readyState!==WebSocket.OPEN){US('offline');SR();return}
  ws.send(JSON.stringify(Object.assign({a:a},x)));
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
</script>
</body>
</html>`;

// ==================== Durable Object - WebSocket 房间 ====================
export class WebSocketDurableObject {
  constructor(state, env) {
    this.state = state;
    this.env = env;
  }

  async fetch(request) {
    const authed = request.headers.get('X-Authed');
    if (authed !== '1') {
      return new Response('Unauthorized', { status: 401 });
    }

    const pair = new WebSocketPair();
    const [client, server] = Object.values(pair);
    this.state.acceptWebSocket(server);
    return new Response(null, { status: 101, webSocket: client });
  }

  async webSocketMessage(ws, message) {
    const sessions = this.state.getWebSockets();
    for (const session of sessions) {
      if (session !== ws) {
        try {
          session.send(message);
        } catch (_) { }
      }
    }
  }

  async webSocketClose(ws, code, reason, wasClean) { }
  async webSocketError(ws, error) { }
}

// ==================== Worker 入口 ====================
export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    cleanupRateLimit();

    // POST /api/auth — 密码验证
    if (url.pathname === '/api/auth' && request.method === 'POST') {
      try {
        const ip = request.headers.get('CF-Connecting-IP') || 'unknown';
        if (!checkRateLimit(ip)) {
          return corsResponse(JSON.stringify({ error: '尝试次数过多，请稍后再试' }), 429);
        }
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

    // / 或 /index.html — 控制页面
    if (url.pathname === '/' || url.pathname === '/index.html') {
      const authed = await verifyCookieAuth(request, env);
      if (!authed) {
        return new Response(LOGIN_PAGE, {
          headers: {
            'Content-Type': 'text/html; charset=utf-8',
            'Cache-Control': 'no-cache',
          },
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
