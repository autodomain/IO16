#include "web_server.h"
#include "lm75_sensor.h"
#include "node_diagnostics.h"
#include "ota_flasher.h"
#include <PubSubClient.h>
#include <avr/wdt.h>

extern PubSubClient client;
extern void pjon_send_test(uint8_t addr, uint8_t led, uint8_t stat);

EthernetServer httpServer(80);

unsigned long pjon_packets_tx = 0;
unsigned long pjon_packets_rx = 0;
unsigned long pjon_packets_err = 0;
extern unsigned long dbg_err_lost, dbg_err_full, dbg_err_other, dbg_send_fail,
    dbg_btn_rx, dbg_k_rx;
extern unsigned long dbg_rx_b1, dbg_rx_b2, dbg_loop_max_us, dbg_loop_last_us;
extern void pjon_service();
extern void pjon_trace_run(uint8_t a, uint8_t l, uint8_t st);
extern uint8_t tr_n;
extern unsigned long tr_t[];
extern uint8_t tr_d[];
extern uint8_t tr_b[];
extern void pjon_echo_test(uint8_t bus_n, uint8_t *pre, uint8_t *echo);
template <class T> static inline void wp(EthernetClient &c, T v) {
  c.print(v);
  pjon_service();
}
template <class T> static inline void wp(EthernetClient &c, T v, int d) {
  c.print(v, d);
  pjon_service();
}
template <class T> static inline void wp_ln(EthernetClient &c, T v) {
  c.println(v);
  pjon_service();
}
extern unsigned long dbg_sec_max[6];
extern uint8_t dbg_last_err_code;
extern uint16_t dbg_last_err_data;
static int dbg_free_ram() {
  extern int __heap_start, *__brkval;
  int v;
  return (int)&v - (__brkval == 0 ? (int)&__heap_start : (int)__brkval);
}
unsigned long mqtt_messages_tx = 0;
unsigned long mqtt_messages_rx = 0;

char last_mqtt_rx_topic[48] = "-";
char last_mqtt_rx_payload[32] = "-";
unsigned long last_mqtt_rx_time = 0;
char last_mqtt_tx_topic[48] = "-";
char last_mqtt_tx_payload[32] = "-";

MqttLogItem mqtt_rx_history[MQTT_LOG_DEPTH];
uint8_t mqtt_rx_history_count = 0;
MqttLogItem mqtt_tx_history[MQTT_LOG_DEPTH];
uint8_t mqtt_tx_history_count = 0;

void web_server_log_rx(const char *topic, const uint8_t *payload, unsigned int length) {
  last_mqtt_rx_time = millis();
  strncpy(last_mqtt_rx_topic, topic, sizeof(last_mqtt_rx_topic) - 1);
  last_mqtt_rx_topic[sizeof(last_mqtt_rx_topic) - 1] = '\0';
  uint16_t plen = (length < sizeof(last_mqtt_rx_payload) - 1)
                      ? length
                      : (sizeof(last_mqtt_rx_payload) - 1);
  memcpy(last_mqtt_rx_payload, payload, plen);
  last_mqtt_rx_payload[plen] = '\0';

  for (int i = MQTT_LOG_DEPTH - 1; i > 0; i--) {
    mqtt_rx_history[i] = mqtt_rx_history[i - 1];
  }
  strncpy(mqtt_rx_history[0].topic, last_mqtt_rx_topic, sizeof(mqtt_rx_history[0].topic) - 1);
  mqtt_rx_history[0].topic[sizeof(mqtt_rx_history[0].topic) - 1] = '\0';
  strncpy(mqtt_rx_history[0].payload, last_mqtt_rx_payload, sizeof(mqtt_rx_history[0].payload) - 1);
  mqtt_rx_history[0].payload[sizeof(mqtt_rx_history[0].payload) - 1] = '\0';
  if (mqtt_rx_history_count < MQTT_LOG_DEPTH)
    mqtt_rx_history_count++;
}

void web_server_log_tx(const char *topic, const char *payload) {
  strncpy(last_mqtt_tx_topic, topic, sizeof(last_mqtt_tx_topic) - 1);
  last_mqtt_tx_topic[sizeof(last_mqtt_tx_topic) - 1] = '\0';
  strncpy(last_mqtt_tx_payload, payload, sizeof(last_mqtt_tx_payload) - 1);
  last_mqtt_tx_payload[sizeof(last_mqtt_tx_payload) - 1] = '\0';

  for (int i = MQTT_LOG_DEPTH - 1; i > 0; i--) {
    mqtt_tx_history[i] = mqtt_tx_history[i - 1];
  }
  strncpy(mqtt_tx_history[0].topic, last_mqtt_tx_topic, sizeof(mqtt_tx_history[0].topic) - 1);
  mqtt_tx_history[0].topic[sizeof(mqtt_tx_history[0].topic) - 1] = '\0';
  strncpy(mqtt_tx_history[0].payload, last_mqtt_tx_payload, sizeof(mqtt_tx_history[0].payload) - 1);
  mqtt_tx_history[0].payload[sizeof(mqtt_tx_history[0].payload) - 1] = '\0';
  if (mqtt_tx_history_count < MQTT_LOG_DEPTH)
    mqtt_tx_history_count++;
}

void web_server_init() { httpServer.begin(); }

static void parse_ip_bytes(const char *str, uint8_t *out) {
  int v[4] = {0, 0, 0, 0};
  sscanf(str, "%d.%d.%d.%d", &v[0], &v[1], &v[2], &v[3]);
  for (int i = 0; i < 4; i++) {
    out[i] = (uint8_t)v[i];
  }
}

static void url_decode(char *src) {
  char *dst = src;
  while (*src) {
    if (*src == '+') {
      *dst++ = ' ';
      src++;
    } else if (*src == '%' && src[1] && src[2]) {
      char hex[3] = {src[1], src[2], '\0'};
      *dst++ = (char)strtol(hex, NULL, 16);
      src += 3;
    } else {
      *dst++ = *src++;
    }
  }
  *dst = '\0';
}

static void get_form_value(const char *body, const char *key, char *out,
                           size_t max_len) {
  out[0] = '\0';
  size_t klen = strlen(key);
  const char *p = body;
  while (p && *p) {
    if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
      const char *val_start = p + klen + 1;
      const char *val_end = strchr(val_start, '&');
      size_t vlen = val_end ? (size_t)(val_end - val_start) : strlen(val_start);
      if (vlen >= max_len)
        vlen = max_len - 1;
      strncpy(out, val_start, vlen);
      out[vlen] = '\0';
      url_decode(out);
      return;
    }
    p = strchr(p, '&');
    if (p)
      p++;
  }
}

static void send_header(EthernetClient &cl, int code = 200) {
  if (code == 200)
    wp_ln(cl, F("HTTP/1.1 200 OK"));
  else
    wp_ln(cl, F("HTTP/1.1 404 Not Found"));
  wp_ln(cl, F("Content-Type: text/html; charset=utf-8"));
  wp_ln(cl, F("Connection: close"));
  cl.println();
}

static void send_redirect(EthernetClient &cl, const char *url = "/") {
  wp_ln(cl, F("HTTP/1.1 303 See Other"));
  wp(cl, F("Location: "));
  wp_ln(cl, url);
  wp_ln(cl, F("Content-Type: text/html; charset=utf-8"));
  wp_ln(cl, F("Connection: close"));
  cl.println();
  wp(cl, F("<!DOCTYPE html><html><head><meta http-equiv='refresh' "
           "content='0;url="));
  wp(cl, url);
  wp_ln(cl, F("'></head><body>Reindirizzamento...</body></html>"));
  cl.flush();
  delay(40);
}

static void send_css(EthernetClient &cl) {
  wp_ln(cl, F("<style>"));
  wp_ln(cl, F("body{font-family:sans-serif;margin:0;padding:20px;background:#"
              "0f172a;color:#f8fafc}"));
  wp_ln(cl,
        F(".wrap{max-width:680px;margin:auto;background:#1e293b;padding:24px;"
          "border-radius:10px;box-shadow:0 4px 14px rgba(0,0,0,0.4)}"));
  wp_ln(cl, F("h1{color:#38bdf8;font-size:22px;margin-top:0;display:flex;align-"
              "items:center;gap:10px}"));
  wp_ln(cl, F(".card{background:#334155;padding:16px;border-radius:8px;margin-"
              "bottom:18px}"));
  wp_ln(cl, F(".card "
              "h3{margin-top:0;color:#94a3b8;font-size:15px;text-transform:"
              "uppercase;letter-spacing:1px}"));
  wp_ln(cl, F("details.card{padding:14px 16px}"));
  wp_ln(cl, F("details.card summary{cursor:pointer;color:#38bdf8;font-size:14px;"
              "font-weight:bold;text-transform:uppercase;letter-spacing:1px;outline:none;"
              "user-select:none;list-style-position:inside}"));
  wp_ln(cl, F("details.card summary:hover{color:#7dd3fc}"));
  wp_ln(cl, F("details.card[open] summary{margin-bottom:14px}"));
  wp_ln(cl,
        F(".stat-grid{display:grid;grid-template-columns:1fr 1fr;gap:10px}"));
  wp_ln(cl, F(".stat-box{background:#1e293b;padding:12px;border-radius:6px;"
              "border-left:3px solid #38bdf8}"));
  wp_ln(cl, F(".stat-val{font-size:18px;font-weight:bold;color:#f1f5f9;margin-"
              "top:4px}"));
  wp_ln(cl,
        F("label{display:block;margin-top:10px;color:#cbd5e1;font-size:13px}"));
  wp_ln(
      cl,
      F("input[type=text],input[type=number],input[type=password]{width:100%;"
        "box-sizing:border-box;padding:9px;background:#0f172a;border:1px solid "
        "#475569;border-radius:5px;color:#fff;font-size:14px;margin-top:4px}"));
  wp_ln(cl, F("input[type=checkbox]{margin-right:8px}"));
  wp_ln(
      cl,
      F(".btn{display:inline-block;padding:10px "
        "18px;background:#0284c7;color:#fff;border:none;border-radius:6px;font-"
        "weight:bold;cursor:pointer;margin-top:14px;text-decoration:none}"));
  wp_ln(cl, F(".btn-red{background:#dc2626}.btn-green{background:#16a34a}"));
  wp_ln(cl, F(".btn:hover{opacity:0.9}"));
  wp_ln(cl, F(".badge{display:inline-block;padding:3px "
              "8px;border-radius:12px;font-size:12px;font-weight:bold}"));
  wp_ln(cl, F(".badge-ok{background:#16a34a;color:#fff}.badge-err{background:#"
              "dc2626;color:#fff}"));
  wp_ln(cl, F("</style>"));
}

