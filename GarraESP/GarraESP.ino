// ============================================================
//  Garra hidráulica com ESP32: motores DC na base e no ombro
//  controlada pelo celular.
//
//  NÃO PRECISA DE NENHUMA BIBLIOTECA EXTRA: usa só o que já vem
//  com o suporte ao ESP32 na Arduino IDE (WiFi, WebServer e LEDC).
//  Painel integrado. Configure WifiLocal.h a partir de WifiLocal.exemplo.h.
//
//  Funciona com ponte H L298N, L9110S, TB6612 ou parecidas
//  (2 entradas por motor). No L298N, deixe os jumpers ENA/ENB.
//
//  Comandos em http://192.168.4.1/cmd?c=<comando>
//    m,<motor>,<dir>   motor 0 = base; motor 1 = ombro
//                      dir = 1 (botão +), -1 (botão −), 0 (parar)
//                      o celular repete enquanto o botão está apertado
//    v,<0-100>         velocidade em %
//    x                 PARAR tudo
//    p                 ping
//  Vários comandos podem ir juntos, separados por ';'  ex: m,0,1;m,1,-1
//  Toda resposta é um JSON com o estado atual.
// ============================================================
#include <WiFi.h>
#include <WebServer.h>
#include "WifiLocal.h"
#if __has_include(<esp_arduino_version.h>)
#include <esp_arduino_version.h>
#endif

// ============================================================
//  CONFIGURAÇÃO  (edite aqui: pinos, nomes, rede, velocidade)
// ============================================================

// ---------- Rede Wi-Fi criada pelo ESP32 (modo Access Point) ----------
// O celular conecta nesta rede e abre http://192.168.4.1
#define WIFI_SSID   "Garra-ESP-Lebron"

// ---------- Motores (cada um = 1 motor DC + 1 par de seringas) ----------
// Uma ponte H de dois canais controla os dois motores.
//
// pinoA / pinoB : vão nas entradas da ponte H (IN1/IN2 ou IN3/IN4).
// botaoMenos / botaoMais : texto dos botões no celular.
// invertido     : true se o motor gira ao contrário do botão.
//
//  Canal A: IN1 = GPIO32, IN2 = GPIO33 (Base).
//  Canal B: IN3 = GPIO25, IN4 = GPIO26 (Ombro).
struct Motor {
  const char* nome;
  const char* botaoMenos;
  const char* botaoMais;
  uint8_t     pinoA;
  uint8_t     pinoB;
  bool        invertido;
};

#define NUM_MOTORES 2

static const Motor MOTORES[NUM_MOTORES] = {
  // nome        botão −      botão +     pinoA pinoB invertido
  { "Base",     "Esquerda",  "Direita",    32,   33,   false },
  { "Ombro",    "Desce",     "Sobe",       25,   26,   false },
};

// ---------- Velocidade (PWM nas entradas da ponte H) ----------
#define VELOCIDADE_PADRAO  70    // % (ajustável pelo celular)
#define VELOCIDADE_MIN     30    // abaixo disso o motor costuma nem girar
#define PWM_FREQ         1000    // Hz
#define PWM_BITS            8    // 0–255
#define RAMPA_MS          150    // partida suave (evita tranco na hidráulica)

// ---------- Segurança ----------
// O celular manda o comando repetidamente enquanto o botão está apertado.
// Se parar de chegar por este tempo, o motor para (botão solto / sinal caiu).
#define TEMPO_SEM_COMANDO_MS  400
// Tempo máximo girando sem soltar o botão (protege o fim de curso da seringa).
#define TEMPO_MAX_MS         6000

void pwmIniciar(uint8_t pino, uint8_t canal);
void pwmEscrever(uint8_t pino, uint8_t canal, uint32_t v);
void aplicarMotor(int i);
void pararMotor(int i);
void pararTudo();
String estadoJson();
void tratarMensagem(const char* msg);
void setup();
void loop();

