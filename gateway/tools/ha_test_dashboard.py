import json
import time
import threading
from http.server import HTTPServer, BaseHTTPRequestHandler
import paho.mqtt.client as mqtt

MQTT_BROKER = "192.168.1.51"
MQTT_PORT = 1883
HTTP_PORT = 8123

# Stato globale in memoria sincronizzato via MQTT
state = {
    "gw": {
        "status": "online",
        "temp": "30.1",
        "ip": "192.168.1.66",
        "rx": 0,
        "tx": 0,
        "bus_health": 100,
        "time": "--:--:--",
        "night_mode": "OFF"
    },
    "nodes": {
        "6": {
            "name": "Tastiera Nodo 6",
            "status": "online",
            "temp": "25.0",
            "quality": 100,
            "night_mode": "OFF",
            "clean_mode": "OFF",
            "leds": {str(i): "OFF" for i in range(1, 9)},
            "led_all": "OFF",
            "last_btn": {"num": None, "type": None, "time": 0}
        },
        "24": {
            "name": "Tastiera Nodo 24",
            "status": "online",
            "temp": "N.D.",
            "quality": 100,
            "night_mode": "OFF",
            "clean_mode": "OFF",
            "leds": {str(i): "OFF" for i in range(1, 9)},
            "led_all": "OFF",
            "last_btn": {"num": None, "type": None, "time": 0}
        }
    },
    "events": []
}

mqtt_client = None

def add_event(msg_type, text):
    t_str = time.strftime("%H:%M:%S")
    state["events"].insert(0, {"time": t_str, "type": msg_type, "text": text})
    if len(state["events"]) > 30:
        state["events"].pop()

def on_mqtt_connect(client, userdata, flags, rc, properties=None):
    add_event("system", f"Connesso al broker MQTT {MQTT_BROKER}:{MQTT_PORT}")
    client.subscribe("pjon_gw_aaf2/#")
    client.subscribe("homeassistant/#")

def on_mqtt_message(client, userdata, msg):
    topic = msg.topic
    try:
        payload = msg.payload.decode('utf-8', errors='ignore').strip()
    except Exception:
        payload = ""

    # 1. Stato Gateway
    if topic == "pjon_gw_aaf2/status":
        state["gw"]["status"] = payload
    elif topic == "pjon_gw_aaf2/temperature":
        state["gw"]["temp"] = payload
    elif topic == "pjon_gw_aaf2/bus_health":
        try:
            state["gw"]["bus_health"] = int(payload)
        except ValueError:
            pass
    elif topic == "pjon_gw_aaf2/time":
        state["gw"]["time"] = payload
    elif topic == "pjon_gw_aaf2/night_mode/state":
        state["gw"]["night_mode"] = payload
        add_event("system", f"Master Modalita Notte: {payload}")
    elif topic == "pjon_gw_aaf2/telemetry":
        try:
            d = json.loads(payload)
            state["gw"]["ip"] = d.get("ip", state["gw"]["ip"])
            state["gw"]["rx"] = d.get("pjon_rx", state["gw"]["rx"])
            state["gw"]["tx"] = d.get("pjon_tx", state["gw"]["tx"])
            if "health" in d:
                state["gw"]["bus_health"] = int(d["health"])
        except Exception:
            pass

    # 2. Eventi Nodi: pjon_gw_aaf2/node/<id>/...
    elif topic.startswith("pjon_gw_aaf2/node/"):
        parts = topic.split("/")
        if len(parts) >= 4:
            nid = parts[2]
            if nid not in state["nodes"]:
                state["nodes"][nid] = {
                    "name": f"Tastiera Nodo {nid}",
                    "status": "online",
                    "temp": "N.D.",
                    "quality": 100,
                    "night_mode": "OFF",
                    "clean_mode": "OFF",
                    "leds": {str(i): "OFF" for i in range(1, 9)},
                    "led_all": "OFF",
                    "last_btn": {"num": None, "type": None, "time": 0}
                }
            n = state["nodes"][nid]

            # Status (online/offline)
            if parts[3] == "status":
                n["status"] = payload
                add_event("status", f"Nodo {nid}: stato {payload.upper()}")

            # Temperature
            elif parts[3] == "temperature":
                n["temp"] = payload
                add_event("temp", f"Nodo {nid}: temperatura {payload} °C")

            # Quality
            elif parts[3] == "quality":
                try:
                    n["quality"] = int(payload)
                except ValueError:
                    pass

            # Modalita Notte & Pulizia
            elif parts[3] == "night_mode" and len(parts) >= 5 and parts[4] == "state":
                n["night_mode"] = payload
                add_event("system", f"Nodo {nid} Modalita Notte: {payload}")

            elif parts[3] == "clean_mode" and len(parts) >= 5 and parts[4] == "state":
                n["clean_mode"] = payload
                add_event("system", f"Nodo {nid} Modalita Pulizia: {payload}")

            # LED State: node/<id>/led/<l>/state
            elif parts[3] == "led" and len(parts) >= 6 and parts[5] == "state":
                led_id = parts[4]
                if led_id == "0":
                    n["led_all"] = payload
                elif led_id in n["leds"]:
                    n["leds"][led_id] = payload
                add_event("led", f"Nodo {nid}: LED {led_id} -> {payload}")

            # Buttons: node/<id>/btn/<num>
            elif parts[3] == "btn":
                try:
                    bnum = int(parts[4])
                    if bnum <= 8:
                        b_idx = bnum
                        b_type = "CLICK"
                    elif bnum <= 16:
                        b_idx = bnum - 8
                        b_type = "LONG PRESS"
                    else:
                        b_idx = bnum - 16
                        b_type = "DOUBLE CLICK"

                    n["last_btn"] = {
                        "num": b_idx,
                        "type": b_type,
                        "time": time.time()
                    }
                    add_event("button", f"🌟 Nodo {nid} - Tasto {b_idx}: {b_type}")
                except ValueError:
                    pass