static void handle_ota_page(EthernetClient &cl) {
  send_header(cl);
  wp_ln(cl, F("<!DOCTYPE html><html><head><meta "
              "charset='utf-8'><title>Aggiornamento OTA RS485</title>"));
  send_css(cl);
  wp_ln(cl, F("<style>"));
  wp_ln(cl, F(".form-group{margin-bottom:16px;}label{display:block;margin-"
              "bottom:6px;color:#cbd5e1;font-weight:500;}"));
  wp_ln(cl, F("input[type=number],select{width:100%;padding:10px;border-radius:"
              "8px;border:1px solid "
              "#334155;background:#0f172a;color:#fff;box-sizing:border-box;}"));
  wp_ln(cl, F("input[type=file]{width:100%;padding:12px;border:1px dashed "
              "#475569;border-radius:8px;background:#0f172a;color:#cbd5e1;box-"
              "sizing:border-box;}"));
  wp_ln(cl, F(".progress-bg{background:#334155;border-radius:8px;overflow:"
              "hidden;height:24px;margin-top:15px;}"));
  wp_ln(cl, F(".progress-bar{background:#38bdf8;width:0%;height:100%;text-"
              "align:center;color:#0f172a;font-weight:bold;line-height:24px;"
              "transition:width 0.15s ease;}"));
  wp_ln(cl,
        F(".log-box{margin-top:12px;padding:12px;background:#0f172a;border-"
          "radius:8px;font-family:monospace;font-size:12px;max-height:180px;"
          "overflow-y:auto;color:#38bdf8;border:1px solid #334155;}"));
  wp_ln(cl, F("</style></head><body><div class='wrap'>"));
  wp_ln(cl, F("<h1><span>Aggiornamento OTA RS485</span><a href='/' class='btn' "
              "style='float:right;font-size:13px;padding:6px 14px;'>Torna al "
              "Pannello</a></h1>"));
  wp_ln(cl, F("<div class='card'><h3>Programmazione Remota Nodi IO16</h3>"));
  wp_ln(
      cl,
      F("<p style='color:#94a3b8;font-size:14px;margin-bottom:20px;'>Permette "
        "di aggiornare il firmware delle tastiere collegate sul bus RS485 "
        "senza programmatori fisici, sia singolarmente sia in blocco su tutta "
        "la rete.</p>"));
  wp_ln(cl, F("<div class='form-group'><label>Modalit&agrave; di "
              "Aggiornamento:</label><select id='modeSelect' "
              "onchange='toggleMode()'><option value='all' selected>Tutti i "
              "Nodi Online (Batch Mass OTA)</option><option "
              "value='single'>Singolo Nodo Specifico</option></select></div>"));
  wp_ln(cl, F("<div id='singleBox' style='display:none;'><div "
              "class='form-group'><label>ID Nodo (1..63):</label><input "
              "type='number' id='nodeId' min='1' max='63' value='1'></div><div "
              "class='form-group'><label>Linea Bus RS485:</label><select "
              "id='busId'><option value='1'>Bus 1 (U2 / UART0)</option><option "
              "value='2'>Bus 2 (U3 / UART1)</option></select></div></div>"));
  wp_ln(
      cl,
      F("<div id='multiBox'><div class='form-group'><label>Nodi Rilevati "
        "Online sul Bus (seleziona chi aggiornare):</label><div id='nodesList' "
        "style='background:#0f172a;padding:10px;border-radius:8px;border:1px "
        "solid #334155;font-size:14px;'>Ricerca nodi online in "
        "corso...</div></div></div>"));
  wp_ln(
      cl,
      F("<div class='form-group'><label>File Firmware (.hex / .pkg):</label><input type='file' id='hexFile' accept='.hex,.pkg,.fw'></div>"));
  wp_ln(cl, F("<button id='btnStart' class='btn btn-green' "
              "style='width:100%;padding:12px;' onclick='startOTA()'>Avvia "
              "Aggiornamento Firmware</button>"));
  wp_ln(cl, F("<div id='progArea' style='display:none;'><div "
              "class='progress-bg'><div id='bar' "
              "class='progress-bar'>0%</div></div><div id='log' "
              "class='log-box'>Pronto.</div></div>"));
  wp_ln(cl, F("</div></div>"));
  wp_ln(cl, F("<script>"));
  wp_ln(cl, F("let parsedFirmwares=null;let onlineNodes=[];"));
  wp_ln(cl, F("function toggleMode(){const "
              "m=document.getElementById('modeSelect').value;document."
              "getElementById('singleBox').style.display=(m==='single')?'block'"
              ":'none';document.getElementById('multiBox').style.display=(m==='"
              "all')?'block':'none';}"));
  wp_ln(cl,
        F("async function loadNodes(){try{const r=await "
          "fetch('/api/status');const d=await "
          "r.json();onlineNodes=(d.nds||[]).filter(n=>n.o===1||n.s===1);const "
          "box=document.getElementById('nodesList');if(onlineNodes.length===0){"
          "box.innerHTML='<span style=\"color:#f59e0b;\">Nessun nodo attivo "
          "rilevato al momento. Puoi usare la modalit&agrave; Singolo "
          "Nodo.</span>';}else{let h='';onlineNodes.forEach(n=>{h+='<label "
          "style=\"display:inline-block;margin-right:15px;margin-bottom:6px;"
          "cursor:pointer;\"><input type=\"checkbox\" class=\"node-cb\" "
          "value=\"'+n.i+'\" data-bus=\"'+(n.b||1)+'\" checked> <b>Nodo "
          "'+n.i+'</b> (Bus "
          "'+(n.b||1)+')</"
          "label>';});box.innerHTML=h;}}catch(e){document.getElementById('"
          "nodesList').innerText='Errore: '+e.message;}}"));
  wp_ln(cl, F("loadNodes();"));
  wp_ln(
      cl,
      F("document.getElementById('hexFile').addEventListener('change',function("
        "e){const f=e.target.files[0];if(!f)return;const r=new "
        "FileReader();r.onload=function(evt){try{parsedFirmwares=parseFirmwareFile("
        "evt.target.result);let keys=Object.keys(parsedFirmwares);if(keys.length>1||(keys.length===1&&keys[0]!=='default')){let msg='File Universale caricato con successo! Supporta: ';keys.forEach(function(k){msg+=k+' ('+parsedFirmwares[k].length+' byte) ';});logMsg(msg);}else{logMsg('File caricato: '+parsedFirmwares.default.length+' byte.');}}catch(err){logMsg('Errore HEX: "
        "'+err.message);}};r.readAsText(f);});"));
  wp_ln(cl, F("function logMsg(m){const "
              "el=document.getElementById('log');el.innerText+='\\n'+m;el."
              "scrollTop=el.scrollHeight;}"));
  wp_ln(cl,
        F("function parseIntelHex(text){const lines=text.split('\\n');const "
          "bin=new Uint8Array(131072);let max=0,ext=0;for(let raw of "
          "lines){let l=raw.trim();if(!l.startsWith(':'))continue;let "
          "len=parseInt(l.substr(1,2),16);let "
          "a=parseInt(l.substr(3,4),16)+ext;let "
          "type=parseInt(l.substr(7,2),16);if(type===0){for(let "
          "i=0;i<len;i++)bin[a+i]=parseInt(l.substr(9+i*2,2),16);if(a+len>max)"
          "max=a+len;}else "
          "if(type===4){ext=parseInt(l.substr(9,4),16)<<16;}}return "
          "{data:bin,length:max};}"));
  wp_ln(cl, F("function parseFirmwareFile(text){text=text.trim();if(text.startsWith('{')){try{const pkg=JSON.parse(text);const res={};for(let k in pkg.firmwares)res[k.toLowerCase()]=parseIntelHex(pkg.firmwares[k]);return res;}catch(e){}}if(text.indexOf(';=== TARGET:')!==-1){const res={};const parts=text.split(';=== TARGET:');for(let p of parts){if(!p.trim())continue;const endIdx=p.indexOf('\\n');const mcu=p.substring(0,endIdx).replace(/===/g,'').trim().toLowerCase();res[mcu]=parseIntelHex(p.substring(endIdx+1));}return res;}return {default:parseIntelHex(text)};}"));
  wp_ln(cl, F("async function flashNode(nid, bid, stepPrefix){"));
  wp_ln(cl, F("logMsg(stepPrefix+' Trigger bootloader Nodo '+nid+' (Bus "
              "'+bid+')...');"));
  wp_ln(
      cl,
      F("const sRes=await fetch('/api/ota_start?node='+nid+'&bus='+bid);const "
        "sJ=await sRes.json();"));
  wp_ln(cl, F("if(sJ.status!=='ok')throw new Error(sJ.msg);"));
  wp_ln(cl, F("const mcu=(sJ.mcu||'unknown').toLowerCase();const fw=parsedFirmwares[mcu]||parsedFirmwares['default'];if(!fw||fw.length===0)throw new Error('Nessun firmware nel file per MCU: '+mcu);"));
  wp_ln(cl, F("logMsg(stepPrefix+' Target MCU: '+mcu+' ('+fw.length+' byte). Flash in corso...');"));
  wp_ln(cl, F("const pageSize=(mcu==='atmega324pb')?128:256;const tot=fw.length;"));
  wp_ln(cl, F("for(let off=0;off<tot;off+=pageSize){"));
  wp_ln(cl, F("const chunk=fw.data.slice(off,off+pageSize);const "
              "wordAddr=off>>1;"));
  wp_ln(cl, F("let ok=false;let lastErr='';for(let r=0;r<4;r++){try{const pRes=await fetch('/api/ota_page?addr='+wordAddr,{method:'POST',body:chunk});const pJ=await pRes.json();if(pJ.status==='ok'){ok=true;break;}else{lastErr=pJ.msg||'errore';}}catch(e){lastErr=e.message;}await new Promise(r=>setTimeout(r,25));}"));
  wp_ln(cl, F("if(!ok)throw new Error('Errore scrittura word 0x'+wordAddr.toString(16)+' ('+lastErr+')');"));
  wp_ln(cl, F("const pPct=Math.round(((off+pageSize)/tot)*100);const bar=document.getElementById('bar');if(bar){bar.style.width=Math.min(100,pPct)+'%';bar.innerText=stepPrefix+' '+Math.min(100,pPct)+'%';}"));
  wp_ln(cl, F("}"));
  wp_ln(cl, F("logMsg(stepPrefix+' Finalizzazione e riavvio Nodo "
              "'+nid+'...');await fetch('/api/ota_end');logMsg(stepPrefix+' "
              "OK: Nodo '+nid+' ('+mcu+') aggiornato con successo!');}"));
  wp_ln(cl, F("async function startOTA(){"));
  wp_ln(cl, F("if(!parsedFirmwares){alert('Seleziona "
              "prima un file firmware valido!');return;}"));
  wp_ln(cl, F("const mode=document.getElementById('modeSelect').value;let "
              "targets=[];"));
  wp_ln(cl, F("if(mode==='single'){targets.push({id:parseInt(document."
              "getElementById('nodeId').value),bus:parseInt(document."
              "getElementById('busId').value)});}else{const "
              "cbs=document.querySelectorAll('.node-cb:checked');cbs.forEach("
              "cb=>{targets.push({id:parseInt(cb.value),bus:parseInt(cb."
              "getAttribute('data-bus')||1)});});if(targets.length===0){alert('"
              "Nessun nodo selezionato dalla lista!');return;}}"));
  wp_ln(cl, F("const btn=document.getElementById('btnStart');const "
              "prog=document.getElementById('progArea');const "
              "bar=document.getElementById('bar');"));
  wp_ln(cl, F("btn.disabled=true;prog.style.display='block';bar.style.width='0%"
              "';bar.innerText='0%';bar.style.background='#38bdf8';"));
  wp_ln(cl, F("document.getElementById('log').innerText='=== Inizio Sessione "
              "Aggiornamento ('+targets.length+' nodi) ===';"));
  wp_ln(cl, F("let successCount=0;"));
  wp_ln(cl, F("for(let i=0;i<targets.length;i++){const t=targets[i];const "
              "prefix='['+(i+1)+'/'+targets.length+']';try{await "
              "flashNode(t.id,t.bus,prefix);successCount++;}catch(err){logMsg("
              "prefix+' ERRORE Nodo '+t.id+': '+err.message);await "
              "fetch('/api/ota_abort');}const "
              "pct=Math.round(((i+1)/"
              "targets.length)*100);bar.style.width=pct+'%';bar.innerText=pct+'"
              "%';if(i<targets.length-1){logMsg('Attesa stabilizzazione bus "
              "(3s)...');await new Promise(r=>setTimeout(r,3000));}}"));
  wp_ln(cl, F("if(successCount===targets.length){bar.style.background='#10b981'"
              ";bar.innerText='100% COMPLETATO';logMsg('=== COMPLETATO: Tutti "
              "i '+targets.length+' nodi sono stati aggiornati! "
              "===');}else{bar.style.background='#f59e0b';bar.innerText="
              "successCount+'/'+targets.length+' AGGIORNATI';logMsg('=== "
              "TERMINATO: '+successCount+' su '+targets.length+' nodi "
              "aggiornati con successo ===');}"));
  wp_ln(cl, F("btn.disabled=false;}"));
  wp_ln(cl, F("</script></body></html>"));
  cl.flush();
  delay(40);
  cl.stop();
}

