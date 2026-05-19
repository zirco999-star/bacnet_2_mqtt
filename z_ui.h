#ifndef Z_UI_H
#define Z_UI_H

#include <Arduino.h>
#include "z_config.h"

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
        nav { background: #000; padding: 0.75rem 1rem; border-bottom: 2px solid var(--primary); display: flex; justify-content: space-between; align-items: center; position: sticky; top: 0; z-index: 100; }
        .logo { font-weight: 900; font-size: 0.9rem; color: var(--primary); text-transform: uppercase; letter-spacing: 0.05em; }
        .logo span { color: var(--muted); font-size: 0.6rem; letter-spacing: 0; text-transform: none; margin-left: 0.3rem; font-weight: 300; display: block; }
        .badge { border: 1px solid currentColor; padding: 0.1rem 0.4rem; font-size: 0.6rem; font-weight: bold; white-space: nowrap; }
        .tabs { display: flex; background: #000; border-bottom: 1px solid var(--border); overflow-x: auto; scrollbar-width: none; }
        .tab-btn { padding: 0.8rem 1rem; background: #141b21; border: none; color: var(--muted); font-size: 0.7rem; font-weight: bold; cursor: pointer; text-transform: uppercase; border-top: 2px solid transparent; transition: all 0.1s; white-space: nowrap; flex-shrink: 0; }
        .tab-btn.active { background: var(--bg); color: var(--primary); border-top-color: var(--primary); }
        .container { max-width: 1100px; margin: 1rem auto; padding: 0 0.75rem; }
        .tab-content { display: none; }
        .tab-content.active { display: block; }
        .card { background: var(--card); border: 1px solid var(--border); margin-bottom: 0.75rem; box-shadow: inset 0 0 10px rgba(0,0,0,0.5); }
        .card-header { background: #1e293b; padding: 0.5rem 0.75rem; border-bottom: 1px solid var(--border); color: var(--accent); font-size: 0.65rem; font-weight: bold; text-transform: uppercase; display: flex; justify-content: space-between; }
        .card-body { padding: 1rem; }
        .grid-stats { display: grid; grid-template-columns: 1fr 1fr; gap: 1px; background: var(--border); border: 1px solid var(--border); }
        @media(min-width: 768px) { .grid-stats { grid-template-columns: repeat(4, 1fr); } }
        .stat-item { background: var(--card); padding: 0.6rem; }
        .stat-label { font-size: 0.55rem; color: var(--muted); margin-bottom: 0.2rem; }
        .stat-value { font-size: 0.85rem; font-weight: bold; color: var(--success); overflow: hidden; text-overflow: ellipsis; }
        form { display: grid; grid-template-columns: 1fr; gap: 0.75rem; }
        @media(min-width: 640px) { form { grid-template-columns: 1fr 1fr; } }
        .full-w { grid-column: 1 / -1; }
        label { display: block; font-size: 0.6rem; color: var(--muted); margin-bottom: 0.2rem; text-transform: uppercase; }
        input, select { background: #000; border: 1px solid var(--border); padding: 0.6rem; color: var(--success); width: 100%; box-sizing: border-box; }
        button.btn-cmd { background: transparent; border: 1px solid var(--primary); color: var(--primary); padding: 0.75rem; font-weight: bold; cursor: pointer; text-transform: uppercase; width: 100%; }
        button.btn-cmd:hover { background: var(--primary); color: #000; }
        #console-out { background: #05070a; color: #4ade80; height: 350px; overflow-y: auto; padding: 0.5rem; font-size: 0.65rem; border: 1px solid #1e293b; }
    </style>
</head>
<body>
    <nav>
        <div class="logo">BACnetMSTP2MQTT <span id="v-tag">...</span> <span>by Z1rc0n1um</span></div>
        <div id="status-tag" class="badge" style="color:var(--error)">Disconnected</div>
    </nav>
    <div class="tabs">
        <button class="tab-btn active" onclick="openTab(event, 't-dash')">Dashboard</button>
        <button class="tab-btn" onclick="openTab(event, 't-wifi')">Network</button>
        <button class="tab-btn" onclick="openTab(event, 't-bac')">BACnet</button>
        <button class="tab-btn" onclick="openTab(event, 't-mq')">MQTT</button>
        <button class="tab-btn" onclick="openTab(event, 't-sys')">System</button>
    </div>
    <div class="container">
        <div id="t-dash" class="tab-content active">
            <div class="grid-stats">
                <div class="stat-item"><div class="stat-label">WiFi Signal</div><div id="s-rssi" class="stat-value">-- dBm</div></div>
                <div class="stat-item"><div class="stat-label">Local IP</div><div id="s-ip" class="stat-value">0.0.0.0</div></div>
                <div class="stat-item"><div class="stat-label">BACnet MAC</div><div id="s-mac" class="stat-value">--</div></div>
                <div class="stat-item"><div class="stat-label">MQTT Node</div><div id="s-mq" class="stat-value">OFFLINE</div></div>
            </div>
            <div class="card" style="margin-top: 0.75rem;">
                <div class="card-header"><span>Kernel Stream</span></div>
                <div id="console-out"></div>
            </div>
        </div>
        <!-- (Le reste du HTML simplifié pour l'exemple, à conserver complet en réel) -->
        <div id="t-wifi" class="tab-content">
             <div class="card"><div class="card-header">WiFi Interface</div><div class="card-body">
                <form id="f-wifi">
                    <div class="form-group"><label>SSID</label><input type="text" name="ssid" id="in-ssid"></div>
                    <div class="form-group"><label>Password</label><input type="password" name="pass" id="in-pass"></div>
                    <button type="button" onclick="doSave('f-wifi')" class="btn-cmd full-w">Commit Network Changes</button>
                </form>
             </div></div>
        </div>
        <div id="t-sys" class="tab-content">
            <div class="card"><div class="card-header">Firmware Update</div><div class="card-body">
                <input type="file" id="fw-file">
                <button type="button" onclick="doUpdate()" class="btn-cmd full-w">Flash Firmware (OTA)</button>
            </div></div>
            <button class="btn-cmd" style="color:red; border-color:red" onclick="fetch('/reboot')">Master Reboot</button>
        </div>
    </div>
    <script>
        function openTab(evt, id) {
            const c=document.getElementsByClassName("tab-content"); for(let i=0; i<c.length; i++)c[i].style.display="none";
            const b=document.getElementsByClassName("tab-btn"); for(let i=0; i<b.length; i++)b[i].classList.remove("active");
            document.getElementById(id).style.display="block"; evt.currentTarget.classList.add("active");
        }
        function doSave(fid) {
            fetch('/save', {method:'POST', body:new FormData(document.getElementById(fid))})
            .then(r=>r.text()).then(t=>alert(t));
        }
        function updateStatus() {
            fetch('/api/status').then(r=>r.json()).then(d=>{
                document.getElementById('v-tag').innerText=d.ver;
                document.title = "BACnetMSTP2MQTT " + d.ver;
                document.getElementById('s-rssi').innerText=d.rssi+" dBm";
                document.getElementById('s-ip').innerText=d.ip;
                document.getElementById('s-mac').innerText=d.mac_id;
                document.getElementById('status-tag').style.color="var(--success)"; document.getElementById('status-tag').innerText="Connected";
            }).catch(()=> { document.getElementById('status-tag').style.color="var(--error)"; });
        }
        setInterval(updateStatus, 3000);
        updateStatus();
    </script>
</body>
</html>
)rawliteral";
#endif