HTML_PAGE = """<!DOCTYPE html>
<html lang="it">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Home Assistant — Test Dashboard IO16</title>
<style>
:root {
  --bg: #0f172a;
  --card: #1e293b;
  --card-border: #334155;
  --accent: #0284c7;
  --text: #f8fafc;
  --text-muted: #94a3b8;
  --btn-on: #22c55e;
  --btn-long: #f59e0b;
  --btn-dbl: #a855f7;
}
* { box-sizing: border-box; }
body {
  font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif;
  background: var(--bg);
  color: var(--text);
  margin: 0;
  padding: 16px;
}
.header {
  max-width: 1100px;
  margin: 0 auto 20px;
  display: flex;
  justify-content: space-between;
  align-items: center;
  border-bottom: 1px solid var(--card-border);
  padding-bottom: 12px;
}
.header h1 {
  font-size: 20px;
  margin: 0;
  display: flex;
  align-items: center;
  gap: 10px;
  color: #38bdf8;
}
.header-badge {
  background: #0369a1;
  color: #fff;
  font-size: 11px;
  padding: 3px 8px;
  border-radius: 12px;
  font-weight: bold;
}
.grid {
  max-width: 1100px;
  margin: 0 auto;
  display: grid;
  grid-template-columns: repeat(auto-fit, minmax(340px, 1fr));
  gap: 18px;
}
.card {
  background: var(--card);
  border: 1px solid var(--card-border);
  border-radius: 12px;
  padding: 18px;
  box-shadow: 0 4px 12px rgba(0,0,0,0.3);
}
.card-header {
  display: flex;
  justify-content: space-between;
  align-items: center;
  margin-bottom: 14px;
}
.card-title {
  font-size: 16px;
  font-weight: bold;
  color: #f1f5f9;
  display: flex;
  align-items: center;
  gap: 8px;
}
.badge-online { background: #16a34a; color: #fff; padding: 2px 8px; border-radius: 10px; font-size: 11px; font-weight: bold; }
.badge-offline { background: #dc2626; color: #fff; padding: 2px 8px; border-radius: 10px; font-size: 11px; font-weight: bold; }
.stats-row {
  display: flex;
  gap: 12px;
  margin-bottom: 16px;
  font-size: 13px;
}
.stat-pill {
  background: #0f172a;
  padding: 6px 10px;
  border-radius: 6px;
  border: 1px solid var(--card-border);
  flex: 1;
  text-align: center;
}
.stat-pill b { display: block; font-size: 15px; color: #38bdf8; margin-top: 2px; }

.section-label {
  font-size: 12px;
  font-weight: bold;
  text-transform: uppercase;
  letter-spacing: 0.5px;
  color: var(--text-muted);
  margin: 12px 0 8px;
}
.buttons-grid {
  display: grid;
  grid-template-columns: repeat(4, 1fr);
  gap: 8px;
  margin-bottom: 14px;
}
.btn-tile {
  background: #0f172a;
  border: 1px solid var(--card-border);
  border-radius: 8px;
  padding: 10px 4px;
  text-align: center;
  font-size: 12px;
  font-weight: bold;
  transition: all 0.15s ease;
  user-select: none;
}
.btn-tile small {
  display: block;
  font-size: 9px;
  font-weight: normal;
  color: var(--text-muted);
  margin-top: 2px;
}
.btn-tile.active-click {
  background: #2563eb !important;
  color: #fff !important;
  border-color: #60a5fa !important;
  transform: scale(0.96);
  box-shadow: 0 0 12px rgba(37,99,235,0.7);
}
.btn-tile.active-long {
  background: #d97706 !important;
  color: #fff !important;
  border-color: #fbbf24 !important;
  transform: scale(0.96);
  box-shadow: 0 0 12px rgba(217,119,6,0.7);
}
.btn-tile.active-dbl {
  background: #9333ea !important;
  color: #fff !important;
  border-color: #c084fc !important;
  transform: scale(0.96);
  box-shadow: 0 0 12px rgba(147,51,234,0.7);
}

.leds-grid {
  display: grid;
  grid-template-columns: repeat(4, 1fr);
  gap: 8px;
}
.led-toggle {
  background: #0f172a;
  border: 1px solid var(--card-border);
  border-radius: 8px;
  padding: 8px 4px;
  text-align: center;
  font-size: 11px;
  cursor: pointer;
  transition: all 0.2s ease;
}
.led-toggle:hover { opacity: 0.9; }
.led-toggle.on {
  background: #15803d;
  border-color: #22c55e;
  color: #fff;
  font-weight: bold;
  box-shadow: 0 0 10px rgba(34,197,94,0.5);
}
.led-dot {
  width: 10px;
  height: 10px;
  border-radius: 50%;
  display: inline-block;
  background: #475569;
  margin-bottom: 4px;
}
.led-toggle.on .led-dot {
  background: #4ade80;
  box-shadow: 0 0 6px #4ade80;
}
.events-card {
  max-width: 1100px;
  margin: 20px auto 0;
  background: var(--card);
  border: 1px solid var(--card-border);
  border-radius: 12px;
  padding: 16px;
}
.events-box {
  background: #0f172a;
  border: 1px solid var(--card-border);
  border-radius: 8px;
  padding: 8px 12px;
  height: 160px;
  overflow-y: auto;
  font-family: monospace;
  font-size: 12px;
}
.ev-row { padding: 3px 0; border-bottom: 1px solid #1e293b; display: flex; gap: 10px; }
.ev-time { color: var(--text-muted); }
.ev-btn { color: #facc15; font-weight: bold; }
.ev-led { color: #4ade80; }
.ev-temp { color: #38bdf8; }
.ev-stat { color: #f472b6; }
</style>
</head>
<body>

<div class="header">
  <h1>🏠 Home Assistant — Dashboard Interattiva IO16 <span class="header-badge">LIVE TESTER</span></h1>
  <div><span id="ha_conn" class="badge-online">CONNESSO A MQTT (192.168.1.51)</span></div>
</div>

<div class="grid" id="devices_grid">
  <!-- Schede generate dinamicamente -->
</div>

<div class="events-card">
  <div style="display:flex;justify-content:space-between;align-items:center;margin-bottom:8px">
    <div style="font-size:14px;font-weight:bold;color:#f1f5f9">📡 Registro Eventi in Tempo Reale (Pulsanti, LED, Sensori)</div>
    <span style="font-size:11px;color:var(--text-muted)">Premi i tasti sulla tastiera fisica per vederli scattare!</span>
  </div>
  <div class="events-box" id="events_box"></div>
</div>

<script>
function toggleLed(nodeId, ledId, curState) {
  var nextState = (curState === 'ON') ? 'OFF' : 'ON';
  fetch('/api/set_led?node=' + nodeId + '&led=' + ledId + '&state=' + nextState, { method: 'POST' });
}

function toggleNightMode(nodeId, curState) {
  var nextState = (curState === 'ON') ? 'OFF' : 'ON';
  fetch('/api/set_night_mode?node=' + nodeId + '&state=' + nextState, { method: 'POST' });
}

function toggleCleanMode(nodeId, curState) {
  var nextState = (curState === 'ON') ? 'OFF' : 'ON';
  fetch('/api/set_clean_mode?node=' + nodeId + '&state=' + nextState, { method: 'POST' });
}

function updateUI() {
  fetch('/api/state')
    .then(r => r.json())
    .then(data => {
      var grid = document.getElementById('devices_grid');
      var html = '';

      // Scheda Gateway
      html += `
        <div class="card">
          <div class="card-header">
            <div class="card-title">🌐 Gateway MQTT ATmega128</div>
            <span class="${data.gw.status === 'online' ? 'badge-online' : 'badge-offline'}">${data.gw.status.toUpperCase()}</span>
          </div>
          <div class="stats-row">
            <div class="stat-pill">IP Gateway<b>${data.gw.ip}</b></div>
            <div class="stat-pill">Salute Bus RS485<b style="color:${data.gw.bus_health>=95?'#22c55e':(data.gw.bus_health>=80?'#eab308':'#ef4444')}">${data.gw.bus_health}%</b></div>
            <div class="stat-pill">Data/Ora NTP<b>${data.gw.time}</b></div>
          </div>
          <div class="stats-row" style="margin-top:6px">
            <div class="stat-pill">Temp Gateway<b>${data.gw.temp} °C</b></div>
            <div class="stat-pill">Pacchetti RX/TX<b>${data.gw.rx} / ${data.gw.tx}</b></div>
            <div class="stat-pill" style="cursor:pointer" onclick="toggleNightMode('all', '${data.gw.night_mode}')">Notte Tutte<b>${data.gw.night_mode==='ON'?'🌙 ATTIVA':'☀️ DISAT'}</b></div>
          </div>
          <div style="color:var(--text-muted);font-size:12px;margin-top:10px">
            Broker: <code>192.168.1.51:1883</code> &bull; Client: <code>pjon_gw_aaf2</code>
          </div>
        </div>
      `;

      // Schede Nodi
      var now = Date.now() / 1000;
      for (var nid in data.nodes) {
        var n = data.nodes[nid];
        var isOnline = (n.status === 'online');

        html += `
          <div class="card">
            <div class="card-header">
              <div class="card-title">⌨️ ${n.name} (Bus RS485)</div>
              <span class="${isOnline ? 'badge-online' : 'badge-offline'}">${n.status.toUpperCase()}</span>
            </div>
            <div class="stats-row">
              <div class="stat-pill">Temperatura<b>${n.temp !== 'N.D.' ? n.temp + ' °C' : 'N.D.'}</b></div>
              <div class="stat-pill">Qualità Segnale<b>${n.quality}%</b></div>
              <div class="stat-pill">Ultimo Tasto<b>${n.last_btn.num ? 'P' + n.last_btn.num : '-'}</b></div>
            </div>

            <div style="display:flex;gap:6px;margin:10px 0">
              <button onclick="toggleNightMode('${nid}', '${n.night_mode}')" style="flex:1;background:${n.night_mode==='ON'?'#4f46e5':'#0f172a'};color:#fff;border:1px solid var(--card-border);border-radius:6px;padding:6px 8px;font-size:11px;cursor:pointer">
                🌙 Notte: <b>${n.night_mode}</b>
              </button>
              <button onclick="toggleCleanMode('${nid}', '${n.clean_mode}')" style="flex:1;background:${n.clean_mode==='ON'?'#d97706':'#0f172a'};color:#fff;border:1px solid var(--card-border);border-radius:6px;padding:6px 8px;font-size:11px;cursor:pointer">
                🧹 Pulizia (60s): <b>${n.clean_mode}</b>
              </button>
            </div>

            <div class="section-label">Pulsanti Fisici (Rilevazione Click in Tempo Reale)</div>
            <div class="buttons-grid">
        `;

        for (var b = 1; b <= 8; b++) {
          var actClass = '';
          var subLabel = 'Inattivo';
          if (n.last_btn.num === b && (now - n.last_btn.time < 1.2)) {
            if (n.last_btn.type === 'CLICK') { actClass = 'active-click'; subLabel = 'CLICK'; }
            else if (n.last_btn.type === 'LONG PRESS') { actClass = 'active-long'; subLabel = 'LUNGO'; }
            else if (n.last_btn.type === 'DOUBLE CLICK') { actClass = 'active-dbl'; subLabel = 'DOPPIO'; }
          }
          html += `
            <div class="btn-tile ${actClass}">
              P${b}
              <small>${subLabel}</small>
            </div>
          `;
        }

        html += `
            </div>

            <div class="section-label" style="display:flex;justify-content:space-between;align-items:center">
              <span>LED Tastiera (Clicca per Accendere/Spegnere)</span>
              <button onclick="toggleLed('${nid}', 0, '${n.led_all}')" style="background:#0284c7;color:#fff;border:none;border-radius:4px;padding:3px 8px;font-size:10px;cursor:pointer">TUTTI ON/OFF</button>
            </div>
            <div class="leds-grid">
        `;

        for (var l = 1; l <= 8; l++) {
          var isLedOn = (n.leds['' + l] === 'ON');
          html += `
            <div class="led-toggle ${isLedOn ? 'on' : ''}" onclick="toggleLed('${nid}', ${l}, '${isLedOn ? 'ON' : 'OFF'}')">
              <div class="led-dot"></div><br>
              LED ${l}
            </div>
          `;
        }

        html += `
            </div>
          </div>
        `;
      }

      grid.innerHTML = html;

      // Eventi
      var evBox = document.getElementById('events_box');
      var evHtml = '';
      data.events.forEach(function(ev) {
        var cls = 'ev-stat';
        if (ev.type === 'button') cls = 'ev-btn';
        else if (ev.type === 'led') cls = 'ev-led';
        else if (ev.type === 'temp') cls = 'ev-temp';
        evHtml += `<div class="ev-row"><span class="ev-time">[${ev.time}]</span> <span class="${cls}">${ev.text}</span></div>`;
      });
      evBox.innerHTML = evHtml;
    })
    .catch(() => {});
}

setInterval(updateUI, 250);
updateUI();
</script>

</body>
</html>
"""

