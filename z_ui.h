#ifndef Z_UI_H
#define Z_UI_H

#include <Arduino.h>

const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="fr">
<head>
    <meta charset="UTF-8"><meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>BACnet-MS/TP_2_MQTT v2.3 | Console</title>
    <style>
        :root { 
            --bg: #0a0f14; --card: #141b21; --primary: #f59e0b; --accent: #0ea5e9; 
            --text: #e2e8f0; --muted: #64748b; --border: #334155; --success: #22c55e; --error: #ef4444;
        }
        body { background: var(--bg); color: var(--text); font-family: 'Consolas', 'Monaco', monospace; margin: 0; }
        nav { background: #000; padding: 0.75rem 1rem; border-bottom: 2px solid var(--primary); display: flex; justify-content: space-between; align-items: center; position: sticky; top: 0; z-index: 100; }
        .logo { font-weight: 900; font-size: 0.9rem; color: var(--primary); text-transform: uppercase; letter-spacing: 0.05em; }
        .logo span { color: var(--muted); font-size: 0.6rem; letter-spacing: 0; text-transform: none; margin-left: 0.3rem; font-weight: 300; display: block; }
        @media(min-width: 640px) { .logo { font-size: 1.1rem; } .logo span { display: inline; font-size: 0.7rem; } }
        .badge { border: 1px solid currentColor; padding: 0.1rem 0.4rem; font-size: 0.6rem; font-weight: bold; white-space: nowrap; }
        .tabs { display: flex; background: #000; border-bottom: 1px solid var(--border); overflow-x: auto; scrollbar-width: none; }
        .tabs::-webkit-scrollbar { display: none; }
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
        input, select { background: #000; border: 1px solid var(--border); padding: 0.6rem; color: var(--success); border-radius: 0; font-family: monospace; font-size: 0.8rem; width: 100%; box-sizing: border-box; }
        .checkbox-row { grid-column: 1 / -1; display: flex; align-items: center; gap: 0.75rem; background: #000; padding: 0.75rem; border: 1px solid var(--border); cursor: pointer; }
        .checkbox-row input { width: 1.2rem; height: 1.2rem; margin: 0; }
        .checkbox-row span { font-size: 0.75rem; color: var(--text); }
        button.btn-cmd { background: transparent; border: 1px solid var(--primary); color: var(--primary); padding: 0.75rem; font-weight: bold; cursor: pointer; text-transform: uppercase; font-size: 0.75rem; width: 100%; }
        button.btn-cmd:hover { background: var(--primary); color: #000; }
        button.btn-danger { border-color: var(--error); color: var(--error); margin-top: 0.5rem; }
        button.btn-danger:hover { background: var(--error); color: #fff; }
        #console-out { background: #05070a; color: #4ade80; height: 250px; overflow-y: auto; padding: 0.5rem; font-size: 0.65rem; border: 1px solid #1e293b; line-height: 1.3; }
        .obj-table { width: 100%; border-collapse: collapse; font-size: 0.6rem; }
        .obj-table th { text-align: left; color: var(--muted); border-bottom: 1px solid var(--border); padding: 0.4rem; }
        .obj-table td { padding: 0.4rem; border-bottom: 1px solid #1e293b; }
        .scroll-x { overflow-x: auto; }
        #up-progress { height: 4px; background: #000; border-radius: 2px; margin-top: 10px; display: none; overflow: hidden; }
        #up-bar { width: 0%; height: 100%; background: var(--success); transition: width 0.3s; }
    </style>
</head>
<body>
    <nav>
        <div class="logo">BACnet-MS/TP_2_MQTT v2.3 <span>by Z1rc0n1um</span></div>
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
                <div class="stat-item"><div class="stat-label">WiFi</div><div id="s-rssi" class="stat-value">-- dBm</div></div>
                <div class="stat-item"><div class="stat-label">Local IP</div><div id="s-ip" class="stat-value">0.0.0.0</div></div>
                <div class="stat-item"><div class="stat-label">MS/TP Mode</div><div class="stat-value">TRANSCEIVER</div></div>
                <div class="stat-item"><div class="stat-label">MQTT Node</div><div id="s-mq" class="stat-value" style="color:var(--error)">OFFLINE</div></div>
            </div>
            <div class="card" style="margin-top: 0.75rem;">
                <div class="card-header"><span>Kernel Stream</span></div>
                <div id="console-out"></div>
            </div>
        </div>
        <div id="t-wifi" class="tab-content">
            <div class="card">
                <div class="card-header">WiFi Config</div>
                <div class="card-body"><form id="f-wifi">
                    <div class="form-group"><label>SSID</label><input type="text" name="ssid" id="in-ssid"></div>
                    <div class="form-group"><label>Password</label><input type="password" name="pass"></div>
                    <label class="checkbox-row"><input type="checkbox" name="static_ip" id="in-static" onchange="document.getElementById('static-box').style.display=this.checked?'grid':'none'"><span>Static IP Assignment</span></label>
                    <div id="static-box" class="full-w grid" style="display:none; grid-template-columns: 1fr 1fr; gap: 0.75rem;">
                        <div class="form-group"><label>Address</label><input type="text" name="local_ip" id="in-ip"></div>
                        <div class="form-group"><label>Gateway</label><input type="text" name="gateway" id="in-gw"></div>
                        <div class="form-group"><label>Mask</label><input type="text" name="subnet" id="in-sn"></div>
                    </div>
                    <button type="button" onclick="doSave('f-wifi')" class="btn-cmd full-w">Save Network</button>
                </form></div>
            </div>
        </div>
        <div id="t-bac" class="tab-content">
            <div class="card">
                <div class="card-header">BACnet Layer</div>
                <div class="card-body"><form id="f-bac">
                    <div class="form-group"><label>Local MAC</label><input type="text" name="mac" id="in-mac"></div>
                    <div class="form-group"><label>Target MAC</label><input type="text" name="target" id="in-target"></div>
                    <div class="form-group full-w"><label>Max Info Frames</label><input type="text" name="max_m" id="in-maxm"></div>
                    <button type="button" onclick="doSave('f-bac')" class="btn-cmd full-w">Update Stack</button>
                </form></div>
            </div>
            <div class="card"><div class="card-header">Discovered Objects</div><div class="card-body scroll-x" style="padding:0;"><table class="obj-table"><thead><tr><th>ID</th><th>TYPE</th><th>NAME</th><th>VALUE</th></tr></thead><tbody id="obj-list"><tr><td colspan="4" style="text-align:center; padding:1.5rem; color:var(--muted)">[ SCAN ACTIVE ]</td></tr></tbody></table></div></div>
        </div>
        <div id="t-mq" class="tab-content">
            <div class="card">
                <div class="card-header">MQTT Broker</div>
                <div class="card-body"><form id="f-mq">
                    <div class="form-group"><label>Server IP</label><input type="text" name="mq_host" id="in-mqh"></div>
                    <div class="form-group"><label>Port</label><input type="text" name="mq_port" id="in-mqp"></div>
                    <div class="form-group full-w"><label>Prefix</label><input type="text" name="mq_pref" id="in-mpre"></div>
                    <button type="button" onclick="doSave('f-mq')" class="btn-cmd full-w">Save Broker</button>
                </form></div>
            </div>
            <div class="card"><div class="card-header">Publication Map</div><div class="card-body scroll-x" style="padding:0;"><table class="obj-table"><thead><tr><th>TOPIC</th><th>OBJECT</th><th>STATE</th></tr></thead><tbody id="pub-list"><tr><td colspan="3" style="text-align:center; padding:1.5rem; color:var(--muted)">[ NO MAPPINGS ]</td></tr></tbody></table></div></div>
        </div>
        <div id="t-sys" class="tab-content">
            <div class="card">
                <div class="card-header">Engine Control</div>
                <div class="card-body"><form id="f-sys">
                    <div class="form-group full-w"><label>Logs</label><select name="log_lvl" id="in-logl"><option value="0">Fatal</option><option value="1">Warn</option><option value="2">Info</option><option value="3">Debug</option></select></div>
                    <label class="checkbox-row"><input type="checkbox" name="bridge_mode" id="in-bridge"><span>Enable TCP Bridge</span></label>
                    <div class="form-group"><label>Admin User</label><input type="text" name="adm_user" id="in-user"></div><div class="form-group"><label>Admin Password</label><input type="password" name="adm_pass"></div>
                    <button type="button" onclick="doSave('f-sys')" class="btn-cmd full-w">Save Config</button>
                </form></div>
            </div>
            <div class="card"><div class="card-header">Firmware Update</div><div class="card-body">
                <label>Select .bin File</label>
                <input type="file" id="fw-file" style="padding:0.4rem; font-size:0.7rem;">
                <button type="button" onclick="doUpdate()" class="btn-cmd full-w" style="margin-top:1rem; border-color:var(--accent); color:var(--accent);">Flash Firmware (OTA)</button>
                <div id="up-progress"><div id="up-bar"></div></div>
            </div></div>
            <button class="btn-cmd btn-danger" onclick="if(confirm('Reboot?'))fetch('/reboot')">Master Reboot</button>
        </div>
    </div>
    <script>
        function openTab(evt, id) {
            const c=document.getElementsByClassName("tab-content"); for(let i=0; i<c.length; i++)c[i].style.display="none";
            const b=document.getElementsByClassName("tab-btn"); for(let i=0; i<b.length; i++)b[i].classList.remove("active");
            document.getElementById(id).style.display="block"; evt.currentTarget.classList.add("active");
        }
        function doSave(fid) {
            const f = document.getElementById(fid);
            fetch('/save', {method:'POST', body:new FormData(f)})
            .then(r=>r.text()).then(t=>alert(t)).catch(e=>alert(e));
        }
        function doUpdate() {
            const file = document.getElementById('fw-file').files[0]; if(!file) return alert("Select file!");
            const fd = new FormData(); fd.append("update", file);
            const xhr = new XMLHttpRequest();
            xhr.open('POST', '/update', true);
            document.getElementById('up-progress').style.display = 'block';
            xhr.upload.onprogress = (e) => { if(e.lengthComputable) document.getElementById('up-bar').style.width = (e.loaded/e.total*100)+'%'; };
            xhr.onload = () => { alert(xhr.status === 200 ? "Success! Rebooting..." : "Update Failed"); location.reload(); };
            xhr.send(fd);
        }
        function updateStatus() {
            fetch('/api/status').then(r=>r.json()).then(d=>{
                document.getElementById('s-rssi').innerText=d.rssi+" dBm"; document.getElementById('s-ip').innerText=d.ip;
                const m=document.getElementById('s-mq'); m.innerText=d.mqtt?"ONLINE":"OFFLINE"; m.style.color=d.mqtt?"var(--success)":"var(--error)";
                document.getElementById('status-tag').style.color="var(--success)"; document.getElementById('status-tag').innerText="Connected";
            }).catch(()=>{document.getElementById('status-tag').style.color="var(--error)"; document.getElementById('status-tag').innerText="Disconnected";});
        }
        function loadConfig() {
            fetch('/api/config').then(r=>r.json()).then(c=>{
                if(document.getElementById('in-ssid')) document.getElementById('in-ssid').value = c.ssid || "";
                document.getElementById('in-static').checked = c.static_ip;
                document.getElementById('in-static').dispatchEvent(new Event('change'));
                document.getElementById('in-ip').value = c.local_ip || "";
                document.getElementById('in-gw').value = c.gateway || "";
                document.getElementById('in-sn').value = c.subnet || "";
                document.getElementById('in-mac').value = c.mac || "";
                document.getElementById('in-target').value = c.target || "";
                document.getElementById('in-maxm').value = c.max_m || "";
                document.getElementById('in-mqh').value = c.mq_host || "";
                document.getElementById('in-mqp').value = c.mq_port || "";
                document.getElementById('in-mpre').value = c.mq_pref || "";
                document.getElementById('in-logl').value = c.log_lvl;
                document.getElementById('in-bridge').checked = c.bridge;
                document.getElementById('in-user').value = c.adm_user || "admin";
            });
        }
        let ws;
        function connectWS() {
            ws = new WebSocket('ws://' + window.location.hostname + '/ws-logs');
            ws.onmessage = (e) => {
                const term = document.getElementById('console-out');
                const line = document.createElement('div');
                line.textContent = e.data; term.appendChild(line);
                term.scrollTop = term.scrollHeight;
                if(term.children.length > 200) term.removeChild(term.firstChild);
            };
            ws.onclose = () => setTimeout(connectWS, 2000);
        }
        window.onload=()=>{ loadConfig(); updateStatus(); connectWS(); setInterval(updateStatus, 5000); };
    </script>
</body>
</html>
)rawliteral";

#endif
