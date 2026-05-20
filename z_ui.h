#ifndef Z_UI_H
#define Z_UI_H

#include <Arduino.h>

const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="fr">
<head>
    <meta charset="UTF-8"><meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>BACnetMSTP2MQTT | Console</title>
    <style>
        :root { 
            --bg: #0a0f14; --card: #141b21; --primary: #f59e0b; --accent: #0ea5e9; 
            --text: #e2e8f0; --muted: #64748b; --border: #334155; --success: #22c55e; --error: #ef4444;
        }
        body { background: var(--bg); color: var(--text); font-family: 'Consolas', 'Monaco', monospace; margin: 0; }
        nav { background: #000; padding: 0.75rem 1rem; border-bottom: 2px solid var(--primary); display: flex; justify-content: space-between; align-items: center; }
        .logo { font-weight: 900; font-size: 0.9rem; color: var(--primary); text-transform: uppercase; }
        .tabs { display: flex; background: #000; border-bottom: 1px solid var(--border); overflow-x: auto; }
        .tab-btn { padding: 0.8rem 1rem; background: #141b21; border: none; color: var(--muted); font-size: 0.7rem; font-weight: bold; cursor: pointer; border-top: 2px solid transparent; }
        .tab-btn.active { background: var(--bg); color: var(--primary); border-top-color: var(--primary); }
        .container { max-width: 1100px; margin: 1rem auto; padding: 0 0.75rem; }
        .tab-content { display: none; }
        .tab-content.active { display: block; }
        .card { background: var(--card); border: 1px solid var(--border); margin-bottom: 0.75rem; }
        .card-header { background: #1e293b; padding: 0.5rem 0.75rem; color: var(--accent); font-size: 0.65rem; font-weight: bold; }
        .card-body { padding: 1rem; }
        .grid-stats { display: grid; grid-template-columns: repeat(auto-fit, minmax(150px, 1fr)); gap: 1px; background: var(--border); border: 1px solid var(--border); }
        .stat-item { background: var(--card); padding: 0.6rem; }
        .stat-label { font-size: 0.55rem; color: var(--muted); }
        .stat-value { font-size: 0.85rem; font-weight: bold; color: var(--success); }
        table { width: 100%; border-collapse: collapse; font-size: 0.75rem; }
        th { text-align: left; color: var(--muted); border-bottom: 1px solid var(--border); padding: 0.5rem; }
        td { padding: 0.5rem; border-bottom: 1px solid #1e293b; }
        #console-out { background: #05070a; color: #4ade80; height: 350px; overflow-y: auto; padding: 0.5rem; font-size: 0.65rem; border: 1px solid #1e293b; line-height: 1.3; }
        .btn-cmd { background: transparent; border: 1px solid var(--primary); color: var(--primary); padding: 0.75rem; font-weight: bold; cursor: pointer; width: 100%; }
        input { background: #000; border: 1px solid var(--border); padding: 0.6rem; color: var(--success); font-family: monospace; width: 100%; box-sizing: border-box; }
        label { display: block; font-size: 0.6rem; color: var(--muted); margin-bottom: 0.2rem; margin-top: 0.5rem; }
    </style>
</head>
<body>
    <nav><div class="logo">BACnetMSTP2MQTT <span id="v-tag">...</span></div><div id="status-tag" class="badge">Checking...</div></nav>
    <div class="tabs">
        <button class="tab-btn active" onclick="openTab(event, 't-dash')">Dashboard</button>
        <button class="tab-btn" onclick="openTab(event, 't-bac')">BACnet</button>
        <button class="tab-btn" onclick="openTab(event, 't-conf')">Configuration</button>
    </div>
    <div class="container">
        <div id="t-dash" class="tab-content active">
            <div class="grid-stats">
                <div class="stat-item"><div class="stat-label">WiFi</div><div id="s-rssi" class="stat-value">--</div></div>
                <div class="stat-item"><div class="stat-label">Local IP</div><div id="s-ip" class="stat-value">0.0.0.0</div></div>
                <div class="stat-item"><div class="stat-label">Tokens</div><div id="s-tokens" class="stat-value">0</div></div>
                <div class="stat-item"><div class="stat-label">MQTT</div><div id="s-mqtt" class="stat-value">OFFLINE</div></div>
            </div>
            <div class="card" style="margin-top: 0.75rem;"><div class="card-header">Kernel Stream</div><div id="console-out"></div></div>
        </div>
        <div id="t-bac" class="tab-content">
            <div class="card"><div class="card-header">BACnet Objects</div><div class="card-body" id="cache-list"></div></div>
        </div>
        <div id="t-conf" class="tab-content">
            <div class="card"><div class="card-body">
                <form id="f-conf">
                    <label>WiFi SSID</label><input type="text" name="ssid" id="in-ssid">
                    <label>WiFi Pass</label><input type="password" name="pass" id="in-pass">
                    <label>Local MAC</label><input type="number" name="mac" id="in-mac">
                    <label>Device ID</label><input type="number" name="did" id="in-did">
                    <label>MQTT Host</label><input type="text" name="mqh" id="in-mqh">
                    <button type="button" onclick="doSave()" class="btn-cmd" style="margin-top:1rem">Save & Reboot</button>
                </form>
            </div></div>
        </div>
    </div>
    <script>
        function openTab(e, id) {
            const c=document.getElementsByClassName("tab-content"); for(let i=0; i<c.length; i++)c[i].style.display="none";
            const b=document.getElementsByClassName("tab-btn"); for(let i=0; i<b.length; i++)b[i].classList.remove("active");
            document.getElementById(id).style.display="block"; e.currentTarget.classList.add("active");
            if(id === 't-bac') refreshCache();
        }
        function doSave() {
            fetch('/save', {method:'POST', body:new FormData(document.getElementById('f-conf'))})
            .then(r=>r.text()).then(t=>{alert(t); location.reload();});
        }
        function refreshCache() {
            fetch('/api/objects').then(r=>r.json()).then(data=>{
                const container = document.getElementById('cache-list');
                let html = '<table><tr><th>Type</th><th>Instance</th><th>Value</th></tr>';
                data.forEach(o => {
                    html += `<tr><td>${o.type}</td><td>${o.inst}</td><td style="color:var(--success); font-weight:bold">${o.val.toFixed(2)}</td></tr>`;
                });
                html += '</table>';
                container.innerHTML = html;
            });
        }
        function updateStatus() {
            fetch('/api/status').then(r=>r.json()).then(d=>{
                document.getElementById('s-rssi').innerText=d.rssi + " dBm";
                document.getElementById('s-ip').innerText=d.ip;
                document.getElementById('s-tokens').innerText=d.mstp_t;
                document.getElementById('s-mqtt').innerText=d.mqtt?"ONLINE":"OFFLINE";
                document.getElementById('s-mqtt').style.color=d.mqtt?"var(--success)":"var(--error)";
            });
        }
        let ws = new WebSocket(`ws://${window.location.host}/ws-logs`);
        ws.onmessage = (e) => {
            const c = document.getElementById('console-out');
            const line = document.createElement('div'); line.textContent = e.data;
            c.appendChild(line); c.scrollTop = c.scrollHeight;
            if(c.childNodes.length > 500) c.removeChild(c.firstChild);
        };
        setInterval(updateStatus, 3000); updateStatus();
    </script>
</body>
</html>
)rawliteral";
#endif
