#ifndef Z_UI_H
#define Z_UI_H

#include <Arduino.h>
#include "z_config.h"

const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="fr">
<head>
    <meta charset="UTF-8"><meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>BACNET2MQTT - )rawliteral" VERSION_GLOBAL R"rawliteral(</title>
    <style>
        :root { 
            --bg: #09090b; --card: #18181b; --primary: #3b82f6; --accent: #6366f1; 
            --text: #fafafa; --muted: #a1a1aa; --border: #27272a; --success: #22c55e; --error: #ef4444; --warning: #f59e0b;
            --glass: rgba(24, 24, 27, 0.8);
        }
        /* Suppression de Google Fonts pour rapidité en mode AP */
        body { background: var(--bg); color: var(--text); font-family: -apple-system, system-ui, sans-serif; margin: 0; line-height: 1.2; -webkit-tap-highlight-color: transparent; }
        
        nav { background: var(--glass); backdrop-filter: blur(12px); padding: 0.5rem 1rem; border-bottom: 1px solid var(--border); display: flex; justify-content: space-between; align-items: center; position: sticky; top: 0; z-index: 100; }
        .logo-box { display: flex; flex-direction: column; }
        .logo { font-weight: 800; font-size: 0.9rem; letter-spacing: -0.03em; text-decoration: none; }
        .logo .b { color: var(--primary); } .logo .n { color: var(--success); } .logo .m { color: var(--accent); }
        .credits { font-size: 0.55rem; color: var(--muted); font-weight: 500; }
        .credits a { color: var(--primary); text-decoration: none; font-weight: 700; }

        .status-group { display: flex; gap: 0.4rem; }
        .badge { padding: 0.2rem 0.5rem; border-radius: 4px; font-size: 0.6rem; font-weight: 800; background: #27272a; color: var(--muted); text-transform: uppercase; }
        .badge-ok { background: #052e16; color: #4ade80; border: 1px solid #14532d; }
        .badge-fail { background: #450a0a; color: #f87171; border: 1px solid #7f1d1d; }

        .tabs { display: flex; gap: 0.2rem; background: #000; padding: 0.3rem 0.5rem; border-bottom: 1px solid var(--border); overflow-x: auto; }
        .tab-btn { padding: 0.4rem 0.6rem; background: transparent; border: none; color: var(--muted); font-size: 0.65rem; font-weight: 700; cursor: pointer; border-radius: 4px; white-space: nowrap; text-transform: uppercase; }
        .tab-btn.active { background: var(--primary); color: #fff; }

        .container { max-width: 1200px; margin: 0.5rem auto; padding: 0 0.4rem; }
        .tab-content { display: none; }
        .tab-content.active { display: block; }

        .grid-stats { display: grid; grid-template-columns: repeat(3, 1fr); gap: 0.4rem; margin-bottom: 0.5rem; }
        @media (max-width: 600px) { .grid-stats { grid-template-columns: 1fr 1fr; } .stat-card:last-child { grid-column: span 2; } }

        .stat-card { background: var(--card); border: 1px solid var(--border); padding: 0.5rem 0.6rem; border-radius: 8px; display: flex; flex-direction: column; min-height: 60px; justify-content: center; }
        .stat-label { font-size: 0.5rem; color: var(--muted); text-transform: uppercase; font-weight: 800; letter-spacing: 0.04em; margin-bottom: 1px; }
        .stat-value { font-size: 0.8rem; font-weight: 700; color: var(--text); font-family: monospace; }
        
        .echo-radar { display: flex; gap: 2px; height: 5px; align-items: center; margin-top: 4px; width: 100%; }
        .echo-segment { flex: 1; height: 100%; background: #27272a; border-radius: 1px; transition: all 0.3s ease; }
        .echo-segment.active { box-shadow: 0 0 3px currentColor; }

        .full-banner { background: linear-gradient(90deg, #1e1e1e, #111); border: 1px solid var(--border); padding: 0.5rem 0.75rem; border-radius: 8px; margin-bottom: 0.5rem; display: flex; justify-content: space-between; align-items: center; }
        .banner-label { font-size: 0.5rem; color: var(--muted); font-weight: 800; text-transform: uppercase; }
        .banner-value { font-size: 0.9rem; font-weight: 800; color: var(--primary); font-family: monospace; }

        .cmd-group { display: grid; grid-template-columns: 1fr 1fr; gap: 0.4rem; margin-bottom: 0.75rem; }
        .btn { padding: 0.5rem; border-radius: 5px; font-weight: 800; cursor: pointer; font-size: 0.65rem; border: none; text-transform: uppercase; transition: filter 0.2s; }
        .btn-p { background: var(--primary); color: #fff; }
        .btn-s { background: #27272a; color: var(--muted); border: 1px solid var(--border); }
        .btn-d { background: #450a0a; color: #f87171; }
        .btn:hover { filter: brightness(1.2); }

        .card { background: var(--card); border: 1px solid var(--border); border-radius: 8px; overflow: hidden; margin-bottom: 0.75rem; }
        .card-header { background: #1e1e1e; padding: 0.5rem 0.6rem; border-bottom: 1px solid var(--border); display: flex; justify-content: space-between; align-items: center; }
        .card-title { font-size: 0.65rem; font-weight: 800; color: var(--muted); text-transform: uppercase; }
        
        /* Controller & Table Styles */
        .controller-block { margin-bottom: 1rem; border: 1px solid var(--border); border-radius: 8px; background: #0c0c0e; overflow: hidden; }
        .controller-header { background: #1a1a1c; padding: 0.6rem 0.75rem; display: flex; justify-content: space-between; align-items: center; border-bottom: 1px solid var(--border); }
        .header-actions { display: flex; gap: 0.6rem; align-items: center; }
        .btn-lg { padding: 0.5rem 0.8rem; font-size: 0.7rem; }
        
        table { width: 100%; border-collapse: collapse; font-size: 0.7rem; }
        th { text-align: left; color: var(--muted); background: #141416; padding: 0.5rem; font-weight: 700; }
        td { padding: 0.4rem 0.5rem; border-bottom: 1px solid var(--border); vertical-align: middle; }
        
        .tag-obj { font-family: monospace; background: #27272a; padding: 1px 3px; border-radius: 3px; font-weight: 700; color: var(--primary); font-size: 0.6rem; }
        .in-edit { background: transparent; border: 1px solid transparent; color: var(--text); font-size: 0.7rem; width: 100%; padding: 2px; border-radius: 3px; }
        .in-edit:focus { background: #000; border-color: var(--primary); outline: none; }

        #console-out { background: #000; color: #a1a1aa; height: 220px; overflow-y: auto; padding: 0.5rem; font-size: 0.65rem; font-family: monospace; }
        
        .switch { position: relative; display: inline-block; width: 28px; height: 16px; }
        .switch input { opacity: 0; width: 0; height: 0; }
        .slider { position: absolute; cursor: pointer; top: 0; left: 0; right: 0; bottom: 0; background-color: #27272a; transition: .3s; border-radius: 16px; }
        .slider:before { position: absolute; content: ""; height: 12px; width: 12px; left: 2px; bottom: 2px; background-color: white; transition: .3s; border-radius: 50%; }
        input:checked + .slider { background-color: var(--success); }
        input:checked + .slider:before { transform: translateX(12px); }

        .grid-form { display: grid; grid-template-columns: 1fr 1fr; gap: 0.6rem; padding: 0.6rem; }
        .full-w { grid-column: span 2; }
        input[type="text"], input[type="number"], input[type="password"] { background: #09090b; border: 1px solid var(--border); padding: 0.4rem; color: var(--text); border-radius: 5px; width: 100%; box-sizing: border-box; font-size: 0.75rem; }
        label { display: block; font-size: 0.55rem; font-weight: 800; color: var(--muted); margin-bottom: 0.2rem; margin-top: 0.5rem; text-transform: uppercase; }
    </style>
</head>
<body>
    <nav>
        <div class="logo-box">
            <div style="display:flex; align-items:center; gap:0.5rem">
                <a href="https://github.com/zirco999-star" target="_blank" class="logo"><span class="b">BACNET</span><span class="n">2</span><span class="m">MQTT</span></a>
                <span style="font-size:0.7rem; font-weight:800; color:var(--muted)">- )rawliteral" VERSION_GLOBAL R"rawliteral(</span>
            </div>
            <div class="credits">by <a href="https://github.com/zirco999-star" target="_blank">Z1rc0n1um</a></div>
        </div>
        <div class="status-group">
            <div id="b-tag" class="badge">BACNET</div>
            <div id="m-tag" class="badge">MQTT</div>
        </div>
    </nav>
    <div class="tabs">
        <button class="tab-btn active" onclick="openTab(event, 't-dash')">Dashboard</button>
        <button class="tab-btn" onclick="openTab(event, 't-bac')">BACnet</button>
        <button class="tab-btn" onclick="openTab(event, 't-conf')">Settings</button>
    </div>
    <div class="container">
        <!-- DASHBOARD -->
        <div id="t-dash" class="tab-content active">
            <div class="grid-stats">
                <div class="stat-card">
                    <div class="stat-label">WiFi Radio</div>
                    <div id="s-rssi-val" class="stat-value">-- dBm</div>
                    <div class="echo-radar" id="radar">
                        <div class="echo-segment"></div><div class="echo-segment"></div><div class="echo-segment"></div><div class="echo-segment"></div><div class="echo-segment"></div><div class="echo-segment"></div><div class="echo-segment"></div><div class="echo-segment"></div>
                    </div>
                </div>
                <div class="stat-card">
                    <div class="stat-label">Node Network</div>
                    <div id="s-ip" class="stat-value" style="color:var(--primary)">0.0.0.0</div>
                    <div id="s-sn" style="font-size:0.45rem; color:var(--muted); font-family:monospace; margin-top:1px">M: 0.0.0.0</div>
                </div>
                <div class="stat-card">
                    <div class="stat-label">MQTT Broker</div>
                    <div id="s-mqtt-host" class="stat-value" style="font-size:0.75rem">---</div>
                    <div id="s-mqtt-status" style="font-size:0.65rem; font-weight:800; margin-top:1px">OFFLINE</div>
                </div>
            </div>

            <div class="full-banner">
                <div class="banner-label">MS/TP Traffic Observation</div>
                <div class="banner-value" id="s-tokens">0 <span style="font-size:0.6rem; opacity:0.5">TOKENS</span></div>
            </div>

            <div class="cmd-group">
                <button class="btn btn-p" onclick="sendWhoIs()">Send Who-is</button>
                <button class="btn btn-s" onclick="sendIAm()">Send I-am</button>
            </div>

            <div class="card">
                <div class="card-header"><div class="card-title">Diagnostic Stream</div><div class="badge" id="s-heap">-- KB</div></div>
                <div id="console-out"></div>
            </div>
        </div>

        <!-- BACNET -->
        <div id="t-bac" class="tab-content">
            <div id="discovery-container"></div>
        </div>

        <!-- SETTINGS -->
        <div id="t-conf" class="tab-content">
            <div class="card">
                <div class="card-header"><div class="card-title">Network Configuration</div><button class="btn btn-p" onclick="doSaveNet()">Save</button></div>
                <form id="f-net" class="grid-form">
                    <input type="hidden" name="form_type" value="wifi">
                    <div class="form-group"><label>WiFi SSID</label><input type="text" name="ssid" id="in-ssid"></div>
                    <div class="form-group"><label>WiFi Password</label><input type="password" name="pass" id="in-pass"></div>
                    <div class="full-w"><label class="checkbox-row" style="background:#000; border-radius:6px; padding:0.5rem; display:flex; align-items:center; gap:0.5rem; cursor:pointer"><input type="checkbox" name="static_ip" id="in-static" onchange="toggleStatic()"><span style="font-size:0.65rem; font-weight:700">STATIC IP MODE</span></label></div>
                    <div id="div-static" class="full-w grid-form" style="display:none; padding:0; gap:0.5rem; margin-top:0.5rem">
                        <div class="form-group"><label>Node IP</label><input type="text" name="local_ip" id="in-ip"></div>
                        <div class="form-group"><label>Gateway</label><input type="text" name="gateway" id="in-gw"></div>
                        <div class="form-group"><label>Subnet Mask</label><input type="text" name="subnet" id="in-sn"></div>
                    </div>
                </form>
            </div>
            <div class="card">
                <div class="card-header"><div class="card-title">BACnet MS/TP Settings</div><button class="btn btn-p" onclick="doSaveBac()">Save</button></div>
                <form id="f-bac" class="grid-form">
                    <div class="form-group"><label>Station MAC</label><input type="number" name="mac" id="in-mac"></div>
                    <div class="form-group"><label>Max Retries</label><input type="number" name="retries" id="in-retries" value="3"></div>
                    <div class="form-group"><label>Max Master</label><input type="number" name="mm" id="in-mm"></div>
                    <div class="form-group"><label>APDU Timeout (ms)</label><input type="number" name="timeout" id="in-timeout" value="1000"></div>
                </form>
            </div>
            <div class="card">
                <div class="card-header"><div class="card-title">MQTT Bridge Protocol</div><button class="btn btn-p" onclick="doSaveMqtt()">Save</button></div>
                <form id="f-mqtt" class="grid-form">
                    <div class="full-w"><label>Broker Server IP</label><input type="text" name="mqh" id="in-mqh"></div>
                    <div class="form-group"><label>MQTT User</label><input type="text" name="mqu" id="in-mqu"></div>
                    <div class="form-group"><label>MQTT Password</label><input type="password" name="mqp" id="in-mqp"></div>
                </form>
            </div>
            <div class="grid-stats" style="margin-top:1rem; grid-template-columns: 1fr 1fr 1fr;">
                <button class="btn btn-d" onclick="doResetCache()">CLEAR BACNET cache</button>
                <button class="btn btn-s" onclick="fetch('/reboot')">REBOOT GATEWAY</button>
                <button class="btn btn-d" style="background:#7f1d1d" onclick="doResetFactory()">RESET FACTORY</button>
            </div>
        </div>
    </div>
    <script>
        function openTab(e, id) {
            const c=document.getElementsByClassName("tab-content"); for(let i=0; i<c.length; i++)c[i].style.display="none";
            const b=document.getElementsByClassName("tab-btn"); for(let i=0; i<b.length; i++)b[i].classList.remove("active");
            document.getElementById(id).style.display="block"; e.currentTarget.classList.add("active");
            if(id === 't-bac') refreshDiscovery(); if(id === 't-conf') loadConfig();
        }
        function toggleStatic() { document.getElementById('div-static').style.display = document.getElementById('in-static').checked ? 'grid' : 'none'; }
        function loadConfig() {
            fetch('/api/status').then(r=>r.json()).then(d=>{
                document.getElementById('in-ssid').value = d.ssid || "";
                document.getElementById('in-pass').value = "******";
                document.getElementById('in-static').checked = d.static || false;
                document.getElementById('in-ip').value = d.ip || "";
                document.getElementById('in-gw').value = d.gw || "";
                document.getElementById('in-sn').value = d.sn || "";
                document.getElementById('in-mac').value = d.mac_id || 1;
                document.getElementById('in-mm').value = d.mm || 127;
                document.getElementById('in-mqh').value = d.mqh || "";
                document.getElementById('in-mqu').value = d.mqu || "";
                document.getElementById('in-mqp').value = d.mqp_set ? "******" : "";
                document.getElementById('in-timeout').value = d.to || 1000;
                document.getElementById('in-retries').value = d.ret || 3;
                toggleStatic();
            });
        }
        function sendWhoIs() { fetch('/api/whois', {method:'POST'}).then(()=>alert("Who-is Broadcast sent.")); }
        function sendIAm() { fetch('/api/iam', {method:'POST'}).then(()=>alert("I-am Response sent.")); }
        function doSaveNet() { fetch('/save', {method:'POST', body:new FormData(document.getElementById('f-net'))}).then(()=>alert("Network Saved.")); }
        function doSaveBac() { fetch('/save', {method:'POST', body:new FormData(document.getElementById('f-bac'))}).then(()=>alert("BACnet Saved.")); }
        function doSaveMqtt() { fetch('/save', {method:'POST', body:new FormData(document.getElementById('f-mqtt'))}).then(()=>alert("MQTT Saved.")); }
        function doResetCache() { if(confirm("Clear BACnet Cache?")) fetch('/api/reset_cache', {method:'POST'}).then(()=>alert("Cache cleared. Rebooting...")); }
        function doResetFactory() { if(confirm("DANGER: Factory Reset?")) fetch('/api/factory_reset', {method:'POST'}).then(()=>alert("Factory Reset OK. Rebooting to AP mode...")); }
        function toggleDevice(id, enabled) {
            const data = { device_id: id, enabled: enabled, objects: [] };
            fetch('/api/save_objects', {method:'POST', body:JSON.stringify(data), headers:{'Content-Type':'application/json'}}).then(() => {
                const block = document.querySelector(`[data-dev="${id}"] table`);
                if(block) block.style.opacity = enabled ? '1' : '0.4';
                if(block) block.style.pointerEvents = enabled ? 'auto' : 'none';
            });
        }

        function downloadEDE(id) {
            window.location.href = "/api/download_ede";
        }
        function saveDeviceChanges(id) {
            const block = document.querySelector(`[data-dev="${id}"]`);
            const rows = block.querySelectorAll('tbody tr');
            const objects = [];
            rows.forEach(r => {
                objects.push({
                    inst: parseInt(r.getAttribute('data-inst')),
                    type: parseInt(r.getAttribute('data-type')),
                    name: r.querySelector('.in-edit').value,
                    poll: r.querySelector('.poll-sw').checked
                });
            });
            const data = { device_id: id, objects: objects };
            fetch('/api/save_objects', {method:'POST', body:JSON.stringify(data), headers:{'Content-Type':'application/json'}}).then(() => alert("Saved."));
        }
        function refreshDiscovery() {
            fetch('/api/objects').then(r=>r.json()).then(controllers=>{
                const container = document.getElementById('discovery-container');
                if(!controllers || controllers.length === 0) { container.innerHTML = "<div style='padding:2rem; text-align:center; color:var(--muted)'>Listening MS/TP Traffic...</div>"; return; }
                let html = '';
                controllers.forEach(c => {
                    html += `<div class="controller-block" data-dev="${c.device_id}">
                        <div class="controller-header">
                            <div style="display:flex; flex-direction:column">
                                <span style="font-weight:800; color:var(--primary); font-size:0.75rem">ID:${c.device_id} | ${c.name}</span>
                                <span style="font-size:0.45rem; color:var(--muted)">${c.vendor}</span>
                            </div>
                            <div class="header-actions">
                                <button class="btn btn-s btn-lg" onclick="downloadEDE(${c.device_id})">EDE</button>
                                <button class="btn btn-p btn-lg" onclick="saveDeviceChanges(${c.device_id})">SAVE</button>
                                <label class="switch"><input type="checkbox" ${c.enabled?'checked':''} onchange="toggleDevice(${c.device_id}, this.checked)"><span class="slider"></span></label>
                            </div>
                        </div>
                        <table style="${c.enabled?'':'opacity:0.4;pointer-events:none'}"><thead><tr><th>OBJ</th><th>NAME</th><th>VALUE</th><th>UNIT</th><th>POLL</th></tr></thead><tbody>`;
                    c.objects.forEach(o => {
                        if (o.type === 8) return; // Skip Device Object
                        let tStr = o.type == 0 ? "AI" : o.type == 1 ? "AO" : o.type == 2 ? "AV" : o.type == 3 ? "BI" : o.type == 4 ? "BO" : o.type == 5 ? "BV" : o.type == 13 ? "MSI" : o.type == 14 ? "MSO" : o.type == 19 ? "MSV" : "OBJ";
                        let valStr = (o.val !== null && o.val !== undefined) ? o.val.toFixed(2) : "---";
                        html += `<tr data-inst="${o.inst}" data-type="${o.type}">
                            <td><span class="tag-obj">${tStr}:${o.inst}</span></td>
                            <td><input type="text" class="in-edit" value="${o.name}"></td>
                            <td style="color:var(--success); font-weight:700; font-family:monospace">${valStr}</td>
                            <td><span class="badge">${o.unit || "---"}</span></td>
                            <td><label class="switch"><input type="checkbox" class="poll-sw" ${o.poll?'checked':''}><span class="slider"></span></label></td>
                        </tr>`;
                    });
                    html += `</tbody></table>
                        <div style="padding:0.6rem; border-top:1px solid var(--border); background:#141416">
                            <button class="btn btn-p btn-lg" style="width:100%" onclick="saveDeviceChanges(${c.device_id})">SAVE CHANGES FOR THIS DEVICE</button>
                        </div>
                    </div>`;
                });
                container.innerHTML = html;
            });
        }

        function updateStatus() {
            fetch('/api/status').then(r=>r.json()).then(d=>{
                const rssi = d.rssi;
                document.getElementById('s-rssi-val').innerText = rssi + " dBm";
                const radar = document.getElementById('radar').children;
                const activeCount = Math.round(((rssi + 100) / 60) * 8);
                for(let i=0; i<8; i++) {
                    radar[i].className = 'echo-segment';
                    if(i < activeCount) {
                        let color = i < 2 ? 'var(--error)' : i < 5 ? 'var(--warning)' : 'var(--success)';
                        radar[i].style.backgroundColor = color; radar[i].classList.add('active');
                    } else { radar[i].style.backgroundColor = '#27272a'; }
                }
                document.getElementById('s-ip').innerText=d.ip;
                document.getElementById('s-sn').innerText="M: " + (d.sn || "255.255.255.0");
                document.getElementById('s-tokens').innerHTML=(d.mstp_cnt || 0) + ' <span style="font-size:0.6rem; opacity:0.5">TOKENS</span>';
                document.getElementById('s-mqtt-host').innerText = d.mqh || "UNDEFINED";
                const mqS = document.getElementById('s-mqtt-status');
                mqS.innerText = d.mqtt ? "SYNC" : "OFFLINE";
                mqS.style.color = d.mqtt ? "var(--success)" : "var(--error)";
                document.getElementById('s-heap').innerText=(d.heap) + " KB";
                
                const bTag = document.getElementById('b-tag');
                const mTag = document.getElementById('m-tag');
                bTag.className = 'badge ' + (d.mstp_t ? 'badge-ok' : 'badge-fail');
                mTag.className = 'badge ' + (d.mqtt ? 'badge-ok' : 'badge-fail');
            });
        }
        let ws_url = `ws://${window.location.host}/ws-logs`;
        try {
            let ws = new WebSocket(ws_url);
            ws.onmessage = (e) => {
                const c = document.getElementById('console-out');
                if(!c) return;
                const line = document.createElement('div');
                line.textContent = e.data; c.appendChild(line); c.scrollTop = c.scrollHeight;
                if(c.childNodes.length > 300) c.removeChild(c.firstChild);
            };
        } catch(err) {}
        setInterval(updateStatus, 3000); updateStatus();
    </script>
</body>
</html>
)rawliteral";
#endif
