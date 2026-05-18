#ifndef WEB_UI_H
#define WEB_UI_H

#include <Arduino.h>

const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="fr">
<head>
    <meta charset="UTF-8"><meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>ZIRCON1UM | Setup</title>
    <style>
        body { background: #0f172a; color: #f8fafc; font-family: system-ui, sans-serif; margin: 0; padding: 0; }
        nav { background: #4338ca; padding: 1rem; box-shadow: 0 4px 6px -1px rgba(0,0,0,0.1); display: flex; justify-content: space-between; }
        .badge { padding: 0.2rem 0.6rem; border-radius: 1rem; font-size: 0.7rem; font-weight: bold; }
        .bg-red { background: #ef4444; } .bg-green { background: #10b981; }
        .container { max-width: 800px; margin: 1rem auto; padding: 1rem; display: grid; gap: 1rem; }
        .card { background: #1e293b; padding: 1.2rem; border-radius: 0.5rem; border: 1px solid #334155; }
        h2 { color: #818cf8; font-size: 0.8rem; text-transform: uppercase; margin-top: 0; }
        .grid { display: grid; grid-template-columns: 1fr 1fr; gap: 1rem; }
        label { display: block; font-size: 0.7rem; color: #94a3b8; margin-bottom: 0.25rem; }
        input[type="text"], input[type="password"] { width: 100%; background: #0f172a; border: 1px solid #475569; padding: 0.5rem; color: white; border-radius: 0.3rem; box-sizing: border-box; }
        button { background: #4f46e5; color: white; border: none; padding: 0.7rem; width: 100%; border-radius: 0.3rem; font-weight: bold; cursor: pointer; margin-top: 1rem; }
        #logs { height: 250px; overflow-y: auto; background: #000; color: #4ade80; font-family: monospace; font-size: 0.7rem; padding: 0.5rem; border-radius: 0.3rem; margin-top: 1rem; }
    </style>
</head>
<body>
    <nav>
        <div style="font-weight:bold;">ZIRCON1UM v2.1</div>
        <div id="status" class="badge bg-red">Déconnecté</div>
    </nav>
    <div class="container">
        <div class="card">
            <h2>Statut</h2>
            <div style="font-size: 0.9rem; font-family: monospace;">
                IP: <span id="ip">0.0.0.0</span> | Signal: <span id="rssi">--</span> dBm
            </div>
        </div>
        <div class="card">
            <h2>Configuration WiFi</h2>
            <form action="/save" method="POST">
                <label>SSID</label><input type="text" name="ssid">
                <label>Password</label><input type="password" name="pass">
                <div style="margin-top: 1rem; display: flex; align-items: center;">
                    <input type="checkbox" name="static_ip" id="static_ip">
                    <label for="static_ip" style="margin: 0 0 0 0.5rem;">IP Statique (192.168.1.50)</label>
                </div>
                <button type="submit">Sauvegarder & Redémarrer</button>
            </form>
        </div>
        <div class="card">
            <h2>Console</h2>
            <div id="logs"></div>
        </div>
    </div>
    <script>
        let ws;
        function connect() {
            ws = new WebSocket('ws://' + window.location.hostname + '/ws-logs');
            ws.onopen = () => { document.getElementById('status').className='badge bg-green'; document.getElementById('status').innerText='Connecté'; };
            ws.onclose = () => { document.getElementById('status').className='badge bg-red'; document.getElementById('status').innerText='Déconnecté'; setTimeout(connect, 2000);};
            ws.onmessage = (e) => {
                const l = document.getElementById('logs');
                const div = document.createElement('div'); div.textContent = e.data; l.appendChild(div);
                l.scrollTop = l.scrollHeight;
                if(l.children.length > 100) l.removeChild(l.firstChild);
                if(e.data.includes("IP:")) {
                    const m = e.data.match(/IP: ([0-9\.]+)/);
                    if(m) document.getElementById('ip').innerText = m[1];
                }
                if(e.data.includes("Signal")) {
                    const m = e.data.match(/Signal ([-0-9]+)/);
                    if(m) document.getElementById('rssi').innerText = m[1];
                }
            };
        }
        window.onload = connect;
    </script>
</body>
</html>
)rawliteral";

#endif
