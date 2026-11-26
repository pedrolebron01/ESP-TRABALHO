// ============================================================
//  Garra hidráulica (seringas + água) com MOTORES DC + PONTE H
//  controlada pelo celular.
//
//  NÃO PRECISA DE NENHUMA BIBLIOTECA EXTRA: usa só o que já vem
//  com o suporte ao ESP32 na Arduino IDE (WiFi, WebServer e LEDC).
//  Tudo está neste arquivo único: a configuração fica logo abaixo.
//
//  Funciona com ponte H L298N, L9110S, TB6612 ou parecidas
//  (2 entradas por motor). No L298N, deixe os jumpers ENA/ENB.
//
//  Comandos em http://192.168.4.1/cmd?c=<comando>
//    m,<motor>,<dir>   dir = 1 (botão +), -1 (botão −), 0 (parar)
//                      o celular repete enquanto o botão está apertado
//    v,<0-100>         velocidade em %
//    x                 PARAR tudo
//    p                 ping
//  Vários comandos podem ir juntos, separados por ';'  ex: m,0,1;m,1,-1
//  Toda resposta é um JSON com o estado atual.
// ============================================================
#include <WiFi.h>
#include <WebServer.h>
#if __has_include(<esp_arduino_version.h>)
#include <esp_arduino_version.h>
#endif

// ============================================================
//  CONFIGURAÇÃO  (edite aqui: pinos, nomes, rede, velocidade)
// ============================================================

// ---------- Rede Wi-Fi criada pelo ESP32 (modo Access Point) ----------
// O celular conecta nesta rede e abre http://192.168.4.1
#define WIFI_SSID   "Garra-ESP-Lebron"
#define WIFI_SENHA  "lebronlindo"   // mínimo 8 caracteres

// ---------- Motores (cada um = 1 motor DC + 1 par de seringas) ----------
// 8 seringas = 4 pares = 4 motores = 2 módulos de ponte H (2 motores cada).
//
// pinoA / pinoB : vão nas entradas da ponte H (IN1/IN2 ou IN3/IN4).
// botaoMenos / botaoMais : texto dos botões no celular.
// invertido     : true se o motor gira ao contrário do botão.
//
//  Ponte H 1:  IN1 = 32  IN2 = 33  (Base)    |  IN3 = 25  IN4 = 26  (Ombro)
//  Ponte H 2:  IN1 = 23  IN2 = 22  (Cotovelo)|  IN3 = 19  IN4 = 18  (Garra)
//  (Ponte H 1 usa pinos do lado esquerdo da placa, a Ponte H 2 do lado direito.)
struct Motor {
  const char* nome;
  const char* botaoMenos;
  const char* botaoMais;
  uint8_t     pinoA;
  uint8_t     pinoB;
  bool        invertido;
};

#define NUM_MOTORES 4

static const Motor MOTORES[NUM_MOTORES] = {
  // nome        botão −      botão +     pinoA pinoB invertido
  { "Base",     "Esquerda",  "Direita",    32,   33,   false },
  { "Ombro",    "Desce",     "Sobe",       25,   26,   false },
  { "Cotovelo", "Recolhe",   "Estica",     23,   22,   false },
  { "Garra",    "Abre",      "Fecha",      19,   18,   false },
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

// ============================================================
//  PÁGINA DO CELULAR
// ============================================================
// Página web servida pelo ESP32 (abre em qualquer navegador de celular).
// Motores DC: SEGURE o botão para mover, SOLTE para parar.

const char PAGINA_HTML[] PROGMEM = R"rawliteral(
<!doctype html>
<html lang="pt-BR">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
<meta name="theme-color" content="#101613">
<title>Garra Hidráulica</title>
<style>
:root{color-scheme:dark;--bg:#101613;--panel:#1a231e;--line:#34443a;--text:#f1f6ef;--muted:#adbdb1;--lime:#d5fa86;--red:#ffb2a7}
*{box-sizing:border-box}
body{margin:0;background:radial-gradient(ellipse at top,#253729 0,transparent 490px),var(--bg);color:var(--text);font:16px system-ui,sans-serif}
main{width:min(100% - 32px,920px);margin:0 auto;padding:clamp(20px,5vw,56px) 0 max(28px,env(safe-area-inset-bottom))}
header{display:flex;align-items:flex-start;justify-content:space-between;gap:12px;flex-wrap:wrap}
h1{margin:0;font-size:clamp(35px,7vw,66px);font-weight:750;line-height:1;letter-spacing:-.065em}
header p,.hint{color:var(--muted);margin:12px 0 0}
.connection,.tag{display:inline-flex;align-items:center;gap:7px;padding:6px 10px;border-radius:99px;font-size:.78rem;border:1px solid var(--line);background:#121a15;color:var(--muted)}
.connection::before{content:"";height:7px;width:7px;border-radius:50%;background:currentColor}
.connection[data-live="true"],.tag[data-state="move"]{color:var(--lime)}
.tag[data-state="limit"]{color:#ffd48e}
.panel{border:1px solid var(--line);border-radius:18px;background:var(--panel)}
.speed{margin:24px 0 12px;padding:14px 18px;display:grid;grid-template-columns:auto 1fr auto;gap:14px;align-items:center}
input{width:100%;min-width:0;min-height:34px;accent-color:var(--lime);margin:0}
output{min-width:4ch;text-align:right;color:var(--lime);font-variant-numeric:tabular-nums}
#grid{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:12px}
.motor{padding:14px}
.motor header{align-items:center;margin-bottom:12px}
h2{margin:0;font-size:1.1rem}
.actions{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:8px}
button{cursor:pointer;font:inherit;font-weight:700;color:var(--text);border:1px solid #465c4c;background:#29382e;border-radius:12px;min-height:58px}
button:disabled{opacity:.48;cursor:default}
button:focus-visible,input:focus-visible{outline:2px solid var(--lime);outline-offset:3px}
.hold{touch-action:none;user-select:none;-webkit-touch-callout:none}
.hold.active{color:#16210f;background:var(--lime);border-color:var(--lime)}
.arrow{display:block;color:var(--lime);font-size:1.25rem;line-height:1.1}
.active .arrow{color:inherit}
.stopbar{position:sticky;bottom:0;padding:10px 0 max(8px,env(safe-area-inset-bottom));background:var(--bg);margin-top:6px}
#stop{width:100%;color:#331714;background:var(--red);border-color:var(--red)}
#stop:active{filter:brightness(.85)}
.hint{font-size:.85rem;margin:10px 0 0}
@media(max-width:540px){#grid{grid-template-columns:1fr}.motor{padding:11px}.motor header{margin-bottom:8px}.hold{min-height:52px}.speed{margin-top:16px}}
</style>
</head>
<body>
<main>
<header>
<div><h1>Garra Hidráulica</h1><p>Painel de controle dos motores</p></div>
<span id="connection" class="connection" role="status">Conectando…</span>
</header>
<div class="speed panel">
<label for="speed">Velocidade</label>
<input id="speed" type="range" min="30" max="100" value="70" disabled>
<output id="percent" for="speed">70%</output>
</div>
<div id="grid"></div>
<p id="hint" class="hint">Segure para mover. Solte para parar.</p>
<div class="stopbar"><button id="stop" type="button">■ &nbsp; Parar tudo</button></div>
</main>
<script>
const grid = document.getElementById('grid');
const connection = document.getElementById('connection');
const speed = document.getElementById('speed');
const percent = document.getElementById('percent');
const preview = location.hostname !== '192.168.4.1';
const defaults = [
  ['Base', 'Esquerda', 'Direita', '←', '→'],
  ['Ombro', 'Desce', 'Sobe', '↓', '↑'],
  ['Cotovelo', 'Recolhe', 'Estica', '↶', '↷'],
  ['Garra', 'Abre', 'Fecha', '↔', '→←']
];
const simulated = {v: 70, j: defaults.map(([n, m, p]) => ({n, m, p, d: 0, t: false}))};
const simulatedStarts = new Map();
const held = new Map();
const released = new Set();
let online = preview;
let stopping = false;
let pendingSpeed = null;
let busy = false;
let dirty = false;
let nextPing = 0;

const cards = defaults.map(([name, minus, plus, minusArrow, plusArrow], index) => {
  const card = document.createElement('section');
  card.className = 'motor panel';
  card.innerHTML = '<header><h2></h2><span class="tag"></span></header><div class="actions"></div>';
  const title = card.querySelector('h2');
  title.textContent = name;
  const tag = card.querySelector('.tag');
  tag.textContent = 'Parado';
  const buttons = [[-1, minus, minusArrow], [1, plus, plusArrow]].map(([direction, label, arrow]) => {
    const button = document.createElement('button');
    button.type = 'button';
    button.className = 'hold';
    button.innerHTML = '<span class="arrow" aria-hidden="true"></span><span class="label"></span>';
    button.querySelector('.arrow').textContent = arrow;
    button.querySelector('.label').textContent = label;
    button.disabled = !online;
    button.addEventListener('pointerdown', event => {
      if (event.button !== 0) return;
      event.preventDefault();
      button.setPointerCapture(event.pointerId);
      start('pointer' + event.pointerId);
    });
    for (const type of ['pointerup', 'pointercancel', 'lostpointercapture']) {
      button.addEventListener(type, event => end('pointer' + event.pointerId));
    }
    button.addEventListener('contextmenu', event => event.preventDefault());
    button.addEventListener('keydown', event => {
      if (![' ', 'Enter'].includes(event.key)) return;
      event.preventDefault();
      if (!event.repeat) start('keyboard');
    });
    button.addEventListener('keyup', event => {
      if (![' ', 'Enter'].includes(event.key)) return;
      event.preventDefault();
      end('keyboard');
    });
    button.addEventListener('blur', () => end('keyboard'));
    function start(source) {
      if (!online || stopping || held.has(index)) return;
      held.set(index, {direction, source, button});
      button.classList.add('active');
      send();
    }
    function end(source) {
      const press = held.get(index);
      if (press?.button !== button || press.source !== source) return;
      held.delete(index);
      button.classList.remove('active');
      released.add(index);
      send();
    }
    card.querySelector('.actions').append(button);
    return button;
  });
  grid.append(card);
  return {title, tag, buttons};
});

function render(state) {
  cards.forEach(({title, tag, buttons}, index) => {
    const motor = state?.j[index];
    buttons.forEach(button => { button.disabled = !online || !motor; });
    tag.dataset.state = motor?.t ? 'limit' : motor?.d ? 'move' : '';
    tag.textContent = !motor ? '—' : motor.t ? 'Solte o botão' : motor.d ? (motor.d > 0 ? motor.p : motor.m) : 'Parado';
    if (!motor) return;
    title.textContent = motor.n;
    buttons[0].querySelector('.label').textContent = motor.m;
    buttons[1].querySelector('.label').textContent = motor.p;
  });
  speed.disabled = !online;
  if (state && pendingSpeed === null) speed.value = state.v;
  percent.value = speed.value + '%';
  connection.dataset.live = String(online);
  connection.textContent = preview ? 'Modo prévia' : online ? 'ESP32 conectado' : 'Sem conexão';
}

function clearHeld() {
  for (const press of held.values()) press.button.classList.remove('active');
  held.clear();
}

function stopAll() {
  for (const index of held.keys()) released.add(index);
  clearHeld();
  stopping = true;
  send();
}

function simulate(commands) {
  for (const command of commands) {
    const [type, a, b] = command.split(',');
    if (type === 'x') {
      simulated.j.forEach(motor => { motor.d = 0; });
      simulatedStarts.clear();
    }
    if (type === 'v') simulated.v = Number(a);
    if (type !== 'm') continue;
    const motor = simulated.j[a];
    if (Number(b) === 0) {
      simulatedStarts.delete(a);
      motor.d = 0;
      motor.t = false;
    } else if (!motor.t) {
      if (!simulatedStarts.has(a)) simulatedStarts.set(a, Date.now());
      motor.t = Date.now() - simulatedStarts.get(a) >= 6000;
      motor.d = motor.t ? 0 : Number(b);
    }
  }
  return simulated;
}

async function send() {
  if (busy) { dirty = true; return; }
  busy = true;
  const commands = [];
  if (stopping) commands.push('x');
  for (const index of released) commands.push('m,' + index + ',0');
  released.clear();
  if (pendingSpeed !== null) commands.push('v,' + pendingSpeed);
  pendingSpeed = null;
  for (const [index, press] of held) commands.push('m,' + index + ',' + press.direction);
  if (!commands.length) commands.push('p');
  const controller = new AbortController();
  const timeout = setTimeout(() => controller.abort(), 750);
  try {
    let state;
    if (preview) state = simulate(commands);
    else {
      const response = await fetch('/cmd?c=' + encodeURIComponent(commands.join(';')), {
        signal: controller.signal, cache: 'no-store', keepalive: commands[0] === 'x'
      });
      if (!response.ok) throw new Error('Falha na comunicação');
      state = await response.json();
      if (!Array.isArray(state.j) || !Number.isFinite(state.v)) throw new Error('Resposta inválida');
    }
    online = true;
    if (commands[0] === 'x') stopping = false;
    render(state);
  } catch {
    stopping = false;
    clearHeld();
    online = false;
    render(null);
  } finally {
    clearTimeout(timeout);
    busy = false;
    nextPing = Date.now() + 900;
    if (dirty) { dirty = false; send(); }
  }
}

speed.addEventListener('input', () => {
  pendingSpeed = Number(speed.value);
  percent.value = speed.value + '%';
  send();
});
document.getElementById('stop').addEventListener('click', stopAll);
window.addEventListener('blur', () => { if (held.size) stopAll(); });
document.addEventListener('visibilitychange', () => {
  if (document.hidden && held.size) stopAll();
});
window.addEventListener('pagehide', () => { if (held.size) stopAll(); });
setInterval(() => {
  if (!busy && (held.size || (!document.hidden && Date.now() >= nextPing))) send();
}, 130);
if (preview) document.getElementById('hint').textContent += ' Nesta prévia, os movimentos são simulados.';
send();
</script>
</body>
</html>
)rawliteral";

// Ícone e manifesto: permitem "Adicionar à tela inicial" com cara de app
const char ICONE_SVG[] PROGMEM = R"rawliteral(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 512 512"><rect width="512" height="512" rx="112" fill="#ffb020"/><g fill="none" stroke="#1a1200" stroke-width="40" stroke-linecap="round" stroke-linejoin="round"><path d="M110 410h170M195 410v-100l105-125M300 185l80-80M380 105l60 20M380 105l-20 60"/></g><circle cx="195" cy="310" r="30" fill="#1a1200"/></svg>)rawliteral";

const char MANIFESTO_JSON[] PROGMEM = R"rawliteral({"name":"Garra Hidráulica","short_name":"Garra","start_url":"/","display":"standalone","orientation":"any","background_color":"#0d1117","theme_color":"#0d1117","icons":[{"src":"/icon.svg","sizes":"any","type":"image/svg+xml","purpose":"any"}]})rawliteral";

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
  WiFi.softAP(WIFI_SSID, WIFI_SENHA);
  Serial.printf("Rede: %s  |  Senha: %s\n", WIFI_SSID, WIFI_SENHA);
  Serial.print("Abra no celular: http://");
  Serial.println(WiFi.softAPIP());

  server.on("/", []() { server.send(200, "text/html", PAGINA_HTML); });
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