static const char PAINEL_3D_HTML[] PROGMEM = R"garrahtml(
<!doctype html>
<html lang="pt-BR">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
<meta name="theme-color" content="#f3f5f2">
<meta name="description" content="Controle da base e do ombro da garra hidráulica pelo Wi-Fi do ESP32.">
<title>Garra Hidráulica • ESP32</title>
<link rel="icon" href="data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 32 32'%3E%3Crect width='32' height='32' rx='8' fill='%2317644d'/%3E%3Cpath d='M6 25h12M11 25v-6l7-9m0 0 5-4m-5 4 1 5m4-9 4 1-1 4' fill='none' stroke='%23ffffff' stroke-width='2' stroke-linecap='round'/%3E%3C/svg%3E">
<style>
:root{color-scheme:light;--bg:#f3f5f2;--panel:#fff;--surface:#f7f9f6;--line:#dce5de;--fg:#192722;--sub:#4f6559;--accent:#17644d;--mint:#e2f1e9;--danger:#9f332e;--mono:ui-monospace,"Cascadia Mono",Consolas,monospace}
*{box-sizing:border-box}
html{scroll-behavior:smooth}
body{margin:0;background:var(--bg);color:var(--fg);font:1rem system-ui,-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif}
button,input{font:inherit}
button{color:inherit;cursor:pointer}
button:focus-visible,a:focus-visible,input:focus-visible,canvas:focus-visible{outline:2px solid var(--accent);outline-offset:3px}
a{color:inherit}
button:disabled{opacity:.5;cursor:default}
.shell{width:min(100% - 64px,1180px);margin-inline:auto}
.topbar{border-bottom:1px solid var(--line);background:#fafbf9}
.topbar .shell,.toplinks,.brand{display:flex;align-items:center}
.topbar .shell{min-height:62px;justify-content:space-between;gap:12px}
.toplinks{gap:18px}
.toplinks a{text-decoration:none;font-size:.86rem;font-weight:650;color:var(--sub)}
.toplinks a:hover{color:var(--accent)}
.brand{gap:9px;font-size:.87rem;font-weight:760;letter-spacing:-.01em}
.brand-logo{width:32px;height:32px;display:grid;place-items:center;border-radius:10px;background:var(--accent);color:#fff}
.brand-logo svg{width:21px;height:21px}
.connection{display:flex;align-items:center;gap:7px;padding:6px 10px;border:1px solid var(--line);border-radius:100px;background:#fff;color:var(--sub);font-size:.77rem;font-weight:650;white-space:nowrap}
.connection::before{content:"";width:6px;height:6px;border-radius:50%;background:currentColor;flex:none}
.connection[data-mode="preview"],.connection[data-mode="live"]{color:var(--accent);background:#eaf4ec;border-color:#c9dfd1}
.connection[data-mode="offline"]{color:var(--danger)}
main{padding:38px 0 30px}
.hero{margin-bottom:24px;animation:arrive .38s ease-out both}
.eyebrow{color:var(--accent);text-transform:uppercase;font:700 .72rem/1.4 var(--mono);letter-spacing:.12em}
h1,h2,h3,p{margin:0}
h1{font-size:clamp(2.25rem,5.7vw,4.55rem);line-height:1;letter-spacing:-.067em;margin:8px 0 10px;font-weight:760}
.hero p{max-width:680px;color:var(--sub);line-height:1.5}
.dashboard{display:grid;grid-template-columns:minmax(0,1.76fr) minmax(300px,1fr);align-items:stretch;gap:16px;animation:arrive .44s .04s ease-out both}
@keyframes arrive{from{opacity:0;transform:translateY(7px)}to{opacity:1;transform:translateY(0)}}
.panel{overflow:hidden;border:1px solid var(--line);border-radius:17px;background:var(--panel);box-shadow:0 8px 28px #25463607}
.scene{position:relative;min-height:438px;background:#e8eee8;isolation:isolate}
.scene canvas{position:absolute;inset:0;width:100%;height:100%;display:block;touch-action:pan-y;cursor:grab}
.scene .model-software{pointer-events:none}
.initial-model{position:absolute;inset:10% 0 8%;width:100%;height:82%;pointer-events:none}
.scene [hidden]{display:none}
.scene canvas:active{cursor:grabbing}
.scene-head,.scene-foot{position:absolute;left:16px;right:16px;display:flex;justify-content:space-between;align-items:flex-start;gap:8px;pointer-events:none}
.scene-head{top:16px}
.scene-foot{bottom:14px;align-items:flex-end}
.scene-title,.axis-legend,.view-hint{border:1px solid #cfdcd3;border-radius:8px;background:#f8fbf7ed}
.scene-title{padding:7px 10px;font-size:.87rem;font-weight:740;letter-spacing:-.015em}
.view-actions{display:flex;gap:6px;pointer-events:auto}
.view-button{border:1px solid #cfdcd3;background:#f8fbf7ed;color:#29483c;border-radius:8px;padding:7px 9px;min-height:34px;font-size:.76rem;font-weight:690;white-space:nowrap}
.view-button:hover{border-color:var(--accent);color:var(--accent)}
.axis-legend{display:flex;align-items:center;gap:6px;padding:5px 8px;font-size:.72rem;font-weight:650;color:var(--sub)}
.axis-legend::before{content:"";width:6px;height:6px;border-radius:50%;background:#637b6d}
.axis-legend.moving{color:var(--accent)}
.axis-legend.moving::before{background:var(--accent);animation:pulse .85s ease-in-out infinite alternate}
@keyframes pulse{to{opacity:.35}}
.view-hint{padding:5px 8px;font-size:.72rem;font-weight:650;color:var(--sub)}
.graphics-error{position:absolute;inset:40% 12% auto;text-align:center;color:var(--sub)}
.controls{padding:17px;display:flex;flex-direction:column;gap:10px}
.controls h2,.info h2{font-size:1.16rem;letter-spacing:-.035em}
.controls-hint{color:var(--sub);font-size:.82rem;margin-top:4px}
.speed{display:grid;grid-template-columns:auto minmax(55px,1fr) auto;align-items:center;gap:10px;background:var(--surface);border:1px solid var(--line);border-radius:10px;padding:8px 10px;min-height:40px}
.speed label{font-size:.82rem;font-weight:700}
.speed input{width:100%;min-width:0;margin:0;accent-color:var(--accent);cursor:pointer}
.speed output{color:var(--accent);font:700 .82rem var(--mono);min-width:4ch;text-align:right}
.motors{display:grid;gap:8px;flex:1}
.motor{min-width:0;padding:9px;background:var(--surface);border:1px solid var(--line);border-radius:11px;transition:border-color .2s,background .2s}
.motor[data-state="move"]{border-color:#75ad93;background:#edf7f0}
.motor[data-state="limit"]{border-color:#bb834b}
.motor-head{display:flex;align-items:center;gap:7px;min-height:22px}
.motor-symbol{width:22px;height:22px;flex:none;display:grid;place-items:center;border-radius:6px;background:var(--mint);color:var(--accent);font:750 .85rem var(--mono)}
.motor h3{font-size:.9rem}
.tag{display:flex;align-items:center;gap:5px;font-size:.73rem;color:var(--sub);margin-left:auto;text-align:right}
.tag::before{content:"";width:5px;height:5px;border-radius:50%;background:currentColor}
[data-state="move"] .tag{color:var(--accent)}
[data-state="limit"] .tag{color:#874d19}
.actions{display:grid;grid-template-columns:1fr 1fr;gap:6px;margin-top:7px}
.hold{border:1px solid #d7e2da;background:#fff;border-radius:8px;min-height:43px;padding:5px 4px;display:flex;align-items:center;justify-content:center;gap:5px;font-size:.8rem;font-weight:680;touch-action:none;user-select:none;-webkit-touch-callout:none;transition:background .15s,color .15s,transform .15s,border-color .15s}
.hold:hover:not(:disabled){border-color:var(--accent)}
.hold .direction{color:var(--accent);font-size:1rem;line-height:1}
.hold.active,.hold.commanded{background:var(--accent);border-color:var(--accent);color:#fff}
.hold.active{transform:scale(.98)}
.hold.active .direction,.hold.commanded .direction{color:inherit}
.stop{width:100%;min-height:42px;border:1px solid #e9c4c0;border-radius:9px;background:#fff4f2;color:var(--danger);font-weight:760;font-size:.84rem;display:flex;align-items:center;justify-content:center;gap:8px;transition:background .15s,transform .15s}
.stop:hover{background:#fbe8e5}
.stop:active{transform:scale(.98)}
.stop::before{content:"";width:8px;height:8px;border-radius:2px;background:currentColor}
.info{padding:23px 0 12px;scroll-margin:14px}
.info h2{margin-bottom:10px}
.info-grid{display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:10px}
.info article{padding:14px;line-height:1.48}
.info article h3{font-size:.9rem;margin-bottom:4px}
.info article p,.info .model-note{color:var(--sub);font-size:.8rem}
.info .model-note{margin-top:10px}
@media(max-width:880px){.shell{width:calc(100% - 38px)}.dashboard{grid-template-columns:minmax(0,1.22fr) minmax(290px,1fr)}.scene-head,.scene-foot{left:11px;right:11px}.controls{padding:13px}}
@media(max-width:680px){.shell{width:calc(100% - 26px)}.topbar .shell{min-height:52px}.toplinks a{display:none}main{padding-top:21px}.hero{margin-bottom:14px}h1{margin:5px 0 8px}.hero p{font-size:.88rem}.dashboard{display:flex;flex-direction:column;gap:10px}.scene{min-height:0;height:clamp(220px,37vh,350px)}.scene-head{top:10px}.scene-foot{bottom:9px}.scene-title,.view-button{padding-inline:6px;font-size:.72rem}.view-hint{display:none}.hold{min-height:44px}.hero,.dashboard{animation:none}.info-grid{grid-template-columns:1fr}.info{padding-top:17px}}
@media(prefers-reduced-motion:reduce){*,*::before,*::after{animation:none!important;transition:none!important;scroll-behavior:auto!important}}
</style>
</head>
<body>
<header class="topbar">
  <div class="shell">
    <div class="brand"><span class="brand-logo" aria-hidden="true"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"><path d="M4 20h10m-6 0v-5l5-6m0 0 4-4 3 1-1 4m-2-5-1 7-3-3"/><circle cx="8" cy="15" r="1.5" fill="currentColor"/></svg></span>Garra ESP</div>
    <div class="toplinks"><a href="#projeto">Sobre o projeto</a><div id="connection" class="connection" role="status">Conectando…</div></div>
  </div>
</header>
<main class="shell">
  <header class="hero">
    <div class="eyebrow">Projeto de robótica · ESP32</div>
    <h1>Garra hidráulica</h1>
    <p>Controle a rotação da base e a altura do ombro pelo celular.</p>
  </header>
  <div class="dashboard">
    <section id="scene" class="scene panel" aria-labelledby="scene-title">
      <svg id="initial-model" class="initial-model" viewBox="0 0 460 290" aria-hidden="true">
        <path fill="#cab294" d="M80 227l159-26 145 44-158 30z"/>
        <ellipse cx="246" cy="221" rx="42" ry="13" fill="#3d6255"/>
        <path d="M245 218l-7-77-59-60-87 55" fill="none" stroke="#957452" stroke-width="20" stroke-linejoin="round"/>
        <path d="M245 211l-7-70-59-60-87 55" fill="none" stroke="#f0e3cd" stroke-width="14" stroke-linejoin="round"/>
        <path d="M92 136l-28 6-14 19m42-25l-22-6-20 11" fill="none" stroke="#3d6255" stroke-width="7" stroke-linecap="round"/>
        <path d="M252 196l-9-44" stroke="#68a794" stroke-width="10" stroke-linecap="round"/>
        <path d="M252 196q25 57 72 31" fill="none" stroke="#43836b" stroke-width="4"/>
        <g fill="#3d6255"><circle cx="245" cy="211" r="5"/><circle cx="238" cy="141" r="5"/><circle cx="179" cy="81" r="5"/></g>
      </svg>
      <canvas id="model-software" class="model-software" aria-hidden="true" hidden></canvas>
      <canvas id="model" aria-label="Modelo tridimensional interativo da garra hidráulica. Use as setas para girar a câmera." tabindex="0"></canvas>
      <p id="graphics-error" class="graphics-error" hidden>Visualização estática. Os controles continuam disponíveis.</p>
      <div class="scene-head">
        <h2 id="scene-title" class="scene-title">Modelo 3D</h2>
        <div class="view-actions"><button id="demo" class="view-button" type="button" hidden>Demonstração</button><button id="reset" class="view-button" type="button">Recentrar</button></div>
      </div>
      <div class="scene-foot"><span id="activity" class="axis-legend">Em repouso</span><span class="view-hint">Arraste para girar a câmera</span></div>
    </section>
    <section class="controls panel" aria-labelledby="control-title">
      <div><h2 id="control-title">Controle dos eixos</h2><p class="controls-hint">Segure para mover e solte para parar.</p></div>
      <div class="speed"><label for="speed">Velocidade</label><input id="speed" type="range" min="30" max="100" step="5" value="70" disabled><output id="percent" for="speed">70%</output></div>
      <div id="motors" class="motors"></div>
      <button id="stop" class="stop" type="button">Parar tudo</button>
    </section>
  </div>
  <section id="projeto" class="info">
    <h2>Sobre o projeto</h2>
    <div class="info-grid">
      <article class="panel"><h3>ESP32 DevKit V1</h3><p>A placa cria a rede <b>Garra-ESP-Lebron</b> e abre o painel em <b>192.168.4.1</b>, sem internet.</p></article>
      <article class="panel"><h3>Dois motores DC</h3><p>Uma ponte H controla os dois sentidos de rotação. Um motor gira a base e o outro sobe ou desce o ombro.</p></article>
      <article class="panel"><h3>Seringas e água</h3><p>Os pares de seringas motorizados transmitem o movimento da base e do ombro pela água nas mangueiras.</p></article>
    </div>
    <p class="model-note">O 3D estima a posição a partir dos comandos. A placa não mede o ângulo dos eixos.</p>
  </section>
</main>
<script>
'use strict';

const byId = id => document.getElementById(id);
const clamp = (value, low, high) => Math.max(low, Math.min(high, value));
const add = (a, b) => a.map((v, i) => v + b[i]);
const sub = (a, b) => a.map((v, i) => v - b[i]);
const mul = (a, value) => a.map(v => v * value);
const mix = (a, b, value) => add(a, mul(sub(b, a), value));
const cross = (a, b) => [a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0]];
const unit = a => mul(a, 1 / (Math.hypot(...a) || 1));
const up = [0, 1, 0];
const front = [0, 0, 1];
const axisCount = 2;

const shaders = {
  vertex: `
    precision mediump float;
    attribute vec3 aPosition, aNormal, aColor;
    attribute float aMaterial;
    uniform vec3 uEye, uRight, uUp, uForward;
    uniform vec2 uScale;
    varying vec3 vPosition, vNormal, vColor;
    varying float vMaterial;
    void main() {
      vec3 d = aPosition - uEye;
      float z = dot(d, uForward);
      gl_Position = vec4(dot(d,uRight)*uScale.x, dot(d,uUp)*uScale.y, z*1.01-.22, z);
      vPosition = aPosition;
      vNormal = aNormal;
      vColor = aColor;
      vMaterial = aMaterial;
    }
  `,
  fragment: `
    precision mediump float;
    uniform vec3 uEye;
    varying vec3 vPosition, vNormal, vColor;
    varying float vMaterial;
    void main() {
      vec3 p = vPosition;
      vec3 n = normalize(vNormal);
      vec3 eye = normalize(uEye-p);
      vec3 lamp = normalize(vec3(-.54,.84,.42));
      if (vMaterial > 3.5) {
        vec2 grid = abs(fract(p.xz*2.0+.5)-.5);
        float line = 1.0-smoothstep(.008,.038,min(grid.x,grid.y));
        float fade = exp(-dot(p.xz,p.xz)*.085);
        vec2 base = (p.xz-vec2(-.18,0.0))/vec2(2.2,1.16);
        vec2 controller = (p.xz-vec2(1.91,-.45))/vec2(.82,.84);
        float shadow = exp(-2.6*dot(base,base))*.19 + exp(-2.4*dot(controller,controller))*.15;
        vec3 ground = vec3(.844,.885,.850)+fade*vec3(.092,.074,.075)-vec3(line*fade*.075+shadow);
        gl_FragColor = vec4(ground,1.0);
        return;
      }
      float diff = max(dot(n,lamp),0.0);
      float fill = max(dot(n,normalize(vec3(.8,.4,-.4))),0.0);
      float spec = pow(max(dot(n,normalize(lamp+eye)),0.0),vMaterial>1.5 ? 23.0 : 45.0);
      float grain = 1.0;
      if (vMaterial > .5 && vMaterial < 1.5) {
        float stripes = sin(p.y*190.0+p.z*60.0+sin(p.x*14.0+p.z*8.0)*1.1);
        grain = .94+stripes*.035+sin(p.x*43.0+p.z*100.0)*.018;
      }
      vec3 shade = vColor*grain*(.49+.46*diff+.16*fill)+spec*vec3(1.0,.89,.72)*(vMaterial>1.5 ? .30 : .16);
      if (vMaterial > 2.5) shade = mix(shade,vColor,.68);
      gl_FragColor = vec4(shade,1.0);
    }
  `
};

function buildGraphics(canvas) {
  const gl = canvas.getContext('webgl', {alpha:true, antialias:true});
  if (!gl) throw new Error('WebGL indisponível');
  const program = gl.createProgram();
  for (const [type, source] of [[gl.VERTEX_SHADER,shaders.vertex],[gl.FRAGMENT_SHADER,shaders.fragment]]) {
    const shader = gl.createShader(type);
    gl.shaderSource(shader, source);
    gl.compileShader(shader);
    if (!gl.getShaderParameter(shader, gl.COMPILE_STATUS)) throw new Error(gl.getShaderInfoLog(shader));
    gl.attachShader(program, shader);
  }
  gl.linkProgram(program);
  if (!gl.getProgramParameter(program, gl.LINK_STATUS)) throw new Error(gl.getProgramInfoLog(program));
  gl.useProgram(program);
  const buffer = gl.createBuffer();
  gl.bindBuffer(gl.ARRAY_BUFFER, buffer);
  for (const [name, size, offset] of [['aPosition',3,0],['aNormal',3,12],['aColor',3,24],['aMaterial',1,36]]) {
    const location = gl.getAttribLocation(program, name);
    gl.enableVertexAttribArray(location);
    gl.vertexAttribPointer(location,size,gl.FLOAT,false,40,offset);
  }
  const uniforms = Object.fromEntries(['Eye','Right','Up','Forward','Scale'].map(name => [name,gl.getUniformLocation(program,'u'+name)]));
  gl.enable(gl.DEPTH_TEST);
  gl.clearColor(.844,.885,.85,1);
  return function draw(vertices, yaw, pitch) {
    const density = Math.min(devicePixelRatio || 1, 1.65);
    const width = Math.round(canvas.clientWidth*density);
    const height = Math.round(canvas.clientHeight*density);
    if (!width || !height) return false;
    if (canvas.width !== width || canvas.height !== height) {
      canvas.width=width; canvas.height=height;
    }
    const aspect = width/height;
    const distance = 7.45;
    const target = [-.30,1.18,0];
    const eye = add(target,[Math.cos(pitch)*Math.sin(yaw)*distance,Math.sin(pitch)*distance,Math.cos(pitch)*Math.cos(yaw)*distance]);
    const forward = unit(sub(target,eye));
    const right = unit(cross(forward,up));
    const vertical = cross(right,forward);
    const scale = Math.min(2.78,aspect*2.63);
    gl.viewport(0,0,width,height);
    gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT);
    gl.uniform3fv(uniforms.Eye,eye);
    gl.uniform3fv(uniforms.Right,right);
    gl.uniform3fv(uniforms.Up,vertical);
    gl.uniform3fv(uniforms.Forward,forward);
    gl.uniform2f(uniforms.Scale,scale/aspect,scale);
    gl.bufferData(gl.ARRAY_BUFFER,vertices,gl.DYNAMIC_DRAW);
    gl.drawArrays(gl.TRIANGLES,0,vertices.length/10);
  };
}

// Desenha a mesma geometria em Canvas 2D se o WebGL não estiver disponível.
function buildSoftwareGraphics(surface) {
  const ctx = surface.getContext('2d');
  if (!ctx) return null;
  const light = unit([-.54,.84,.42]);
  return (vertices, yaw, pitch) => {
    const w = canvas.clientWidth, h = canvas.clientHeight;
    if (!w || !h) return false;
    const density = Math.min(devicePixelRatio || 1,1.35);
    const width = Math.round(w*density), height = Math.round(h*density);
    if (surface.width !== width || surface.height !== height) {
      surface.width=width; surface.height=height;
    }
    ctx.setTransform(density,0,0,density,0,0);
    ctx.clearRect(0,0,w,h);
    const target=[-.30,1.18,0];
    const eye=add(target,[Math.cos(pitch)*Math.sin(yaw)*7.45,Math.sin(pitch)*7.45,Math.cos(pitch)*Math.cos(yaw)*7.45]);
    const forward=unit(sub(target,eye)), right=unit(cross(forward,up)), vertical=cross(right,forward);
    const scale=Math.min(2.78,w/h*2.63)*h/2;
    const dot=(a,b)=>a[0]*b[0]+a[1]*b[1]+a[2]*b[2];
    function project(x,y,z){
      const d=[x-eye[0],y-eye[1],z-eye[2]], depth=dot(d,forward);
      return [w/2+scale*dot(d,right)/depth,h/2-scale*dot(d,vertical)/depth,depth];
    }
    function path(points){
      ctx.beginPath();
      ctx.moveTo(points[0][0],points[0][1]);
      for(let i=1;i<points.length;i++) ctx.lineTo(points[i][0],points[i][1]);
    }
    ctx.strokeStyle='#cadbce';
    ctx.lineWidth=1;
    for(let i=-5;i<=5;i++){
      const n=i*.5;
      path([project(n,0,-2.5),project(n,0,2.5)]); ctx.stroke();
      path([project(-2.5,0,n),project(2.5,0,n)]); ctx.stroke();
    }
    const faces=[];
    for(let i=0;i<vertices.length;i+=30){
      const v=vertices, material=v[i+9];
      if(material>3.5) continue;
      const normal=[v[i+3]+v[i+13]+v[i+23],v[i+4]+v[i+14]+v[i+24],v[i+5]+v[i+15]+v[i+25]];
      if(dot(normal,[eye[0]-v[i],eye[1]-v[i+1],eye[2]-v[i+2]])<=0) continue;
      const points=[i,i+10,i+20].map(j=>project(v[j],v[j+1],v[j+2]));
      if(points.some(point=>point[2]<=.15)) continue;
      const shade=material>2.5?.98:.64+.40*Math.max(0,dot(unit(normal),light));
      const color=[v[i+6],v[i+7],v[i+8]].map(c=>Math.round(clamp(c*shade,0,1)*255));
      faces.push({points,depth:points.reduce((sum,p)=>sum+p[2],0),color:'rgb('+color.join(',')+')'});
    }
    faces.sort((a,b)=>b.depth-a.depth);
    ctx.lineWidth=.6;
    for(const face of faces){
      path(face.points);
      ctx.closePath();
      ctx.fillStyle=ctx.strokeStyle=face.color;
      ctx.fill();
      ctx.stroke();
    }
    return true;
  };
}

const palette = {
  rim:[.55,.32,.20], edge:[.67,.44,.30], wood:[.80,.56,.36], paint:[.96,.93,.84],
  pin:[.68,.74,.75], cap:[.29,.35,.35], motor:[.37,.41,.40], base:[.21,.25,.24],
  rubber:[.13,.17,.16], hose:[.13,.43,.30], water:[.39,.72,.62], barrel:[.55,.65,.61],
  green:[.29,.74,.48], amber:[.04,.58,.36], board:[.19,.39,.31], metal:[.46,.49,.46],
  mark:[.24,.36,.31]
};

function makeMesh(pose, motion, time, simple=false) {
  const vertices = [];
  function vertex(p, n, color, material) {
    vertices.push(p[0],p[1],p[2],n[0],n[1],n[2],color[0],color[1],color[2],material);
  }
  function triangle(a,b,c,n,color,material) {
    vertex(a,n,color,material);
    vertex(b,n,color,material);
    vertex(c,n,color,material);
  }
  function quad(a,b,c,d,n,color,material) {
    triangle(a,b,c,n,color,material); triangle(a,c,d,n,color,material);
  }
  function box(center, dimensions, color, material=0, basis=[[1,0,0],up,front], edge=color) {
    const [x,y,z]=basis.map((axis,i)=>mul(axis,dimensions[i]/2));
    const corner=(a,b,c)=>add(center,add(mul(x,a),add(mul(y,b),mul(z,c))));
    for (const side of [-1,1]) {
      quad(corner(side,-1,-1),corner(side,1,-1),corner(side,1,1),corner(side,-1,1),mul(basis[0],side),edge,material);
      quad(corner(-1,side,-1),corner(1,side,-1),corner(1,side,1),corner(-1,side,1),mul(basis[1],side),edge,material);
      quad(corner(-1,-1,side),corner(1,-1,side),corner(1,1,side),corner(-1,1,side),mul(basis[2],side),color,material);
    }
  }
  function cylinder(a,b,radius,color,material=0,sides=12,capped=true) {
    if(simple) sides=Math.min(sides,6);
    const axis = unit(sub(b,a));
    const tangent = unit(cross(axis,Math.abs(axis[1])<.88 ? up : front));
    const bitangent = cross(axis,tangent);
    for(let i=0;i<sides;i++){
      const angle = i*2*Math.PI/sides;
      const next = (i+1)*2*Math.PI/sides;
      const normal = t=>add(mul(tangent,Math.cos(t)),mul(bitangent,Math.sin(t)));
      const n0=normal(angle), n1=normal(next);
      const a0=add(a,mul(n0,radius)),a1=add(a,mul(n1,radius));
      const b0=add(b,mul(n0,radius)),b1=add(b,mul(n1,radius));
      vertex(a0,n0,color,material);vertex(b0,n0,color,material);vertex(b1,n1,color,material);
      vertex(a0,n0,color,material);vertex(b1,n1,color,material);vertex(a1,n1,color,material);
      if(capped){
        triangle(a,a0,a1,mul(axis,-1),color,material);
        triangle(b,b0,b1,axis,color,material);
      }
    }
  }
  function plate(a,b,width,depth,color=palette.paint) {
    const axis=unit(sub(b,a)), side=unit(cross(front,axis)), normal=cross(axis,side);
    box(mix(a,b,.5),[Math.hypot(...sub(b,a)),width,depth],color,1,[axis,side,normal],palette.edge);
    for(const p of [a,b]) cylinder(add(p,mul(normal,-depth/2)),add(p,mul(normal,depth/2)),width/2,color,1);
  }
  function joint(point,radius=.115,span=.54,active=false) {
    const a=add(point,[0,0,-span/2]),b=add(point,[0,0,span/2]);
    cylinder(a,b,radius,palette.cap);
    for(const side of [-1,1]) {
      const cap=add(point,[0,0,side*(span/2+.025)]);
      cylinder(add(cap,[0,0,-.026]),add(cap,[0,0,.026]),radius*.83,active ? palette.amber : palette.pin,active ? 3 : 0);
      cylinder(add(cap,[0,0,side*.024]),add(cap,[0,0,side*.037]),radius*.44,palette.cap,0,6);
    }
  }
  function syringe(a,b,active=false,scale=1) {
    const d=unit(sub(b,a));
    const at=f=>add(a,mul(d,f*scale));
    const glass=active ? [.69,.83,.71] : palette.barrel;
    const liquid=active ? palette.amber : palette.water;
    cylinder(at(-.09),at(.04),.036*scale,palette.rubber,2);
    cylinder(at(.035),at(.10),.055*scale,palette.metal,2);
    cylinder(at(.10),at(.56),.084*scale,glass,2);
    cylinder(at(.16),at(.46),.088*scale,liquid,active ? 3 : 2);
    cylinder(at(.55),at(.575),.107*scale,palette.paint,2);
    cylinder(at(.57),b,.036*scale,palette.pin,2);
    cylinder(add(b,mul(d,-.033*scale)),b,.077*scale,palette.rubber,2);
    const radial = unit(sub(front,mul(d,d[2])));
    for(let j=0;j<5;j++){
      const p = add(at(.19+j*.058),mul(radial,.088*scale));
      const lateral = unit(cross(radial,d));
      cylinder(p,add(p,mul(lateral,(j%2 ? .025 : .042)*scale)),.0055*scale,palette.mark,0,5);
    }
    return at(-.095);
  }
  function tube(a,c1,c2,b,active,phase) {
    function at(t){
      const s=1-t;
      return add(add(mul(a,s*s*s),mul(c1,3*s*s*t)),add(mul(c2,3*s*t*t),mul(b,t*t*t)));
    }
    const color=active ? [.24,.60,.39] : palette.hose;
    let old=at(0);
    const segments=simple?8:17;
    for(let i=1;i<=segments;i++){
      const p=at(i/segments);
      cylinder(old,p,.025,color,2,7,false);
      old=p;
    }
    if(active){
      const t=((time*.74+phase)%1+1)%1;
      cylinder(at(t),at(Math.min(1,t+.054)),.035,palette.amber,3,8);
    }
  }

  quad([-18,0,-18],[-18,0,18],[18,0,18],[18,0,-18],up,palette.base,4);
  box([-.40,.067,.25],[3.97,.13,2.12],palette.paint,1,[[1,0,0],front,up],palette.rim);
  box([-.40,.142,.25],[3.94,.025,2.09],palette.paint,0,[[1,0,0],front,up]);
  box([1.96,.060,-.53],[1.20,.12,1.84],palette.paint,1,[[1,0,0],front,up],palette.rim);
  box([1.96,.127,-.53],[1.17,.025,1.81],palette.paint,0,[[1,0,0],front,up]);

  const pivot=[.18,0,.16];
  const swivel=pose[0];
  const cosine=Math.cos(swivel),sine=Math.sin(swivel);
  const start=vertices.length;
  cylinder(add(pivot,[0,.15,0]),add(pivot,[0,.22,0]),.55,palette.base,0,22);
  cylinder(add(pivot,[0,.22,0]),add(pivot,[0,.30,0]),.48,motion[0] ? palette.amber : palette.rim,0,22);
  for(const x of [-.20,.55])for(const z of [-.13,.46]){
    cylinder([x,.30,z],[x,.321,z],.047,palette.pin,0,7);
  }
  const anchor=[.18,.32,.16], shoulder=[.035,1.22,.16];
  const angle=pose[1];
  const elbow=add(shoulder,[-Math.cos(angle)*.92,Math.sin(angle)*.92,0]);
  const angle2=angle-1.16;
  const wrist=add(elbow,[-Math.cos(angle2)*1.21,Math.sin(angle2)*1.21,0]);
  for(const z of [-.205,.205]){
    const offset=[0,0,z];
    plate(add(anchor,offset),add(shoulder,offset),.37,.105);
    plate(add(shoulder,offset),add(elbow,offset),.31,.095);
    plate(add(elbow,offset),add(wrist,offset),.30,.095);
  }
  joint(anchor,.106,.49,motion[0]); joint(shoulder,.123,.54,motion[1]); joint(elbow,.12,.54); joint(wrist,.089,.47);
  const handAngle=angle2*.32-.10;
  const grip=add(wrist,[-.24*Math.cos(handAngle),.24*Math.sin(handAngle),0]);
  plate(wrist,grip,.24,.33,palette.wood);
  for(const side of [-1,1]){
    const a=add(grip,[0,0,side*.15]);
    const jawAngle=.46;
    const b=add(a,[-.37*Math.cos(jawAngle),.035,side*.37*Math.sin(jawAngle)]);
    const tip=add(b,[-.21,.045,-side*.072]);
    plate(a,b,.13,.10,palette.paint);
    plate(b,tip,.115,.10,palette.paint);
    box(tip,[.13,.13,.14],palette.rubber,2);
    cylinder(add(a,[0,-.05,0]),add(a,[0,.09,0]),.054,palette.pin,0,8);
  }
  const waterStarts=[
    syringe([.60,.39,.16],[-.08,.47,.16],motion[0],.87),
    syringe([.34,.54,.16],mix(shoulder,elbow,.69),motion[1],1.05)
  ];
  function rotate(p, normal=false){
    const [x,y,z]=normal?p:sub(p,pivot);
    const v=[x*cosine+z*sine,y,z*cosine-x*sine];
    return normal?v:add(v,pivot);
  }
  for(let i=start;i<vertices.length;i+=10){
    const p=rotate(vertices.slice(i,i+3));
    const n=rotate(vertices.slice(i+3,i+6),true);
    for(let j=0;j<3;j++){
      vertices[i+j]=p[j]; vertices[i+3+j]=n[j];
    }
  }
  for(let i=0;i<axisCount;i++){
    const x=1.64+i*.46;
    const axis=mechanisms[i];
    const travel=(pose[i]-axis.min)/(axis.max-axis.min)-.5;
    const head=[x,.24,.17], tail=[x,.57+travel*.12,-.81+travel*.23];
    box([x,.21,-.83],[.26,.11,.26],palette.wood,1);
    plate([x,.25,-.84],[x,.68,-.89],.13,.14,palette.wood);
    const end=syringe(head,tail,motion[i],.82);
    box([x,.28,-1.12],[.22,.20,.32],palette.motor,0);
    box([x,.30,-1.11],[.224,.09,.20],palette.rubber,2);
    const origin=rotate(waterStarts[i]);
    const c1=add(origin,[.23,.08,.48+i*.12]);
    const c2=add(end,[-.30,.10,.75-i*.13]);
    tube(origin,c1,c2,end,motion[i],i*.21);
  }
  box([1.99,.20,-1.14],[.51,.046,.38],palette.board,0,[[1,0,0],front,up]);
  box([1.99,.236,-1.15],[.24,.033,.21],palette.metal);
  box([2.14,.234,-1.09],[.07,.03,.09],palette.rubber);
  box([1.82,.235,-1.08],[.05,.034,.05],palette.green,3);
  return new Float32Array(vertices);
}

const mechanisms = [
  {name:'Base', labels:['Esquerda','Direita'], arrows:['←','→'], symbol:'↔', min:-.88,max:.88,rate:1.02,direction:1},
  {name:'Ombro', labels:['Desce','Sobe'], arrows:['↓','↑'], symbol:'↕', min:.24,max:1.52,rate:.85,direction:1}
];
const pose = [0,.96];
const visualMotion = [0,0];
const held = new Map();
const released = new Set();
const connection = byId('connection');
const speed = byId('speed');
const percent = byId('percent');
const activity = byId('activity');
const canvas = byId('model');
const preview = location.hostname !== '192.168.4.1';
const simulated = {v:70,j:mechanisms.map(({name,labels})=>({n:name,m:labels[0],p:labels[1],d:0,t:false}))};
const simulatedStarts = new Map();
let currentState = preview ? simulated : null;
let pendingSpeed = null;
let speedInFlight = null;
let stopPending = false;
let busy = false;
let dirty = false;
let everConnected = false;
let nextPing = 0;
let demoStart = null;
let demoStep = -1;
let sceneDirty = true;

const cards = mechanisms.map(({name,labels,arrows,symbol},index)=>{
  const card = document.createElement('article');
  card.className = 'motor';
  card.innerHTML = '<header class="motor-head"><span class="motor-symbol" aria-hidden="true"></span><h3></h3><p class="tag"></p></header><div class="actions"></div>';
  card.querySelector('.motor-symbol').textContent = symbol;
  const title=card.querySelector('h3');
  const tag=card.querySelector('.tag');
  title.textContent=name;
  tag.textContent='Parado';
  const buttons = labels.map((label, i)=>{
    const direction = i ? 1 : -1;
    const button=document.createElement('button');
    button.type='button';
    button.className='hold';
    button.disabled=true;
    button.innerHTML='<span class="direction" aria-hidden="true"></span><span class="label"></span>';
    button.firstElementChild.textContent=arrows[i];
    button.lastElementChild.textContent=label;
    button.addEventListener('pointerdown', event=>{
      if(event.button!==0 || !start(index,direction,button,event.pointerId)) return;
      event.preventDefault();
      button.focus({preventScroll:true});
      try { button.setPointerCapture(event.pointerId); }
      catch { stopAll(); }
    });
    for(const type of ['pointerup','pointercancel','lostpointercapture']){
      button.addEventListener(type,event=>release(index,button,event.pointerId));
    }
    button.addEventListener('contextmenu',event=>event.preventDefault());
    button.addEventListener('keydown',event=>{
      if(event.key!==' ' && event.key!=='Enter') return;
      event.preventDefault();
      if(!event.repeat) start(index,direction,button,'key');
    });
    button.addEventListener('keyup',event=>{
      if(event.key!==' ' && event.key!=='Enter') return;
      event.preventDefault();
      release(index,button,'key');
    });
    button.addEventListener('blur',()=>release(index,button,'key'));
    card.querySelector('.actions').append(button);
    return button;
  });
  byId('motors').append(card);
  return {title,tag,buttons,card};
});

function start(index,direction,button,source){
  if(document.hidden || !currentState?.j[index] || stopPending || held.has(index)) return false;
  cancelDemo();
  held.set(index,{direction,button,source});
  button.classList.add('active');
  send();
  return true;
}

function release(index,button,source){
  const press=held.get(index);
  if(press?.button!==button || press.source!==source) return;
  held.delete(index);
  press.button.classList.remove('active');
  released.add(index);
  render();
  send();
}

function clearHeld(){
  for(const press of held.values()) press.button?.classList.remove('active');
  held.clear();
}

function cancelDemo(){
  if(demoStart===null) return;
  demoStart=null;
  demoStep=-1;
  for(const [index,press] of held){
    if(press.source==='demo'){
      held.delete(index);
      released.add(index);
    }
  }
  byId('demo').textContent='Demonstração';
}

function stopAll(){
  cancelDemo();
  for(const index of held.keys()) released.add(index);
  clearHeld();
  stopPending=true;
  render();
  send();
}

function render(){
  const mode=preview?'preview':currentState?'live':everConnected?'offline':'connecting';
  const connectionLabel={preview:'Modo simulação',live:'ESP32 conectado',offline:'ESP32 desconectado',connecting:'Buscando ESP32…'}[mode];
  connection.dataset.mode=mode;
  if(connection.textContent!==connectionLabel) connection.textContent=connectionLabel;
  const activeNames=[];
  cards.forEach(({title,tag,buttons,card},index)=>{
    const axis=currentState?.j[index];
    const direction=stopPending || axis?.t ? 0 : axis?.d || 0;
    const target=held.get(index)?.direction;
    visualMotion[index]=direction===target ? direction : 0;
    card.dataset.state=axis?.t && !stopPending ? 'limit' : direction ? 'move' : 'idle';
    tag.textContent=axis?.t && !stopPending ? 'Solte o botão' : direction ? (direction>0?axis.p:axis.m) : axis ? 'Parado' : 'Indisponível';
    buttons.forEach((button,i)=>{
      button.disabled=!axis;
      button.classList.toggle('commanded',direction === (i ? 1 : -1) && direction !== 0);
    });
    if(!axis) return;
    title.textContent=axis.n;
    buttons[0].lastElementChild.textContent=axis.m;
    buttons[1].lastElementChild.textContent=axis.p;
    if(visualMotion[index]) activeNames.push(axis.n);
  });
  activity.textContent=activeNames.length ? activeNames.join(' + ')+' em movimento' : 'Em repouso';
  activity.classList.toggle('moving',activeNames.length>0);
  speed.disabled=!currentState;
  if(currentState && pendingSpeed===null && speedInFlight===null) speed.value=currentState.v;
  percent.value=speed.value+'%';
}

function simulate(commands){
  for(const command of commands){
    const [type,rawIndex,rawDirection]=command.split(',');
    if(type==='x'){
      simulated.j.forEach(axis=>{axis.d=0;});
      simulatedStarts.clear();
    }else if(type==='v'){
      simulated.v=Number(rawIndex);
    }else if(type==='m'){
      const index=Number(rawIndex),direction=Number(rawDirection);
      const axis=simulated.j[index];
      if(!direction){
        simulatedStarts.delete(index);
        axis.d=0;
        axis.t=false;
      }else if(!axis.t){
        const now=performance.now();
        if(direction!==axis.d || !simulatedStarts.has(index)) simulatedStarts.set(index,now);
        axis.t=now-simulatedStarts.get(index)>=6000;
        axis.d=axis.t?0:direction;
      }
    }
  }
  return simulated;
}

function consultarPlaca(commands){
  return new Promise((resolve,reject)=>{
    const req=new XMLHttpRequest();
    req.open('GET','/cmd?c='+encodeURIComponent(commands.join(';')),true);
    req.timeout=700;
    req.onload=()=>{
      if(req.status!==200){reject(new Error('HTTP '+req.status));return;}
      try{resolve(JSON.parse(req.responseText));}catch(error){reject(error);}
    };
    req.onerror=req.ontimeout=req.onabort=()=>reject(new Error('Sem resposta do ESP32'));
    req.send();
  });
}

async function send(){
  if(busy){dirty=true;return;}
  busy=true;
  const sendingStop=stopPending;
  const commands=sendingStop?['x']:[];
  for(const index of released) commands.push('m,'+index+',0');
  released.clear();
  speedInFlight=pendingSpeed;
  pendingSpeed=null;
  if(speedInFlight!==null) commands.push('v,'+speedInFlight);
  if(!sendingStop && !document.hidden){
    for(const [index,press] of held) commands.push('m,'+index+','+press.direction);
  }
  if(!commands.length) commands.push('p');
  try{
    if(preview){
      currentState=simulate(commands);
    }else{
      const result=await consultarPlaca(commands);
      if(!Number.isFinite(result.v) || !Array.isArray(result.j) || result.j.length<axisCount ||
         !result.j.slice(0,axisCount).every(axis=>axis && [-1,0,1].includes(axis.d) && typeof axis.n==='string' && typeof axis.m==='string' && typeof axis.p==='string')){
        throw new Error('Estado do ESP32 inválido');
      }
      currentState={v:result.v,j:result.j.slice(0,axisCount)};
    }
    everConnected=true;
    if(sendingStop) stopPending=false;
  }catch{
    if(currentState) everConnected=true;
    currentState=null;
    clearHeld();
    stopPending=false;
    connection.textContent='ESP32 desconectado';
    everConnected=true;
  }finally{
    speedInFlight=null;
    render();
    busy=false;
    nextPing=Date.now()+900;
    if(dirty){dirty=false;send();}
  }
}

speed.addEventListener('input',()=>{
  pendingSpeed=Number(speed.value);
  percent.value=speed.value+'%';
  send();
});
byId('stop').addEventListener('click',stopAll);
window.addEventListener('blur',()=>{if(held.size || demoStart!==null) stopAll();});
document.addEventListener('visibilitychange',()=>{if(document.hidden && (held.size || demoStart!==null)) stopAll();});
window.addEventListener('pagehide',()=>{if(held.size || demoStart!==null) stopAll();});
setInterval(()=>{
  if(!busy && !document.hidden && (held.size || Date.now()>=nextPing)) send();
},125);

const routine=[
  [980,[[0,-1],[1,1]]],
  [1120,[[0,1]]],
  [1220,[[1,-1]]],
  [400,[]],
  [650,[[0,-1],[1,1]]]
];
function animateDemo(now){
  if(demoStart===null) return;
  let duration=now-demoStart;
  const step=routine.findIndex(([length])=>{
    duration-=length;
    return duration<0;
  });
  if(step===demoStep) return;
  for(const index of held.keys()) released.add(index);
  clearHeld();
  if(step<0){cancelDemo();send();return;}
  demoStep=step;
  for(const [index,direction] of routine[step][1]) held.set(index,{direction,source:'demo'});
  send();
}

const defaultView=[.50,.34];
const reducedMotion=matchMedia('(prefers-reduced-motion: reduce)');
const camera=[defaultView[0]+(reducedMotion.matches?0:.28),defaultView[1]];
const viewTarget=[...defaultView];
const demo=byId('demo');
const reset=byId('reset');
let draw=null;
let drag=null;
let mesh=null;
let previous=0;
let lastDraw=0;
let needsMesh=true;
const fallbackCanvas=byId('model-software');
const initialModel=byId('initial-model');
let software=false;
function selectRenderer(forceSoftware=false){
  draw=null;
  software=forceSoftware;
  if(!software){
    try{draw=buildGraphics(canvas);}catch{software=true;}
  }
  if(software){
    try{draw=buildSoftwareGraphics(fallbackCanvas);}catch{draw=null;}
  }
  canvas.style.opacity=software?'0':'1';
  fallbackCanvas.hidden=!(software && draw);
  byId('graphics-error').hidden=Boolean(draw);
  reset.disabled=!draw;
  demo.hidden=!preview || !draw;
  needsMesh=true;
  sceneDirty=true;
}
selectRenderer();
canvas.addEventListener('webglcontextlost',event=>{
  event.preventDefault();
  selectRenderer(true);
});
canvas.addEventListener('webglcontextrestored',()=>selectRenderer());
demo.hidden=!preview || !draw;
demo.addEventListener('click',()=>{
  if(!preview || !draw) return;
  if(demoStart!==null){stopAll();return;}
  for(const index of held.keys()) released.add(index);
  clearHeld();
  demoStart=performance.now();
  demoStep=-1;
  demo.textContent='Parar demo';
  animateDemo(demoStart);
});
reset.addEventListener('click',()=>{[viewTarget[0],viewTarget[1]]=defaultView;sceneDirty=true;});
canvas.addEventListener('pointerdown',event=>{
  if(event.button!==0 || drag) return;
  drag={id:event.pointerId,x:event.clientX,y:event.clientY};
  canvas.setPointerCapture(event.pointerId);
});
canvas.addEventListener('pointermove',event=>{
  if(drag?.id!==event.pointerId) return;
  viewTarget[0]-=(event.clientX-drag.x)*.009;
  viewTarget[1]=clamp(viewTarget[1]+(event.clientY-drag.y)*.006,.09,1.12);
  drag.x=event.clientX;
  drag.y=event.clientY;
});
for(const type of ['lostpointercapture','pointercancel','pointerup']){
  canvas.addEventListener(type,event=>{if(drag?.id===event.pointerId) drag=null;});
}
canvas.addEventListener('keydown',event=>{
  const deltas={ArrowLeft:[.16,0],ArrowRight:[-.16,0],ArrowUp:[0,.12],ArrowDown:[0,-.12]};
  if(!deltas[event.key]) return;
  event.preventDefault();
  const [yaw,pitch]=deltas[event.key];
  viewTarget[0]+=yaw;
  viewTarget[1]=clamp(viewTarget[1]+pitch,.09,1.12);
});
if(window.ResizeObserver) new ResizeObserver(()=>{sceneDirty=true;}).observe(canvas);
else window.addEventListener('resize',()=>{sceneDirty=true;});

let lastMotion='';
function frame(now){
  requestAnimationFrame(frame);
  if(document.hidden || now-lastDraw<(software?55:25)) return;
  lastDraw=now;
  animateDemo(now);
  const delta=previous ? clamp((now-previous)/1000,0,.06) : 0;
  previous=now;
  const power=Number(speed.value)/100;
  mechanisms.forEach((axis,index)=>{
    const position=clamp(pose[index]+visualMotion[index]*delta*axis.rate*power*axis.direction,axis.min,axis.max);
    if(position!==pose[index]){pose[index]=position;needsMesh=true;}
  });
  for(let i=0;i<2;i++){
    const distance=viewTarget[i]-camera[i];
    if(Math.abs(distance)>.001){
      camera[i]+=distance*(reducedMotion.matches?1:Math.min(delta*12,1));
      sceneDirty=true;
    }
  }
  if(!draw) return;
  const motionKey=visualMotion.join(',');
  if(needsMesh || motionKey!==lastMotion || visualMotion.some(Boolean)){
    mesh=makeMesh(pose,visualMotion,now/1000,software);
    lastMotion=motionKey;
    needsMesh=false;
    sceneDirty=true;
  }
  if(sceneDirty && mesh){
    try{
      if(draw(mesh,camera[0],camera[1])!==false) initialModel.hidden=true;
      sceneDirty=false;
    }catch{
      if(!software) selectRenderer(true);
      else{
        draw=null;
        fallbackCanvas.hidden=true;
        initialModel.hidden=false;
        byId('graphics-error').hidden=false;
      }
    }
  }
}
render();
requestAnimationFrame(frame);
send();
</script>
</body>
</html>
)garrahtml";

// Ícone e manifesto: permitem "Adicionar à tela inicial" com cara de app
const char ICONE_SVG[] PROGMEM = R"rawliteral(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 512 512"><rect width="512" height="512" rx="112" fill="#17644d"/><g fill="none" stroke="#ffffff" stroke-width="40" stroke-linecap="round" stroke-linejoin="round"><path d="M110 410h170M195 410v-100l105-125M300 185l80-80M380 105l60 20M380 105l-20 60"/></g><circle cx="195" cy="310" r="30" fill="#ffffff"/></svg>)rawliteral";

const char MANIFESTO_JSON[] PROGMEM = R"rawliteral({"name":"Garra Hidráulica","short_name":"Garra","start_url":"/","display":"standalone","orientation":"any","background_color":"#f3f5f2","theme_color":"#f3f5f2","icons":[{"src":"/icon.svg","sizes":"any","type":"image/svg+xml","purpose":"any"}]})rawliteral";

// ============================================================
//  PROGRAMA
// ============================================================

int   direcao[NUM_MOTORES];          // -1, 0, 1
float duty[NUM_MOTORES];             // força atual (0..1), sobe na rampa
unsigned long ultimoCmd[NUM_MOTORES];
unsigned long inicioGiro[NUM_MOTORES];
bool  travado[NUM_MOTORES];          // parou por tempo máximo: espera soltar o botão
int   velocidade = VELOCIDADE_PADRAO;

WebServer server(80);

// ------------------------------------------------------------
//  PWM via LEDC (nativo do ESP32), compatível com core 2.x e 3.x
// ------------------------------------------------------------
#define PWM_MAX ((1 << PWM_BITS) - 1)

#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  void pwmIniciar(uint8_t pino, uint8_t) { ledcAttach(pino, PWM_FREQ, PWM_BITS); }
  void pwmEscrever(uint8_t pino, uint8_t, uint32_t v) { ledcWrite(pino, v); }
#else
  void pwmIniciar(uint8_t pino, uint8_t canal) { ledcSetup(canal, PWM_FREQ, PWM_BITS); ledcAttachPin(pino, canal); }
  void pwmEscrever(uint8_t, uint8_t canal, uint32_t v) { ledcWrite(canal, v); }
#endif

void aplicarMotor(int i) {
  int d = direcao[i];
  if (MOTORES[i].invertido) d = -d;
  uint32_t v = (uint32_t)(duty[i] * PWM_MAX);
  uint32_t a = (d > 0) ? v : 0;
  uint32_t b = (d < 0) ? v : 0;
  pwmEscrever(MOTORES[i].pinoA, i * 2,     a);
  pwmEscrever(MOTORES[i].pinoB, i * 2 + 1, b);
}

void pararMotor(int i) {
  direcao[i] = 0;
  duty[i] = 0;
  aplicarMotor(i);
}

void pararTudo() {
  for (int i = 0; i < NUM_MOTORES; i++) pararMotor(i);
}

// ------------------------------------------------------------
String estadoJson() {
  String s = "{\"v\":" + String(velocidade) + ",\"j\":[";
  for (int i = 0; i < NUM_MOTORES; i++) {
    if (i) s += ",";
    s += "{\"n\":\"" + String(MOTORES[i].nome) + "\"" +
         ",\"m\":\"" + String(MOTORES[i].botaoMenos) + "\"" +
         ",\"p\":\"" + String(MOTORES[i].botaoMais) + "\"" +
         ",\"d\":"   + String(direcao[i]) +
         ",\"t\":"   + (travado[i] ? "true" : "false") + "}";
  }
  return s + "]}";
}

void tratarMensagem(const char* msg) {
  int j, v;
  unsigned long agora = millis();
  switch (msg[0]) {
    case 'm':
      if (sscanf(msg, "m,%d,%d", &j, &v) == 2 && j >= 0 && j < NUM_MOTORES) {
        v = constrain(v, -1, 1);
        if (v == 0) { pararMotor(j); travado[j] = false; break; }
        if (travado[j]) { ultimoCmd[j] = agora; break; }   // ainda segurando: ignora
        if (v != direcao[j]) {               // começou a girar (ou trocou o sentido)
          duty[j] = 0;
          inicioGiro[j] = agora;
          direcao[j] = v;
        }
        ultimoCmd[j] = agora;
      }
      break;
    case 'v':
      if (sscanf(msg, "v,%d", &v) == 1) velocidade = constrain(v, VELOCIDADE_MIN, 100);
      break;
    case 'x':
      pararTudo();
      Serial.println("PARAR acionado");
      break;
    default:   // 'p' (ping)
      break;
  }
}

// ------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n=== Garra hidraulica ESP32 (motores DC) ===");

  for (int i = 0; i < NUM_MOTORES; i++) {
    pwmIniciar(MOTORES[i].pinoA, i * 2);
    pwmIniciar(MOTORES[i].pinoB, i * 2 + 1);
    pararMotor(i);
  }

  WiFi.mode(WIFI_AP);
  if (!WiFi.softAP(WIFI_SSID, WIFI_SENHA)) {
    Serial.println("Falha ao iniciar o Wi-Fi da garra.");
    return;
  }
  Serial.print("Rede: ");
  Serial.println(WIFI_SSID);
  Serial.print("Abra no celular: http://");
  Serial.println(WiFi.softAPIP());

  server.on("/", []() { server.sendHeader("Cache-Control", "no-store"); server.send_P(200, "text/html; charset=utf-8", PAINEL_3D_HTML); });
  server.on("/icon.svg", []() {
    server.sendHeader("Cache-Control", "max-age=86400");
    server.send(200, "image/svg+xml", ICONE_SVG);
  });
  server.on("/manifest.json", []() { server.send(200, "application/manifest+json", MANIFESTO_JSON); });
  server.on("/cmd", []() {
    if (server.hasArg("c")) {            // vários comandos separados por ';'
      String c = server.arg("c");
      int ini = 0;
      while (ini < (int)c.length() && ini < 200) {
        int fim = c.indexOf(';', ini);
        if (fim < 0) fim = c.length();
        tratarMensagem(c.substring(ini, fim).c_str());
        ini = fim + 1;
      }
    }
    server.send(200, "application/json", estadoJson());
  });
  server.onNotFound([]() {
    server.sendHeader("Location", "/");
    server.send(302, "text/plain", "");
  });
  server.begin();
}

void loop() {
  server.handleClient();

  static unsigned long t = 0;
  unsigned long agora = millis();
  if (agora - t < 10) return;
  t = agora;

  float alvo = velocidade / 100.0f;
  float passo = alvo * 10.0f / RAMPA_MS;

  for (int i = 0; i < NUM_MOTORES; i++) {
    // Destrava quando o botão foi solto (comandos pararam de chegar)
    if (travado[i] && agora - ultimoCmd[i] > TEMPO_SEM_COMANDO_MS) travado[i] = false;
    if (direcao[i] == 0) continue;

    // Segurança: botão solto / sinal perdido
    if (agora - ultimoCmd[i] > TEMPO_SEM_COMANDO_MS) {
      pararMotor(i);
      continue;
    }
    // Segurança: girando tempo demais sem soltar
    if (agora - inicioGiro[i] > TEMPO_MAX_MS) {
      Serial.printf("%s: tempo maximo atingido, parado\n", MOTORES[i].nome);
      pararMotor(i);
      travado[i] = true;
      continue;
    }
    // Partida suave
    duty[i] = min(alvo, duty[i] + passo);
    aplicarMotor(i);
  }
}