void web_server_handle() {
  EthernetClient cl = httpServer.available();
  if (!cl)
    return;

  char line[128];
  int line_idx = 0;
  char method[8] = "";
  char path[32] = "";
  int content_length = 0;
  bool is_first_line = true;

  unsigned long start_time = millis();
  while (cl.connected() && (millis() - start_time < 2000)) {
    if (!cl.available()) {
      pjon_service();
      continue;
    }
    char c = cl.read();

    if (c != '\r' && c != '\n') {
      if (line_idx < (int)sizeof(line) - 1) {
        line[line_idx++] = c;
      }
    } else if (c == '\n') {
      line[line_idx] = '\0';
      if (is_first_line) {
        sscanf(line, "%7s %31s", method, path);
        is_first_line = false;
      } else {
        if (strncasecmp(line, "Content-Length:", 15) == 0) {
          content_length = atoi(line + 15);
        }
      }

      if (line_idx == 0) {
        // Fine headers HTTP!
        break;
      }
      line_idx = 0;
    }
  }

  // Gestione eventuale body POST
  char body[280];
  body[0] = '\0';
  if (strcmp(method, "POST") == 0 && strncmp(path, "/api/ota_page", 13) != 0) {
    int body_idx = 0;
    unsigned long b_start = millis();
    while (cl.connected() && body_idx < content_length &&
           (millis() - b_start < 1500)) {
      if (cl.available()) {
        char c = cl.read();
        if (body_idx < (int)sizeof(body) - 1) {
          body[body_idx++] = c;
        }
      }
    }
    body[body_idx] = '\0';

    if (strcmp(path, "/save") == 0) {
      char tmp[40];

      // DHCP
      get_form_value(body, "dhcp", tmp, sizeof(tmp));
      config.dhcp = (strcmp(tmp, "1") == 0) ? 1 : 0;

      // IP Statici
      get_form_value(body, "ip", tmp, sizeof(tmp));
      if (strlen(tmp) > 6)
        parse_ip_bytes(tmp, config.ip);
      get_form_value(body, "gw", tmp, sizeof(tmp));
      if (strlen(tmp) > 6)
        parse_ip_bytes(tmp, config.gateway);
      get_form_value(body, "sub", tmp, sizeof(tmp));
      if (strlen(tmp) > 6)
        parse_ip_bytes(tmp, config.subnet);
      get_form_value(body, "dns", tmp, sizeof(tmp));
      if (strlen(tmp) > 6)
        parse_ip_bytes(tmp, config.dns);

      // MQTT
      get_form_value(body, "msrv", tmp, sizeof(tmp));
      if (strlen(tmp) > 0)
        strncpy(config.mqtt_server, tmp, sizeof(config.mqtt_server) - 1);
      get_form_value(body, "mprt", tmp, sizeof(tmp));
      if (atoi(tmp) > 0)
        config.mqtt_port = (uint16_t)atoi(tmp);
      get_form_value(body, "musr", tmp, sizeof(tmp));
      strncpy(config.mqtt_user, tmp, sizeof(config.mqtt_user) - 1);
      get_form_value(body, "mpwd", tmp, sizeof(tmp));
      strncpy(config.mqtt_pass, tmp, sizeof(config.mqtt_pass) - 1);
      get_form_value(body, "mcid", tmp, sizeof(tmp));
      if (strlen(tmp) > 0)
        strncpy(config.mqtt_client_id, tmp, sizeof(config.mqtt_client_id) - 1);
      get_form_value(body, "gwnm", tmp, sizeof(tmp));
      if (strlen(tmp) > 0)
        strncpy(config.gateway_name, tmp, sizeof(config.gateway_name) - 1);
      get_form_value(body, "mpre", tmp, sizeof(tmp));
      if (strlen(tmp) > 0)
        strncpy(config.mqtt_prefix, tmp, sizeof(config.mqtt_prefix) - 1);

      // Home Assistant
      get_form_value(body, "hadisc", tmp, sizeof(tmp));
      config.ha_discovery = (strcmp(tmp, "1") == 0) ? 1 : 0;
      get_form_value(body, "hand", tmp, sizeof(tmp));
      if (atoi(tmp) > 0)
        config.ha_nodes_count = (uint8_t)atoi(tmp);

      // PJON
      get_form_value(body, "pjid", tmp, sizeof(tmp));
      if (atoi(tmp) > 0)
        config.pjon_id = (uint8_t)atoi(tmp);
      get_form_value(body, "pjbd", tmp, sizeof(tmp));
      if (atol(tmp) > 0)
        config.pjon_baud = (uint32_t)atol(tmp);

      config_save();

      send_header(cl);
      wp_ln(cl, F("<!DOCTYPE html><html><head><meta charset='utf-8'>"));
      send_css(cl);
      wp_ln(
          cl,
          F("</head><body><div class='wrap'><h1>Salvataggio Completato</h1>"));
      wp_ln(
          cl,
          F("<p>Le impostazioni sono state salvate nella memoria EEPROM.</p>"));
      wp_ln(cl, F("<p>Per renderle effettive sul modulo di rete W5500 e sul "
                  "bus RS485, riavvia il Gateway.</p>"));
      wp_ln(
          cl,
          F("<a href='/reboot' class='btn btn-red'>Riavvia Gateway Ora</a> "));
      wp_ln(cl, F("<a href='/' class='btn'>Torna al Pannello</a>"));
      wp_ln(cl, F("</div></body></html>"));
      cl.flush();
      delay(30);
      cl.stop();
      return;
    }
  }

  // Endpoint Ping Nodi (reindirizza istantaneamente alla home o risponde a
  // fetch)
  if (strncmp(path, "/pingall", 8) == 0) {
    node_diag_force_ping_all();
    wp_ln(cl, F("HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nConnection: "
                "close\r\n"));
    wp_ln(cl, F("<!DOCTYPE html><html><head><meta http-equiv='refresh' "
                "content='0;url=/'></head><body><script>location.replace('/"
                "');</script>Reindirizzamento...</body></html>"));
    cl.flush();
    delay(40);
    cl.stop();
    return;
  }

  // Endpoint Test Rapido RS485 (reindirizza istantaneamente alla home o
  // risponde a fetch)
  if (strncmp(path, "/test", 5) == 0) {
    char t_addr[8] = "1", t_led[8] = "0", t_stat[8] = "1";
    char *q = strchr(path, '?');
    if (q) {
      get_form_value(q + 1, "addr", t_addr, sizeof(t_addr));
      get_form_value(q + 1, "led", t_led, sizeof(t_led));
      get_form_value(q + 1, "stat", t_stat, sizeof(t_stat));
    } else if (strcmp(method, "POST") == 0) {
      get_form_value(body, "addr", t_addr, sizeof(t_addr));
      get_form_value(body, "led", t_led, sizeof(t_led));
      get_form_value(body, "stat", t_stat, sizeof(t_stat));
    }
    uint8_t addr = (uint8_t)atoi(t_addr);
    if (addr > 0) {
      pjon_send_test(addr, (uint8_t)atoi(t_led), (uint8_t)atoi(t_stat));
    }
    wp_ln(cl, F("HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nConnection: "
                "close\r\n"));
    wp_ln(cl, F("<!DOCTYPE html><html><head><meta http-equiv='refresh' "
                "content='0;url=/'></head><body><script>location.replace('/"
                "');</script>Reindirizzamento...</body></html>"));
    cl.flush();
    delay(40);
    cl.stop();
    return;
  }

  if (strncmp(path, "/api/trace/", 11) == 0) {
    // /api/trace/<addr>/<led>/<stat>  -> invia il comando e restituisce il
    // traffico RS485 catturato
    int a = atoi(path + 11);
    char *p2 = strchr(path + 11, '/');
    int l = p2 ? atoi(p2 + 1) : 0;
    char *p3 = p2 ? strchr(p2 + 1, '/') : NULL;
    int st = p3 ? atoi(p3 + 1) : 1;
    pjon_trace_run((uint8_t)a, (uint8_t)l, (uint8_t)st);
    cl.print(F("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nConnection: "
               "close\r\n\r\n"));
    char lb[32];
    unsigned long t0 = tr_n ? tr_t[0] : 0;
    for (uint8_t q = 0; q < tr_n; q++) {
      int ln = snprintf_P(lb, sizeof(lb), PSTR("%lu %u %u\n"), tr_t[q] - t0,
                          tr_d[q], tr_b[q]);
      cl.write((const uint8_t *)lb, ln);
      pjon_service();
    }
    cl.flush();
    cl.stop();
    return;
  }

  if (strncmp(path, "/api/echo", 9) == 0) {
    uint8_t pre = 0, echo = 0;
    pjon_echo_test(path[strlen(path) - 1] == '2' ? 2 : 1, &pre, &echo);
    char eb[64];
    int en =
        snprintf_P(eb, sizeof(eb), PSTR("{\"pre\":%u,\"echo\":%u}"), pre, echo);
    cl.print(F("HTTP/1.1 200 OK\r\nContent-Type: "
               "application/json\r\nConnection: close\r\n\r\n"));
    cl.write((const uint8_t *)eb, en);
    cl.flush();
    cl.stop();
    return;
  }

  if (strcmp(path, "/api/scan") == 0) {
    node_diag_start_discovery_scan();
    wp_ln(cl, F("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n{\"status\":\"scanning\"}"));
    cl.flush();
    cl.stop();
    return;
  }

  if (strcmp(path, "/api/clear") == 0) {
    node_diag_clear_all();
    wp_ln(cl, F("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n{\"status\":\"cleared\"}"));
    cl.flush();
    cl.stop();
    return;
  }

  if (strncmp(path, "/api/node_config", 16) == 0) {
    char t_node[16] = "all";
    char t_bl[8] = "12";
    char t_mode[8] = "1";
    char *q = strchr(path, '?');
    if (q) {
      get_form_value(q + 1, "node", t_node, sizeof(t_node));
      get_form_value(q + 1, "backlight", t_bl, sizeof(t_bl));
      get_form_value(q + 1, "mode", t_mode, sizeof(t_mode));
    } else if (strcmp(method, "POST") == 0) {
      get_form_value(body, "node", t_node, sizeof(t_node));
      get_form_value(body, "backlight", t_bl, sizeof(t_bl));
      get_form_value(body, "mode", t_mode, sizeof(t_mode));
    }

    uint8_t bl = (uint8_t)atoi(t_bl);
    uint8_t md = (uint8_t)atoi(t_mode);

    // Gestione configurazione simultanea per TUTTI i nodi
    if (strcmp(t_node, "all") == 0 || strcmp(t_node, "0") == 0) {
      uint8_t count = 0;
      for (uint8_t i = 1; i <= MAX_DIAG_NODES; i++) {
        if (remote_nodes[i].active || remote_nodes[i].online) {
          pjon_send_config_set(i, bl, md);
          remote_nodes[i].backlight_pct = bl;
          remote_nodes[i].led_mode = md;
          remote_nodes[i].has_config = true;
          count++;
          unsigned long t0 = millis();
          while (millis() - t0 < 40) pjon_service();
        }
      }
      pjon_send_config_set(0, bl, md);
      unsigned long tb0 = millis();
      while (millis() - tb0 < 60) pjon_service();

      char resp[96];
      snprintf_P(resp, sizeof(resp), PSTR("{\"status\":\"ok\",\"node\":\"all\",\"count\":%u,\"backlight\":%u,\"mode\":%u}"),
                 count, bl, md);
      cl.print(F("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n"));
      cl.print(resp);
      cl.flush();
      cl.stop();
      return;
    }

    uint8_t n_id = (uint8_t)atoi(t_node);
    if (n_id >= 1 && n_id <= MAX_DIAG_NODES) {
      if (strcmp(method, "POST") == 0 || strstr(path, "backlight=")) {
        pjon_send_config_set(n_id, bl, md);
        unsigned long t0 = millis();
        while (millis() - t0 < 120) {
          pjon_service();
        }
        remote_nodes[n_id].backlight_pct = bl;
        remote_nodes[n_id].led_mode = md;
        remote_nodes[n_id].has_config = true;
      } else {
        pjon_send_config_req(n_id);
        unsigned long t0 = millis();
        while (millis() - t0 < 150) {
          pjon_service();
          if (remote_nodes[n_id].has_config) break;
        }
      }
      char resp[80];
      snprintf_P(resp, sizeof(resp), PSTR("{\"status\":\"ok\",\"node\":%u,\"backlight\":%u,\"mode\":%u}"),
                 n_id, remote_nodes[n_id].backlight_pct, remote_nodes[n_id].led_mode);
      cl.print(F("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n"));
      cl.print(resp);
      cl.flush();
      cl.stop();
      return;
    }
  }

  if (strcmp(path, "/api/status") == 0) {
    // Risposta costruita in un unico buffer e inviata con una sola write
    static char sb[3072];
    int n = 0;
#define APP(...)                                                               \
  do {                                                                         \
    if (n < (int)sizeof(sb) - 1)                                               \
      n += snprintf_P(sb + n, sizeof(sb) - n, __VA_ARGS__);                    \
  } while (0)
    char tbuf[10];
    if (lm75_is_available())
      dtostrf(lm75_get_temp(), 1, 1, tbuf);
    else
      strcpy(tbuf, "null");
    APP(PSTR("{\"mqtt\":%d,\"scan\":%d,\"prx\":%lu,\"ptx\":%lu,\"mrx\":%lu,\"mtx\":%lu,"
             "\"rt\":\"%s\",\"rp\":\"%s\",\"tt\":\"%s\",\"tp\":\"%s\",\"temp\":"
             "%s,\"nds\":["),
        client.connected() ? 1 : 0, node_diag_is_scanning() ? 1 : 0, pjon_packets_rx, pjon_packets_tx,
        mqtt_messages_rx, mqtt_messages_tx, last_mqtt_rx_topic,
        last_mqtt_rx_payload, last_mqtt_tx_topic, last_mqtt_tx_payload, tbuf);
    bool fst = true;
    for (uint8_t i = 1; i <= MAX_DIAG_NODES; i++) {
      if (remote_nodes[i].active && (remote_nodes[i].last_seen > 0 || remote_nodes[i].online)) {
        char nt_str[10];
        if (remote_nodes[i].has_temp && remote_nodes[i].temp > 0.0f)
          dtostrf(remote_nodes[i].temp, 1, 1, nt_str);
        else
          strcpy(nt_str, "null");
        unsigned long cur_now = millis();
        unsigned long sec_ago = (remote_nodes[i].last_seen > 0) ? (cur_now - remote_nodes[i].last_seen) / 1000 : 999999;
        APP(PSTR("%s{\"i\":%u,\"o\":%d,\"b\":%u,\"r\":%lu,\"s\":%d,\"t\":%s,\"q\":%u,\"a\":%u,\"f\":%u,\"l\":%lu,\"bl\":%u,\"lm\":%u,\"hc\":%d}"),
            fst ? "" : ",", i,
            remote_nodes[i].online ? 1 : 0,
            remote_nodes[i].bus_id ? remote_nodes[i].bus_id : 1,
            (unsigned long)remote_nodes[i].packets_rx,
            (remote_nodes[i].last_seen > 0) ? 1 : 0,
            nt_str,
            remote_nodes[i].quality,
            remote_nodes[i].attempts,
            remote_nodes[i].fails_total,
            sec_ago,
            remote_nodes[i].backlight_pct,
            remote_nodes[i].led_mode,
            remote_nodes[i].has_config ? 1 : 0);
        fst = false;
      }
    }
    APP(PSTR("],\"rxh\":["));
    for (uint8_t q = 0; q < mqtt_rx_history_count; q++) {
      APP(PSTR("%s{\"t\":\"%s\",\"p\":\"%s\"}"), q == 0 ? "" : ",",
          mqtt_rx_history[q].topic, mqtt_rx_history[q].payload);
    }
    APP(PSTR("],\"txh\":["));
    for (uint8_t q = 0; q < mqtt_tx_history_count; q++) {
      APP(PSTR("%s{\"t\":\"%s\",\"p\":\"%s\"}"), q == 0 ? "" : ",",
          mqtt_tx_history[q].topic, mqtt_tx_history[q].payload);
    }
    APP(PSTR("],\"dbg\":{\"err\":%lu,\"lost\":%lu,\"full\":%lu,\"oth\":%lu,"
             "\"sfail\":%lu,\"btn\":%lu,\"k\":%lu,\"rx1\":%lu,\"rx2\":%lu,"
             "\"loopmax\":%lu,\"looplast\":%lu,\"lec\":%u,\"led\":%u,\"ram\":%"
             "d,\"sec\":[%lu,%lu,%lu,%lu,%lu],\"up\":%lu}}"),
        pjon_packets_err, dbg_err_lost, dbg_err_full, dbg_err_other,
        dbg_send_fail, dbg_btn_rx, dbg_k_rx, dbg_rx_b1, dbg_rx_b2,
        dbg_loop_max_us, dbg_loop_last_us, dbg_last_err_code, dbg_last_err_data,
        dbg_free_ram(), dbg_sec_max[0], dbg_sec_max[1], dbg_sec_max[2],
        dbg_sec_max[3], dbg_sec_max[4], millis() / 1000);
#undef APP
    for (uint8_t q = 0; q < 5; q++)
      dbg_sec_max[q] = 0;
    dbg_loop_max_us = 0;
    cl.print(F("HTTP/1.1 200 OK\r\nContent-Type: "
               "application/json\r\nConnection: close\r\nContent-Length: "));
    cl.print(n);
    cl.print(F("\r\n\r\n"));
    cl.write((const uint8_t *)sb, n);
    cl.flush();
    cl.stop();
    return;
  }

  if (strcmp(path, "/ota") == 0) {
    handle_ota_page(cl);
    return;
  }

  if (strncmp(path, "/api/ota_start", 14) == 0) {
    char t_node[8] = "1";
    char t_bus[8] = "1";
    char *q = strchr(path, '?');
    if (q) {
      get_form_value(q + 1, "node", t_node, sizeof(t_node));
      get_form_value(q + 1, "bus", t_bus, sizeof(t_bus));
    }

    char err_msg[64];
    bool ok = ota_start(atoi(t_node), atoi(t_bus), err_msg, sizeof(err_msg));
    cl.print(F("HTTP/1.1 200 OK\r\nContent-Type: "
               "application/json\r\nConnection: close\r\n\r\n"));
    if (ok) {
      cl.print(F("{\"status\":\"ok\",\"mcu\":\""));
      cl.print(ota_get_target_mcu());
      cl.print(F("\"}"));
    } else {
      cl.print(F("{\"status\":\"error\",\"msg\":\""));
      cl.print(err_msg);
      cl.print(F("\"}"));
    }
    cl.flush();
    cl.stop();
    return;
  }

  if (strncmp(path, "/api/ota_page", 13) == 0) {
    char t_addr[12] = "0";
    char *q = strchr(path, '?');
    if (q) {
      get_form_value(q + 1, "addr", t_addr, sizeof(t_addr));
    }
    uint16_t word_addr = (uint16_t)atoi(t_addr);

    uint8_t page_buf[256];
    int bytes_read = 0;
    unsigned long t0 = millis();
    while (bytes_read < content_length && bytes_read < 256 &&
           (millis() - t0 < 1000)) {
      if (cl.available()) {
        page_buf[bytes_read++] = cl.read();
      }
    }

    char err_msg[64];
    bool ok = ota_write_page(word_addr, page_buf, bytes_read, err_msg,
                             sizeof(err_msg));
    cl.print(F("HTTP/1.1 200 OK\r\nContent-Type: "
               "application/json\r\nConnection: close\r\n\r\n"));
    if (ok) {
      cl.print(F("{\"status\":\"ok\",\"mcu\":\""));
      cl.print(ota_get_target_mcu());
      cl.print(F("\"}"));
    } else {
      cl.print(F("{\"status\":\"error\",\"msg\":\""));
      cl.print(err_msg);
      cl.print(F("\"}"));
    }
    cl.flush();
    cl.stop();
    return;
  }

  if (strncmp(path, "/api/ota_end", 12) == 0) {
    char err_msg[32];
    ota_end(err_msg, sizeof(err_msg));
    cl.print(
        F("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: "
          "close\r\n\r\n{\"status\":\"ok\"}"));
    cl.flush();
    cl.stop();
    return;
  }

  if (strncmp(path, "/api/ota_abort", 14) == 0) {
    ota_abort();
    cl.print(
        F("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: "
          "close\r\n\r\n{\"status\":\"ok\"}"));
    cl.flush();
    cl.stop();
    return;
  }

  if (strcmp(path, "/reboot") == 0) {
    send_header(cl);
    wp_ln(cl, F("<!DOCTYPE html><html><head><meta charset='utf-8'><meta "
                "http-equiv='refresh' content='6;url=/'></head>"));
    send_css(cl);
    wp_ln(cl, F("<body><div class='wrap'><h1>Riavvio in Corso...</h1>"));
    wp_ln(cl,
          F("<p>Il Gateway si sta riavviando. La pagina si ricaricher&agrave; "
            "automaticamente tra 6 secondi.</p></div></body></html>"));
    cl.flush();
    delay(300);
    cl.stop();
    delay(100);
    wdt_enable(WDTO_250MS);
    while (1) {
    }
  }

  // GET / (Pagina Principale)
  send_header(cl);
  wp_ln(cl, F("<!DOCTYPE html><html><head><meta charset='utf-8'><title>MQTT "
              "Gateway</title>"));
  send_css(cl);
  wp_ln(cl, F("</head><body><div class='wrap'>"));
  wp_ln(cl, F("<h1><span>Gateway MQTT &mdash; ATmega128 <small style='font-size:12px;background:#f59e0b;color:#000;padding:2px 7px;border-radius:10px;font-weight:bold;vertical-align:middle;margin-left:6px'>BETA</small></span><a href='/ota' "
              "class='btn' style='float:right;font-size:13px;padding:6px "
              "14px;'>Aggiornamento OTA</a></h1>"));

  // Card 1: Diagnostica e Stato
  wp_ln(cl, F("<div class='card'><h3>Stato del Sistema</h3>"));
  wp_ln(cl, F("<div class='stat-grid'>"));

  // MQTT Status
  wp(cl, F("<div class='stat-box'>MQTT Broker<div class='stat-val'><span "
           "id='mqtt_badge' class='badge "));
  if (client.connected()) {
    wp(cl, F("badge-ok'>CONNESSO</span>"));
  } else {
    wp(cl, F("badge-err'>DISCONNESSO</span>"));
  }
  wp_ln(cl, F("</div></div>"));

  // IP Attuale
  wp(cl, F("<div class='stat-box'>IP Gateway<div class='stat-val'>"));
  wp(cl, Ethernet.localIP());
  wp_ln(cl, F("</div></div>"));

  // Temperatura LM75 I2C
  wp(cl, F("<div class='stat-box'>Temperatura LM75<div id='lm75_val' "
           "class='stat-val'>"));
  if (lm75_is_available()) {
    wp(cl, lm75_get_temp(), 1);
    wp(cl, F(" &deg;C"));
  } else {
    wp(cl, F("<span style='color:#94a3b8'>N.D.</span>"));
  }
  wp_ln(cl, F("</div></div>"));

  // PJON Packets
  wp(cl, F("<div class='stat-box'>Pacchetti PJON (RX / TX)<div id='pjon_stats' "
           "class='stat-val'>"));
  wp(cl, pjon_packets_rx);
  wp(cl, F(" / "));
  wp(cl, pjon_packets_tx);
  wp_ln(cl, F("</div></div>"));

  // MQTT Messages
  wp(cl, F("<div class='stat-box'>Messaggi MQTT (RX / TX)<div id='mqtt_stats' "
           "class='stat-val'>"));
  wp(cl, mqtt_messages_rx);
  wp(cl, F(" / "));
  wp(cl, mqtt_messages_tx);
  wp_ln(cl, F("</div></div>"));

  wp_ln(cl, F("</div></div>")); // Chiude stat-grid e card

  // Card Monitor Dati MQTT
  wp_ln(cl, F("<div class='card'>"));
  wp_ln(cl, F("<h3>Monitor Traffico MQTT (Tempo Reale)</h3>"));
  wp(cl, F("<div style='margin-bottom:12px;font-size:13px;color:#94a3b8'>Broker Configurato: <b style='color:#f1f5f9'>"));
  wp(cl, config.mqtt_server);
  wp(cl, F(":"));
  wp(cl, config.mqtt_port);
  wp(cl, F("</b> (Client: <code>"));
  wp(cl, config.mqtt_client_id);
  wp_ln(cl, F("</code>)</div>"));

  // Ultimi Ricevuti (RX)
  wp_ln(cl, F("<div style='margin-bottom:12px'>"));
  wp_ln(cl, F("<div style='display:flex;justify-content:space-between;align-items:center;margin-bottom:5px'>"));
  wp_ln(cl, F("<span style='color:#94a3b8;font-size:12px;font-weight:bold;text-transform:uppercase;letter-spacing:0.5px'>Ultimi Ricevuti (RX)</span>"));
  wp(cl, F("<span id='mrx_cnt_badge' style='color:#64748b;font-size:11px'>"));
  wp(cl, mqtt_messages_rx);
  wp_ln(cl, F(" messaggi</span></div>"));
  wp_ln(cl, F("<div id='mqtt_rx_list' style='background:#0f172a;border:1px solid #475569;border-radius:6px;padding:6px 10px;font-family:monospace;font-size:12px;min-height:65px;max-height:160px;overflow-y:auto'>"));
  if (mqtt_rx_history_count > 0) {
    for (uint8_t q = 0; q < mqtt_rx_history_count; q++) {
      wp(cl, F("<div style='padding:4px 0;border-bottom:1px solid #1e293b'><div style='display:flex;justify-content:space-between;align-items:center'><b style='color:#38bdf8;font-size:12px'>"));
      wp(cl, mqtt_rx_history[q].topic);
      wp(cl, F("</b><span class='badge' style='background:#0284c7;color:#fff;padding:1px 6px;font-size:11px'>"));
      wp(cl, mqtt_rx_history[q].payload);
      wp(cl, F("</span></div><div style='color:#64748b;font-size:11px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;margin-top:2px'><span style='color:#475569'>topic:</span> "));
      wp(cl, mqtt_rx_history[q].topic);
      wp_ln(cl, F("</div></div>"));
    }
  } else {
    wp_ln(cl, F("<div style='color:#64748b;font-style:italic;padding:4px 0'>In attesa di messaggi dal broker...</div>"));
  }
  wp_ln(cl, F("</div></div>"));

  // Ultimi Inviati (TX)
  wp_ln(cl, F("<div>"));
  wp_ln(cl, F("<div style='display:flex;justify-content:space-between;align-items:center;margin-bottom:5px'>"));
  wp_ln(cl, F("<span style='color:#94a3b8;font-size:12px;font-weight:bold;text-transform:uppercase;letter-spacing:0.5px'>Ultimi Inviati (TX)</span>"));
  wp(cl, F("<span id='mtx_cnt_badge' style='color:#64748b;font-size:11px'>"));
  wp(cl, mqtt_messages_tx);
  wp_ln(cl, F(" messaggi</span></div>"));
  wp_ln(cl, F("<div id='mqtt_tx_list' style='background:#0f172a;border:1px solid #475569;border-radius:6px;padding:6px 10px;font-family:monospace;font-size:12px;min-height:65px;max-height:160px;overflow-y:auto'>"));
  if (mqtt_tx_history_count > 0) {
    for (uint8_t q = 0; q < mqtt_tx_history_count; q++) {
      wp(cl, F("<div style='padding:4px 0;border-bottom:1px solid #1e293b'><div style='display:flex;justify-content:space-between;align-items:center'><b style='color:#38bdf8;font-size:12px'>"));
      wp(cl, mqtt_tx_history[q].topic);
      wp(cl, F("</b><span class='badge' style='background:#0284c7;color:#fff;padding:1px 6px;font-size:11px'>"));
      wp(cl, mqtt_tx_history[q].payload);
      wp(cl, F("</span></div><div style='color:#64748b;font-size:11px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;margin-top:2px'><span style='color:#475569'>topic:</span> "));
      wp(cl, mqtt_tx_history[q].topic);
      wp_ln(cl, F("</div></div>"));
    }
  } else {
    wp_ln(cl, F("<div style='color:#64748b;font-style:italic;padding:4px 0'>Nessun messaggio inviato</div>"));
  }
  wp_ln(cl, F("</div></div>"));

  wp_ln(cl, F("</div>"));

  // Card Diagnostica Nodi Remoti IO16 (Mappa Grafica 63 Nodi)
  wp_ln(cl, F("<div class='card'>"));
  wp_ln(cl, F("<div "
              "style='display:flex;justify-content:space-between;align-items:"
              "center;margin-bottom:10px'>"));
  wp_ln(cl, F("<h3 style='margin:0'>Mappa Dispositivi RS485 (1..63)</h3>"));
  wp_ln(cl, F("<button type='button' id='btn_ping' onclick='doPing(this)' "
              "class='btn' style='padding:6px "
              "12px;font-size:12px;margin:0'>Ping Nodi Attivi</button>"));
  wp_ln(cl, F("</div>"));

  // Contatori sintetici stato nodi
  uint8_t c_on = 0, c_off = 0, c_ina = 0;
  for (uint8_t i = 1; i <= MAX_DIAG_NODES; i++) {
    if (!remote_nodes[i].active)
      c_ina++;
    if (!remote_nodes[i].active || remote_nodes[i].last_seen == 0)
      c_ina++;
    else if (remote_nodes[i].online)
      c_on++;
    else
      c_off++;
  }

  wp_ln(cl, F("<div "
              "style='display:flex;gap:14px;margin-bottom:12px;font-size:12px;"
              "align-items:center'>"));
  wp(cl, F("<span style='display:inline-flex;align-items:center;gap:5px'><span "
           "style='width:9px;height:9px;border-radius:50%;background:#16a34a;"
           "display:inline-block'></span><b>Online:</b> <span id='cnt_on'>"));
  wp(cl, c_on);
  wp(cl, F("</span></span>"));
  wp(cl, F("<span style='display:inline-flex;align-items:center;gap:5px'><span "
           "style='width:9px;height:9px;border-radius:50%;background:#dc2626;"
           "display:inline-block'></span><b>Offline:</b> <span id='cnt_off'>"));
  wp(cl, F("<span style='display:inline-flex;align-items:center;gap:5px'><span "
           "style='width:9px;height:9px;border-radius:50%;background:#dc2626;"
           "display:inline-block'></span><b>Offline (Persi):</b> <span "
           "id='cnt_off'>"));
  wp(cl, c_off);
  wp(cl, F("</span></span>"));
  wp(cl,
     F("<span style='display:inline-flex;align-items:center;gap:5px'><span "
       "style='width:9px;height:9px;border-radius:50%;background:#475569;"
       "display:inline-block'></span><b>Inattivi:</b> <span id='cnt_ina'>"));
  wp(cl,
     F("<span style='display:inline-flex;align-items:center;gap:5px'><span "
       "style='width:9px;height:9px;border-radius:50%;background:#475569;"
       "display:inline-block'></span><b>Non Trovati:</b> <span id='cnt_ina'>"));
  wp(cl, c_ina);
  wp_ln(cl, F("</span></span></div>"));

  // Griglia a matrice grafica per tutti i 63 nodi (senza occupare spazio
  // verticale)
  wp_ln(cl, F("<div "
              "style='display:grid;grid-template-columns:repeat(auto-fill,"
              "minmax(33px,1fr));gap:4px;margin-bottom:16px'>"));
  for (uint8_t i = 1; i <= MAX_DIAG_NODES; i++) {
    wp(cl, F("<div id='ng_"));
    wp(cl, i);
    wp(cl, F("' style='text-align:center;padding:6px "
             "0;font-size:11px;font-weight:bold;border-radius:4px;border:1px "
             "solid "));
    if (remote_nodes[i].online) {
      wp(cl, F("#22c55e;background:#16a34a;color:#fff' title='Nodo "));
      wp(cl, i);
      wp(cl, F(": ONLINE (Bus "));
      wp(cl, remote_nodes[i].bus_id ? remote_nodes[i].bus_id : 1);
      wp(cl, F(")'"));
    } else if (remote_nodes[i].active && remote_nodes[i].last_seen > 0) {
      wp(cl, F("#ef4444;background:#dc2626;color:#fff' title='Nodo "));
      wp(cl, i);
      wp(cl, F(": OFFLINE'"));
      wp(cl, F(": OFFLINE (Disconnesso)'"));
    } else {
      wp(cl, F("#334155;background:#1e293b;color:#64748b;opacity:0.6' "
               "title='Nodo "));
      wp(cl, i);
      wp(cl, F(": Inattivo'"));
      wp(cl, F(": Non trovato'"));
    }
    wp(cl, F(">"));
    wp(cl, i);
    wp_ln(cl, F("</div>"));
  }
  wp_ln(cl, F("</div>"));

  // Tabella dettagliata: SOLO ED ESCLUSIVAMENTE I NODI CONTATTATI
  wp_ln(
      cl,
      F("<div style='display:flex;justify-content:space-between;align-items:center;margin:14px 0 8px'>"
        "<h4 style='margin:0;color:#cbd5e1;font-size:13px'>Dettaglio Dispositivi Contattati</h4>"
        "<div>"
        "<button id='btn_scan' class='btn' onclick='startBusScan()' style='padding:5px 12px;font-size:12px;margin:0 6px 0 0;background:#0284c7'>🔍 Scansiona Bus</button>"
        "<button id='btn_clear' class='btn btn-red' onclick='clearNodes()' style='padding:5px 12px;font-size:12px;margin:0'>🗑️ Azzera Dispositivi</button>"
        "</div>"
        "</div>"));
  wp_ln(cl, F("<table id='tbl_nodes' style='width:100%;border-collapse:collapse;font-size:13px;text-align:left'>"));
  wp_ln(cl, F("<tr style='color:#94a3b8;border-bottom:1px solid #475569'><th style='padding:6px'>Nodo</th><th>Interfaccia</th><th>Stato</th><th>Temperatura</th><th>Qualit&agrave; Link</th><th>Backlight</th><th>Modo LED</th><th>Attivit&agrave;</th><th>Pacchetti</th><th>Azione</th></tr>"));

  bool any_active = false;
  unsigned long cur_now = millis();
  for (uint8_t i = 1; i <= MAX_DIAG_NODES; i++) {
    if (!remote_nodes[i].active || remote_nodes[i].last_seen == 0)
      continue;
    any_active = true;

    wp(cl, F("<tr id='tr_n_"));
    wp(cl, i);
    wp(cl, F("' style='border-bottom:1px solid #334155'><td style='padding:7px 6px'><b>#"));
    wp(cl, i);
    wp(cl, F("</b></td><td>"));
    if (remote_nodes[i].bus_id == 2) {
      wp(cl, F("<span class='badge' style='background:#0284c7;color:#fff'>RS485-2 (U3)</span>"));
    } else {
      wp(cl, F("<span class='badge' style='background:#4f46e5;color:#fff'>RS485-1 (U2)</span>"));
    }
    wp(cl, F("</td><td id='st_"));
    wp(cl, i);
    wp(cl, F("'>"));

    if (remote_nodes[i].online) {
      wp(cl, F("<span class='badge badge-ok'>ONLINE</span>"));
    } else {
      wp(cl, F("<span class='badge badge-err'>OFFLINE</span>"));
    }
    wp(cl, F("</td><td id='tp_"));
    wp(cl, i);
    wp(cl, F("'>"));

    if (remote_nodes[i].has_temp && remote_nodes[i].temp > 0.0f) {
      wp(cl, F("<span class='badge' style='background:#0284c7;color:#fff'>"));
      wp(cl, remote_nodes[i].temp, 1);
      wp(cl, F(" &deg;C</span>"));
    } else {
      wp(cl, F("<span style='color:#64748b'>-</span>"));
    }
    wp(cl, F("</td><td id='ql_"));
    wp(cl, i);
    wp(cl, F("'>"));

    if (!remote_nodes[i].online) {
      wp(cl, F("<span class='badge badge-err'>0% (Off)</span>"));
    } else if (remote_nodes[i].quality >= 90) {
      wp(cl, F("<span class='badge badge-ok' title='1 tentativo (ottimo)'>100% (1 tent.)</span>"));
    } else if (remote_nodes[i].quality >= 70) {
      wp(cl, F("<span class='badge' style='background:#15803d;color:#fff' title='2 tentativi'>80% (2 tent.)</span>"));
    } else if (remote_nodes[i].quality >= 50) {
      wp(cl, F("<span class='badge' style='background:#ca8a04;color:#000' title='Tentativi multipli'>"));
      wp(cl, remote_nodes[i].quality);
      wp(cl, F("% ("));
      wp(cl, remote_nodes[i].attempts);
      wp(cl, F(" tent.)</span>"));
    } else if (remote_nodes[i].quality > 0) {
      wp(cl, F("<span class='badge badge-err' title='Molti tentativi'>"));
      wp(cl, remote_nodes[i].quality);
      wp(cl, F("% ("));
      wp(cl, remote_nodes[i].attempts);
      wp(cl, F(" tent.)</span>"));
    } else {
      wp(cl, F("<span class='badge badge-ok' title='1 tentativo (ottimo)'>100% (1 tent.)</span>"));
    }
    if (remote_nodes[i].fails_total > 0) {
      wp(cl, F(" <span style='color:#f59e0b;font-size:11px' title='Fallimenti storici: "));
      wp(cl, remote_nodes[i].fails_total);
      wp(cl, F("'>("));
      wp(cl, remote_nodes[i].fails_total);
      wp(cl, F(" err)</span>"));
    }
    wp(cl, F("</td><td id='bl_"));
    wp(cl, i);
    wp(cl, F("'><span class='badge' style='background:#475569;color:#fff'>"));
    wp(cl, remote_nodes[i].backlight_pct);
    wp(cl, F("%</span></td><td id='lm_"));
    wp(cl, i);
    wp(cl, F("'>"));
    if (remote_nodes[i].led_mode == 0) wp(cl, F("<span class='badge' style='background:#475569;color:#fff'>Remoto</span>"));
    else if (remote_nodes[i].led_mode == 1) wp(cl, F("<span class='badge' style='background:#0284c7;color:#fff'>Temp 5s</span>"));
    else if (remote_nodes[i].led_mode == 2) wp(cl, F("<span class='badge' style='background:#8b5cf6;color:#fff'>Toggle</span>"));
    else if (remote_nodes[i].led_mode == 3) wp(cl, F("<span class='badge' style='background:#d97706;color:#fff'>Momentaneo</span>"));
    else wp(cl, F("<span class='badge'>-</span>"));
    wp(cl, F("</td><td id='ls_"));
    wp(cl, i);
    wp(cl, F("'>"));

    if (remote_nodes[i].last_seen > 0) {
      unsigned long sec_ago = (cur_now - remote_nodes[i].last_seen) / 1000;
      wp(cl, sec_ago);
      wp(cl, F("s fa"));
    } else {
      wp(cl, F("-"));
    }
    wp(cl, F("</td><td id='pk_"));
    wp(cl, i);
    wp(cl, F("'>"));
    wp(cl, remote_nodes[i].packets_rx);
    wp(cl, F("</td><td id='act_"));
    wp(cl, i);
    wp(cl, F("'><button class='btn' style='padding:2px 8px;font-size:11px;margin:0;background:#0284c7' onclick='selectNodeForConfig("));
    wp(cl, i);
    wp(cl, F(","));
    wp(cl, remote_nodes[i].backlight_pct);
    wp(cl, F(","));
    wp(cl, remote_nodes[i].led_mode);
    wp_ln(cl, F(")'>⚙️</button></td></tr>"));
  }

  if (!any_active) {
    wp_ln(cl, F("<tr id='no_nodes_row'><td colspan='10' style='padding:10px 6px;color:#94a3b8;font-style:italic'>Nessun dispositivo ha ancora comunicato. I nodi appariranno qui appena trasmettono sul bus.</td></tr>"));
  }

  wp_ln(cl, F("</table></div>"));

  // Card 2: Test Rapido PJON
  wp_ln(cl, F("<details class='card'><summary>Test Rapido Bus RS485</summary>"));
  wp_ln(cl,
        F("<form onsubmit='event.preventDefault();doTest(this);' "
          "style='display:flex;gap:10px;align-items:center;flex-wrap:wrap'>"));
  wp_ln(cl, F("<span>Client ID: <input type='number' name='addr' value='1' "
              "min='1' max='253' style='width:65px'></span>"));
  wp_ln(cl, F("<span>LED: <select name='led' "
              "style='padding:8px;background:#0f172a;color:#fff;border:1px "
              "solid #475569;border-radius:5px'><option value='0'>Tutti "
              "(0)</option><option value='1'>LED 1</option><option "
              "value='2'>LED 2</option><option value='3'>LED 3</option><option "
              "value='4'>LED 4</option><option value='5'>LED 5</option><option "
              "value='6'>LED 6</option><option value='7'>LED 7</option><option "
              "value='8'>LED 8</option></select></span>"));
  wp_ln(
      cl,
      F("<span>Stato: <select name='stat' "
        "style='padding:8px;background:#0f172a;color:#fff;border:1px solid "
        "#475569;border-radius:5px'><option value='1'>Accendi</option><option "
        "value='0'>Spegni</option></select></span>"));
  wp_ln(cl, F("<button type='submit' class='btn btn-green' "
              "style='margin-top:0'>Invia Test</button>"));
  wp_ln(cl, F("</form></details>"));

  // Card Configurazione Parametri Tastiera IO16 (EEPROM)
  wp_ln(cl, F("<details class='card' id='card_node_cfg'>"));
  wp_ln(cl, F("<summary>Configurazione Tastiere IO16 (Backlight & Modi LED)</summary>"));
  wp_ln(cl, F("<p style='color:#94a3b8;font-size:13px;margin:0 0 14px'>Regola la percentuale di retroilluminazione in standby e la modalit&agrave; locale dei LED. Puoi applicarla a un singolo nodo o a <b>tutti i nodi contemporaneamente</b> in una sola volta (salvataggio in EEPROM via RS485).</p>"));
  wp_ln(cl, F("<div style='display:grid;grid-template-columns:1fr 1fr;gap:12px;margin-bottom:12px'>"));
  wp_ln(cl, F("<div><label style='margin-top:0'>Target Configurazione:</label><select id='cfg_node' onchange='onNodeSelectChange()' style='padding:9px;background:#0f172a;border:1px solid #475569;border-radius:5px;color:#fff;width:100%'><option value='all' selected>🌟 TUTTI I NODI (Broadcast Mass Config)</option><option value='6'>Nodo 6 (Bus 1)</option><option value='24'>Nodo 24 (Bus 1)</option></select></div>"));
  wp_ln(cl, F("<div><label style='margin-top:0'>Modalit&agrave; Accensione LED:</label><select id='cfg_mode' style='padding:9px;background:#0f172a;border:1px solid #475569;border-radius:5px;color:#fff;width:100%'><option value='0'>0: Solo Remoto (MQTT / HA)</option><option value='1' selected>1: Temporizzato 5s (Feedback locale)</option><option value='2'>2: Toggle Bistabile (ON/OFF locale)</option><option value='3'>3: Momentaneo (Premuto = ON)</option></select></div>"));
  wp_ln(cl, F("</div>"));
  wp_ln(cl, F("<div style='margin-bottom:14px'><div style='display:flex;justify-content:space-between;align-items:center'><label style='margin-top:0'>Retroilluminazione Standby (%):</label><b id='bl_val_label' style='color:#38bdf8;font-size:15px'>12%</b></div><input type='range' id='cfg_backlight' min='0' max='100' step='1' value='12' style='width:100%;margin-top:6px' oninput=\"document.getElementById('bl_val_label').innerText=this.value+'%'\"><div style='display:flex;justify-content:space-between;color:#64748b;font-size:11px'><span>0% (Spento)</span><span>12% (Default)</span><span>25%</span><span>50%</span><span>100% (Piena)</span></div></div>"));
  wp_ln(cl, F("<div style='display:flex;align-items:center;gap:10px;flex-wrap:wrap'><button type='button' id='btn_read_cfg' class='btn' onclick='readNodeCfg()' style='margin-top:0;opacity:0.5' disabled title='Seleziona un singolo nodo per leggere'>📥 Leggi dal Nodo</button><button type='button' id='btn_save_cfg' class='btn btn-green' onclick='saveNodeCfg()' style='margin-top:0'>💾 Salva in EEPROM su TUTTI i Nodi</button><span id='cfg_msg' style='font-size:13px;font-weight:bold'></span></div>"));
  wp_ln(cl, F("</details>"));

  // Card 3: Form di Configurazione
  wp_ln(cl, F("<form method='POST' action='/save' onsubmit=\"['inp_ip','inp_gw','inp_sub','inp_dns'].forEach(function(i){var e=document.getElementById(i);if(e)e.disabled=false;})\">"));

  // Sezione Rete
  wp_ln(cl, F("<details class='card'><summary>Parametri Rete Ethernet (W5500)</summary>"));
  wp(cl, F("<label><input type='checkbox' name='dhcp' id='cb_dhcp' value='1' onchange='toggleDhcp()' "));
  if (config.dhcp)
    wp(cl, F("checked"));
  wp_ln(cl, F("> Abilita DHCP Automatico</label>"));

  wp_ln(cl, F("<div id='static_ip_fields'>"));

  wp_ln(cl, F("<label>Indirizzo IP Statico:"));
  wp(cl, F("<input type='text' id='inp_ip' name='ip' value='"));
  wp(cl, config.ip[0]);
  wp(cl, '.');
  wp(cl, config.ip[1]);
  wp(cl, '.');
  wp(cl, config.ip[2]);
  wp(cl, '.');
  wp(cl, config.ip[3]);
  wp_ln(cl, F("'></label>"));

  wp_ln(cl, F("<label>Gateway:"));
  wp(cl, F("<input type='text' id='inp_gw' name='gw' value='"));
  wp(cl, config.gateway[0]);
  wp(cl, '.');
  wp(cl, config.gateway[1]);
  wp(cl, '.');
  wp(cl, config.gateway[2]);
  wp(cl, '.');
  wp(cl, config.gateway[3]);
  wp_ln(cl, F("'></label>"));

  wp_ln(cl, F("<label>Subnet Mask:"));
  wp(cl, F("<input type='text' id='inp_sub' name='sub' value='"));
  wp(cl, config.subnet[0]);
  wp(cl, '.');
  wp(cl, config.subnet[1]);
  wp(cl, '.');
  wp(cl, config.subnet[2]);
  wp(cl, '.');
  wp(cl, config.subnet[3]);
  wp_ln(cl, F("'></label>"));

  wp_ln(cl, F("<label>DNS Server:"));
  wp(cl, F("<input type='text' id='inp_dns' name='dns' value='"));
  wp(cl, config.dns[0]);
  wp(cl, '.');
  wp(cl, config.dns[1]);
  wp(cl, '.');
  wp(cl, config.dns[2]);
  wp(cl, '.');
  wp(cl, config.dns[3]);
  wp_ln(cl, F("'></label>"));
  wp_ln(cl, F("</div>"));
  wp_ln(cl, F("</details>"));

  // Sezione MQTT
  wp_ln(cl, F("<details class='card'><summary>Parametri Broker MQTT</summary>"));
  wp_ln(cl, F("<label>IP / Hostname Broker:"));
  wp(cl, F("<input type='text' name='msrv' value='"));
  wp(cl, config.mqtt_server);
  wp_ln(cl, F("'></label>"));

  wp_ln(cl, F("<label>Porta (default 1883):"));
  wp(cl, F("<input type='number' name='mprt' value='"));
  wp(cl, config.mqtt_port);
  wp_ln(cl, F("'></label>"));

  wp_ln(cl, F("<label>Username:"));
  wp(cl, F("<input type='text' name='musr' value='"));
  wp(cl, config.mqtt_user);
  wp_ln(cl, F("'></label>"));

  wp_ln(cl, F("<label>Password:"));
  wp(cl, F("<input type='password' name='mpwd' value='"));
  wp(cl, config.mqtt_pass);
  wp_ln(cl, F("'></label>"));

  wp_ln(cl, F("<label>Nome Descrittivo Gateway (es. Piano Terra):"));
  wp(cl, F("<input type='text' name='gwnm' value='"));
  wp(cl, config.gateway_name);
  wp_ln(cl, F("'></label>"));

  wp_ln(cl, F("<label>ID Univoco Gateway / MQTT Client ID (necessario per "
              "Gateway multipli):"));
  wp(cl, F("<input type='text' name='mcid' value='"));
  wp(cl, config.mqtt_client_id);
  wp_ln(cl, F("'></label>"));

  wp_ln(cl, F("<label>Prefisso Topic Legacy (es. scene o pjon):"));
  wp(cl, F("<input type='text' name='mpre' value='"));
  wp(cl, config.mqtt_prefix);
  wp_ln(cl, F("'></label>"));
  wp_ln(cl, F("</details>"));

  // Sezione Home Assistant
  wp_ln(cl, F("<details class='card'><summary>Integrazione Home Assistant "
              "(Auto-Discovery)</summary>"));
  wp(cl, F("<label><input type='checkbox' name='hadisc' value='1' "));
  if (config.ha_discovery)
    wp(cl, F("checked"));
  wp_ln(cl, F("> Abilita MQTT Auto-Discovery per Home Assistant</label>"));
  wp_ln(cl, F("</details>"));

  // Sezione Bus PJON
  wp_ln(cl, F("<details class='card'><summary>Parametri Bus RS485 (PJON)</summary>"));
  wp_ln(cl, F("<label>ID Gateway (default 254):"));
  wp(cl, F("<input type='number' name='pjid' value='"));
  wp(cl, config.pjon_id);
  wp_ln(cl, F("'></label>"));

  wp_ln(cl, F("<label>Baud Rate Seriale RS485 (default 38400):"));
  wp(cl, F("<input type='number' name='pjbd' value='"));
  wp(cl, config.pjon_baud);
  wp_ln(cl, F("'></label>"));
  wp_ln(cl, F("</details>"));

  wp_ln(cl, F("<button type='submit' class='btn'>Salva Configurazione in "
              "EEPROM</button> "));
  wp_ln(cl, F("<a href='/reboot' class='btn btn-red'>Riavvia Gateway</a>"));
  wp_ln(cl, F("</form>"));

  // Script per aggiornamento automatico dinamico ogni 2 secondi senza
  // ricaricare la pagina
  wp_ln(cl, F("<script>"));
  wp_ln(cl,
        F("function doPing(b){var ot=b.innerText;b.innerText='Invio "
          "Ping...';b.disabled=true;fetch('/"
          "pingall').then(function(){b.innerText='Ping "
          "Inviato!';setTimeout(function(){b.innerText=ot;b.disabled=false;},"
          "1200);}).catch(function(){b.innerText=ot;b.disabled=false;});}"));
  wp_ln(cl, F("function doTest(f){var fd=new FormData(f);var "
              "b=f.querySelector('button');var "
              "ot=b.innerText;b.innerText='Invio...';b.disabled=true;var "
              "q='addr='+fd.get('addr')+'&led='+fd.get('led')+'&stat='+fd.get('"
              "stat');fetch('/"
              "test?'+q).then(function(){b.innerText='Inviato!';setTimeout("
              "function(){b.innerText=ot;b.disabled=false;},1200);}).catch("
              "function(){b.innerText=ot;b.disabled=false;});}"));
  wp_ln(cl, F("function descMsg(t,p,rx){var m;if((m=t.match(/node\\/(\\d+)\\/temp/)))return 'Temp. Nodo '+m[1]+' ('+p+' &deg;C)';if(t.indexOf('temp')!==-1)return 'Temp. Gateway ('+p+' &deg;C)';if((m=t.match(/node\\/(\\d+)\\/btn\\/(\\d+)/))){var b=parseInt(m[2]);if(b<=8)return 'Click P'+b+' Nodo '+m[1];if(b<=16)return 'Long Press P'+(b-8)+' Nodo '+m[1];return 'Doppio Click P'+(b-16)+' Nodo '+m[1];}if((m=t.match(/node\\/(\\d+)\\/led\\/(\\d+)/)))return 'Comando LED '+m[2]+' Nodo '+m[1]+' ('+p+')';if((m=t.match(/scene\\/(\\d+)\\/(\\d+)/)))return (rx?'Comando LED ':'Click Tasto ')+m[2]+' Nodo '+m[1];if(t.indexOf('status')!==-1)return 'Stato Gateway: '+p;if(t.indexOf('announce')!==-1)return 'Annuncio Broker';return (rx?'Ricevuto: ':'Inviato: ')+p;}"));
  wp_ln(cl, F("function bgMsg(p){var u=(''+p).toUpperCase();if(u==='ON'||u==='ONLINE'||u==='1')return 'background:#16a34a;color:#fff';if(u==='OFF'||u==='OFFLINE'||u==='0')return 'background:#dc2626;color:#fff';if(!isNaN(parseFloat(p)))return 'background:#0284c7;color:#fff';return 'background:#475569;color:#fff';}"));
  wp_ln(cl, F("function getModeLabel(m){if(m===0)return '<span class=\"badge\" style=\"background:#475569;color:#fff\">Remoto</span>';if(m===1)return '<span class=\"badge\" style=\"background:#0284c7;color:#fff\">Temp 5s</span>';if(m===2)return '<span class=\"badge\" style=\"background:#8b5cf6;color:#fff\">Toggle</span>';if(m===3)return '<span class=\"badge\" style=\"background:#d97706;color:#fff\">Momentaneo</span>';return '<span class=\"badge\">-</span>';}"));
  wp_ln(cl, F("function onNodeSelectChange(){var v=document.getElementById('cfg_node').value;var br=document.getElementById('btn_read_cfg');var bs=document.getElementById('btn_save_cfg');if(v==='all'){if(br){br.disabled=true;br.style.opacity='0.5';br.title='Seleziona un singolo nodo per leggere la configurazione';}if(bs)bs.innerText='💾 Salva in EEPROM su TUTTI i Nodi';}else{if(br){br.disabled=false;br.style.opacity='1';br.title='';}if(bs)bs.innerText='💾 Salva in EEPROM Nodo '+v;}}"));
  wp_ln(cl, F("function selectNodeForConfig(id,bl,md){var ni=document.getElementById('cfg_node');if(ni){ni.value=id;onNodeSelectChange();}if(bl!==undefined){var sl=document.getElementById('cfg_backlight');if(sl)sl.value=bl;var lb=document.getElementById('bl_val_label');if(lb)lb.innerText=bl+'%';}if(md!==undefined){var ms=document.getElementById('cfg_mode');if(ms)ms.value=md;}var el=document.getElementById('card_node_cfg');if(el){el.open=true;el.scrollIntoView({behavior:'smooth'});}}"));
  wp_ln(cl, F("function readNodeCfg(){var nid=document.getElementById('cfg_node').value;if(nid==='all'){alert('Seleziona un singolo nodo specifico per leggere i parametri.');return;}var msg=document.getElementById('cfg_msg');if(msg){msg.style.color='#38bdf8';msg.innerText='Lettura da Nodo '+nid+'...';}fetch('/api/node_config?node='+nid).then(function(r){return r.json()}).then(function(d){if(d.status==='ok'){document.getElementById('cfg_backlight').value=d.backlight;document.getElementById('bl_val_label').innerText=d.backlight+'%';document.getElementById('cfg_mode').value=d.mode;if(msg){msg.style.color='#22c55e';msg.innerText='Configurazione letta!';setTimeout(function(){msg.innerText='';},3000);}}else{if(msg){msg.style.color='#ef4444';msg.innerText='Errore o timeout';}}}).catch(function(){if(msg){msg.style.color='#ef4444';msg.innerText='Errore richiesta';}});}"));
  wp_ln(cl, F("function saveNodeCfg(){var nid=document.getElementById('cfg_node').value;var bl=document.getElementById('cfg_backlight').value;var md=document.getElementById('cfg_mode').value;var msg=document.getElementById('cfg_msg');var btn=document.getElementById('btn_save_cfg');var ot=btn?btn.innerText:'';if(btn){btn.disabled=true;btn.innerText='Salvataggio in corso...';}if(msg){msg.style.color='#38bdf8';msg.innerText=(nid==='all')?'Salvataggio su TUTTI i nodi in corso...':('Salvataggio in EEPROM Nodo '+nid+'...');}fetch('/api/node_config?node='+nid+'&backlight='+bl+'&mode='+md,{method:'POST'}).then(function(r){return r.json()}).then(function(d){if(d.status==='ok'){if(msg){msg.style.color='#22c55e';msg.innerText=(nid==='all')?('Salvato con successo su TUTTI i nodi! ('+(d.count||'tutti')+' nodi aggiornati)'):('Salvato in EEPROM Nodo '+nid+'!');setTimeout(function(){msg.innerText='';},4000);}}else{if(msg){msg.style.color='#ef4444';msg.innerText='Errore salvataggio: '+(d.msg||'');}}if(btn){btn.disabled=false;btn.innerText=ot;}}).catch(function(){if(msg){msg.style.color='#ef4444';msg.innerText='Errore richiesta';}if(btn){btn.disabled=false;btn.innerText=ot;}});}"));
  wp_ln(cl, F("function startBusScan(){var b=document.getElementById('btn_scan');if(b){b.disabled=true;b.innerText='Scansione avviata...';b.style.opacity='0.7';}fetch('/api/scan').catch(function(){});}"));
  wp_ln(cl, F("function clearNodes(){if(!confirm('Vuoi davvero azzerare tutti i nodi e ricominciare la ricerca?'))return;var b=document.getElementById('btn_clear');if(b){b.disabled=true;b.innerText='Azzeramento...';}fetch('/api/clear').then(function(){var tb=document.getElementById('tbl_nodes');if(tb){while(tb.rows.length>1)tb.deleteRow(1);var r=tb.insertRow(-1);r.id='no_nodes_row';r.innerHTML='<td colspan=\"10\" style=\"padding:10px 6px;color:#94a3b8;font-style:italic\">Nessun dispositivo rilevato. Clicca &quot;Scansiona Bus&quot; per avviare la ricerca.</td>';}for(var i=1;i<=63;i++){var g=document.getElementById('ng_'+i);if(g){g.style.background='#1e293b';g.style.borderColor='#334155';g.style.color='#64748b';g.style.opacity='0.6';}}var eo=document.getElementById('cnt_on');if(eo)eo.innerText='0';var ef=document.getElementById('cnt_off');if(ef)ef.innerText='0';var ei=document.getElementById('cnt_ina');if(ei)ei.innerText='63';if(b){b.disabled=false;b.innerText='🗑️ Azzera Dispositivi';}}).catch(function(){if(b){b.disabled=false;b.innerText='🗑️ Azzera Dispositivi';}});}"));
  wp_ln(cl, F("function toggleDhcp(){var d=document.getElementById('cb_dhcp');var dis=d?d.checked:false;var box=document.getElementById('static_ip_fields');if(box){box.style.opacity=dis?'0.35':'1';box.style.pointerEvents=dis?'none':'auto';}['inp_ip','inp_gw','inp_sub','inp_dns'].forEach(function(i){var e=document.getElementById(i);if(e)e.disabled=dis;});}toggleDhcp();"));
  wp_ln(cl, F("setInterval(function(){"));
  wp_ln(cl, F("fetch('/api/status').then(function(r){return "
              "r.json()}).then(function(d){"));
  wp_ln(cl, F("var bsc=document.getElementById('btn_scan');if(bsc&&d.scan!==undefined){if(d.scan){bsc.disabled=true;bsc.innerText='Scansione (1..63)...';bsc.style.opacity='0.7';}else{bsc.disabled=false;bsc.innerText='🔍 Scansiona Bus';bsc.style.opacity='1';}}"));
  wp_ln(cl, F("var m=document.getElementById('mqtt_badge');"));
  wp_ln(cl, F("if(m){m.className='badge '+(d.mqtt?'badge-ok':'badge-err');m.innerText=d.mqtt?'CONNESSO':'DISCONNESSO';}"));
  wp_ln(
      cl,
      F("var p=document.getElementById('pjon_stats');if(p)p.innerText=d.prx+' "
        "/ '+d.ptx;"));
  wp_ln(
      cl,
      F("var s=document.getElementById('mqtt_stats');if(s)s.innerText=d.mrx+' "
        "/ '+d.mtx;"));
  wp_ln(cl,
        F("var "
          "te=document.getElementById('lm75_val');if(te){if(d.temp!==null&&d."
          "temp!==undefined)te.innerHTML=d.temp.toFixed(1)+' &deg;C';else "
          "te.innerHTML='<span style=\"color:#94a3b8\">N.D.</span>';}"));
  wp_ln(cl, F("if(d.rxh&&d.rxh.length>0){var rh='';d.rxh.forEach(function(m){rh+='<div style=\"padding:4px 0;border-bottom:1px solid #1e293b\"><div style=\"display:flex;justify-content:space-between;align-items:center\"><b style=\"color:#38bdf8;font-size:12px\">'+descMsg(m.t,m.p,true)+'</b><span class=\"badge\" style=\"padding:1px 6px;font-size:11px;'+bgMsg(m.p)+'\">'+m.p+'</span></div><div style=\"color:#64748b;font-size:11px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;margin-top:2px\"><span style=\"color:#475569\">topic:</span> '+m.t+'</div></div>';});var rbox=document.getElementById('mqtt_rx_list');if(rbox)rbox.innerHTML=rh;}"));
  wp_ln(cl, F("var rxb=document.getElementById('mrx_cnt_badge');if(rxb)rxb.innerText=d.mrx+' messaggi';"));
  wp_ln(cl, F("if(d.txh&&d.txh.length>0){var th='';d.txh.forEach(function(m){th+='<div style=\"padding:4px 0;border-bottom:1px solid #1e293b\"><div style=\"display:flex;justify-content:space-between;align-items:center\"><b style=\"color:#38bdf8;font-size:12px\">'+descMsg(m.t,m.p,false)+'</b><span class=\"badge\" style=\"padding:1px 6px;font-size:11px;'+bgMsg(m.p)+'\">'+m.p+'</span></div><div style=\"color:#64748b;font-size:11px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;margin-top:2px\"><span style=\"color:#475569\">topic:</span> '+m.t+'</div></div>';});var tbox=document.getElementById('mqtt_tx_list');if(tbox)tbox.innerHTML=th;}"));
  wp_ln(cl, F("var txb=document.getElementById('mtx_cnt_badge');if(txb)txb.innerText=d.mtx+' messaggi';"));
  wp_ln(cl, F("if(d.nds){"));
  wp_ln(cl, F("var csel=document.getElementById('cfg_node');if(csel){var seen={};for(var q=0;q<csel.options.length;q++)seen[csel.options[q].value]=true;d.nds.forEach(function(n){if(n.s&&!seen[''+n.i]){var opt=document.createElement('option');opt.value=n.i;opt.innerText='Nodo '+n.i+' (Bus '+(n.b||1)+')';csel.appendChild(opt);seen[''+n.i]=true;}});}"));
  wp_ln(cl, F("var on=0,off=0,any_s=false;var tb=document.getElementById('tbl_nodes');"));
  wp_ln(cl, F("d.nds.forEach(function(n){"));
  wp_ln(cl, F("var g=document.getElementById('ng_'+n.i);"));
  wp_ln(cl, F("if(g){"));
  wp_ln(cl, F("if(n.o){g.style.background='#16a34a';g.style.borderColor='#22c55e';g.style.color='#fff';g.style.opacity='1';on++;}"));
  wp_ln(cl, F("else if(n.s){g.style.background='#dc2626';g.style.borderColor='#ef4444';g.style.color='#fff';g.style.opacity='1';off++;}"));
  wp_ln(cl, F("else{g.style.background='#1e293b';g.style.borderColor='#334155';g.style.color='#64748b';g.style.opacity='0.6';}"));
  wp_ln(cl, F("}"));
  wp_ln(cl, F("if(n.s){"));
  wp_ln(cl, F("any_s=true;if(tb){"));
  wp_ln(cl, F("var row=document.getElementById('tr_n_'+n.i);"));
  wp_ln(cl, F("if(!row){"));
  wp_ln(cl, F("var nr=document.getElementById('no_nodes_row');if(nr)nr.remove();"));
  wp_ln(cl, F("row=tb.insertRow(-1);row.id='tr_n_'+n.i;row.style.borderBottom='1px solid #334155';"));
  wp_ln(cl, F("row.innerHTML='<td style=\"padding:7px 6px\"><b>#'+n.i+'</b></td><td><span class=\"badge\" style=\"background:'+(n.b==2?'#0284c7':'#4f46e5')+';color:#fff\">RS485-'+(n.b==2?'2 (U3)':'1 (U2)')+'</span></td><td id=\"st_'+n.i+'\"></td><td id=\"tp_'+n.i+'\"></td><td id=\"ql_'+n.i+'\"></td><td id=\"bl_'+n.i+'\"></td><td id=\"lm_'+n.i+'\"></td><td id=\"ls_'+n.i+'\"></td><td id=\"pk_'+n.i+'\">0</td><td id=\"act_'+n.i+'\"></td>';"));
  wp_ln(cl, F("}"));
  wp_ln(cl, F("var st=document.getElementById('st_'+n.i);if(st)st.innerHTML=n.o?'<span class=\"badge badge-ok\">ONLINE</span>':'<span class=\"badge badge-err\">OFFLINE</span>';"));
  wp_ln(cl, F("var tp=document.getElementById('tp_'+n.i);if(tp){if(n.t!==null&&n.t!==undefined)tp.innerHTML='<span class=\"badge\" style=\"background:#0284c7;color:#fff\">'+n.t.toFixed(1)+' &deg;C</span>';else tp.innerHTML='<span style=\"color:#64748b\">-</span>';}"));
  wp_ln(cl, F("var ql=document.getElementById('ql_'+n.i);if(ql){var qh='';if(!n.o)qh='<span class=\"badge badge-err\">0% (Off)</span>';else if(n.q>=90)qh='<span class=\"badge badge-ok\" title=\"Ottimo\">'+n.q+'% ('+n.a+' tent.)</span>';else if(n.q>=70)qh='<span class=\"badge\" style=\"background:#15803d;color:#fff\">'+n.q+'% ('+n.a+' tent.)</span>';else if(n.q>=50)qh='<span class=\"badge\" style=\"background:#ca8a04;color:#000\">'+n.q+'% ('+n.a+' tent.)</span>';else qh='<span class=\"badge badge-err\">'+n.q+'% ('+n.a+' tent.)</span>';if(n.f&&n.f>0)qh+=' <span style=\"color:#f59e0b;font-size:11px\" title=\"Fallimenti storici: '+n.f+'\">('+n.f+' err)</span>';ql.innerHTML=qh;}"));
  wp_ln(cl, F("var ble=document.getElementById('bl_'+n.i);if(ble)ble.innerHTML='<span class=\"badge\" style=\"background:#475569;color:#fff\">'+(n.bl||12)+'%</span>';"));
  wp_ln(cl, F("var lme=document.getElementById('lm_'+n.i);if(lme)lme.innerHTML=getModeLabel(n.lm);"));
  wp_ln(cl, F("var acte=document.getElementById('act_'+n.i);if(acte)acte.innerHTML='<button class=\"btn\" style=\"padding:2px 8px;font-size:11px;margin:0;background:#0284c7\" onclick=\"selectNodeForConfig('+n.i+','+(n.bl||12)+','+(n.lm!==undefined?n.lm:1)+')\">⚙️</button>';"));
  wp_ln(cl, F("var lse=document.getElementById('ls_'+n.i);if(lse){if(n.l!==undefined&&n.l<900000){if(n.l<60)lse.innerText=n.l+'s fa';else if(n.l<3600)lse.innerText=Math.floor(n.l/60)+'m '+(n.l%60)+'s fa';else lse.innerText=Math.floor(n.l/3600)+'h fa';}else{lse.innerText='-';}}"));
  wp_ln(cl, F("var pk=document.getElementById('pk_'+n.i);if(pk)pk.innerText=n.r;"));
  wp_ln(cl, F("}"));
  wp_ln(cl, F("}else{var old_r=document.getElementById('tr_n_'+n.i);if(old_r)old_r.remove();}"));
  wp_ln(cl, F("});"));
  wp_ln(cl, F("if(!any_s&&tb){var nr=document.getElementById('no_nodes_row');if(!nr&&tb.rows.length<=1){var r=tb.insertRow(-1);r.id='no_nodes_row';r.innerHTML='<td colspan=\"10\" style=\"padding:10px 6px;color:#94a3b8;font-style:italic\">Nessun dispositivo rilevato. Clicca &quot;Scansiona Bus&quot; per avviare la ricerca.</td>';}}"));
  wp_ln(cl,
        F("var eo=document.getElementById('cnt_on');if(eo)eo.innerText=on;"));
  wp_ln(cl,
        F("var ef=document.getElementById('cnt_off');if(ef)ef.innerText=off;"));
  wp_ln(cl, F("var "
              "ei=document.getElementById('cnt_ina');if(ei)ei.innerText=(63-on-"
              "off);"));
  wp_ln(cl, F("}"));
  wp_ln(cl, F("}).catch(function(){});"));
  wp_ln(cl, F("},2000);"));
  wp_ln(cl, F("</script>"));

  wp_ln(cl, F("</div></body></html>"));
  cl.stop();
}