class RequestHandler(BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path == "/" or self.path == "/index.html":
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.end_headers()
            self.wfile.write(HTML_PAGE.encode("utf-8"))
        elif self.path == "/api/state":
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            self.wfile.write(json.dumps(state).encode("utf-8"))
        else:
            self.send_response(404)
            self.end_headers()

    def do_POST(self):
        if self.path.startswith("/api/set_led"):
            import urllib.parse
            query = urllib.parse.urlparse(self.path).query
            params = urllib.parse.parse_qs(query)
            node_id = params.get("node", ["6"])[0]
            led_id = params.get("led", ["1"])[0]
            st = params.get("state", ["ON"])[0]

            topic = f"pjon_gw_aaf2/node/{node_id}/led/{led_id}/set"
            if mqtt_client:
                mqtt_client.publish(topic, st)
                add_event("led", f"Inviato comando: {topic} -> {st}")

            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            self.wfile.write(b'{"status":"ok"}')
        elif self.path.startswith("/api/set_night_mode"):
            import urllib.parse
            query = urllib.parse.urlparse(self.path).query
            params = urllib.parse.parse_qs(query)
            node_id = params.get("node", ["all"])[0]
            st = params.get("state", ["ON"])[0]
            if node_id == "all" or node_id == "0":
                topic = "pjon_gw_aaf2/night_mode/set"
            else:
                topic = f"pjon_gw_aaf2/node/{node_id}/night_mode/set"
            if mqtt_client:
                mqtt_client.publish(topic, st)
                add_event("system", f"Inviato comando: {topic} -> {st}")
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            self.wfile.write(b'{"status":"ok"}')
        elif self.path.startswith("/api/set_clean_mode"):
            import urllib.parse
            query = urllib.parse.urlparse(self.path).query
            params = urllib.parse.parse_qs(query)
            node_id = params.get("node", ["6"])[0]
            st = params.get("state", ["ON"])[0]
            topic = f"pjon_gw_aaf2/node/{node_id}/clean_mode/set"
            if mqtt_client:
                mqtt_client.publish(topic, st)
                add_event("system", f"Inviato comando: {topic} -> {st}")
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            self.wfile.write(b'{"status":"ok"}')
        else:
            self.send_response(404)
            self.end_headers()

    def log_message(self, format, *args):
        pass

def start_mqtt():
    global mqtt_client
    mqtt_client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
    mqtt_client.username_pw_set("admin", "admin")
    mqtt_client.on_connect = on_mqtt_connect
    mqtt_client.on_message = on_mqtt_message
    try:
        mqtt_client.connect(MQTT_BROKER, MQTT_PORT, 60)
        mqtt_client.loop_forever()
    except Exception as e:
        print(f"MQTT Error: {e}")

if __name__ == "__main__":
    t = threading.Thread(target=start_mqtt, daemon=True)
    t.start()

    server = HTTPServer(("0.0.0.0", HTTP_PORT), RequestHandler)
    print(f"Test Dashboard avviata su http://localhost:{HTTP_PORT}")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass

