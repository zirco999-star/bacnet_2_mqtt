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
        .logo span { color: var(--muted); font-size: 0.6rem; text-transform: none; margin-left: 0.3rem; }
        .badge { border: 1px solid currentColor; padding: 0.1rem 0.4rem; font-size: 0.6rem; font-weight: bold; white-space: nowrap; }
        .tabs { display: flex; background: #000; border-bottom: 1px solid var(--border); overflow-x: auto; scrollbar-width: none; }
        .tab-btn { padding: 0.8rem 1rem; background: #141b21; border: none; color: var(--muted); font-size: 0.7rem; font-weight: bold; cursor: pointer; border-top: 2px solid transparent; transition: all 0.1s; white-space: nowrap; }
        .tab-btn.active { background: var(--bg); color: var(--primary); border-top-color: var(--primary); }
        .container { max-width: 1100px; margin: 1rem auto; padding: 0 0.75rem; }
        .tab-content { display: none; }
        .tab-content.active { display: block; }
        .card { background: var(--card); border: 1px solid var(--border); margin-bottom: 0.75rem; }
        .card-header { background: #1e293b; padding: 0.5rem 0.75rem; color: var(--accent); font-size: 0.65rem; font-weight: bold; text-transform: uppercase; display: flex; justify-content: space-between; }
        .card-body { padding: 1rem; }
        .grid-stats { display: grid; grid-template-columns: repeat(auto-fit, minmax(150px, 1fr)); gap: 1px; background: var(--border); border: 1px solid var(--border); }
        .stat-item { background: var(--card); padding: 0.6rem; }
        .stat-label { font-size: 0.55rem; color: var(--muted); }
        .stat-value { font-size: 0.85rem; font-weight: bold; color: var(--success); }
        form { display: grid; grid-template-columns: 1fr; gap: 0.75rem; }
        @media(min-width: 640px) { form { grid-template-columns: 1fr 1fr; } }
        .full-w { grid-column: 1 / -1; }
        label { display: block; font-size: 0.6rem; color: var(--muted); margin-bottom: 0.2rem; }
        input, select { background: #000; border: 1px solid var(--border); padding: 0.6rem; color: var(--success); border-radius: 0; font-family: monospace; font-size: 0.8rem; width: 100%; box-sizing: border-box; }
        .checkbox-row { grid-column: 1 / -1; display: flex; align-items: center; gap: 0.75rem; background: #000; padding: 0.75rem; border: 1px solid var(--border); cursor: pointer; }
        .checkbox-row input { width: 1.2rem; height: 1.2rem; margin: 0; }
        button.btn-cmd { background: transparent; border: 1px solid var(--primary); color: var(--primary); padding: 0.75rem; font-weight: bold; cursor: pointer; text-transform: uppercase; font-size: 0.75rem; width: 100%; }
        button.btn-cmd:hover { background: var(--primary); color: #000; }
        #console-out { background: #05070a; color: #4ade80; height: 350px; overflow-y: auto; padding: 0.5rem; font-size: 0.65rem; border: 1px solid #1e293b; line-height: 1.3; margin-top: 0.5rem; }
        #up-progress { height: 4px; background: #000; border-radius: 2px; margin-top: 10px; display: none; overflow: hidden; }
        #up-bar { width: 0%; height: 100%; background: var(--success); transition: width 0.3s; }
        table { width: 100%; border-collapse: collapse; font-size: 0.75rem; }
        th { text-align: left; color: var(--muted); border-bottom: 1px solid var(--border); padding: 0.5rem; }
        td { padding: 0.5rem; border-bottom: 1px solid #1e293b; }
    </style>
</head>
<body>
    <nav>
        <div class="logo">BACnetMSTP2MQTT <span id="v-tag">...</span> <span>by Z1rc0n1um</span></div>
        <div id="status-tag" class="badge" style="color:var(--error)">Disconnected</div>
    </nav>
    <div class="tabs">
        <button class="tab-btn active" onclick="openTab(event, 't-dash')">Dashboard</button>
        <button class="tab-btn" onclick="openTab(event, 't-bac')">Discovery</button>
        <button class="tab-btn" onclick="openTab(event, 't-conf')">Configuration</button>
        <button class="tab-btn" onclick="openTab(event, 't-sys')">System</button>
    </div>
    <div class="container">
        <div id="t-dash" class="tab-content active">
            <div class="grid-stats">
                <div class="stat-item"><div class="stat-label">WiFi Signal</div><div id="s-rssi" class="stat-value">--</div></div>
                <div class="stat-item"><div class="stat-label">Local IP</div><div id="s-ip" class="stat-value">0.0.0.0</div></div>
                <div class="stat-item"><div class="stat-label">Tokens Seen</div><div id="s-tokens" class="stat-value">0</div></div>
                <div class="stat-item"><div class="stat-label">PFM Replies</div><div id="s-pfm" class="stat-value">0</div></div>
            </div>
            <div class="card" style="margin-top: 0.75rem;">
                <div class="card-header">Kernel Stream (Live Logs)</div>
                <div id="console-out"></div>
            </div>
        </div>
        <div id="t-bac" class="tab-content">
            <div class="card"><div class="card-header">Discovered BACnet Devices</div><div class="card-body">
                <div id="cache-list"></div>
                <button type="button" onclick="fetch('/api/discover')" class="btn-cmd full-w" style="margin-top:1rem; border-color:var(--accent); color:var(--accent)">Trigger Who-Is</button>
            </div></div>
        </div>
        <div id="t-conf" class="tab-content">
            <div class="card"><div class="card-header">Network Settings</div><div class="card-body">
                <form id="f-wifi">
                    <input type="hidden" name="form_type" value="wifi">
                    <div class="form-group"><label>SSID</label><input type="text" name="ssid" id="in-ssid"></div>
                    <div class="form-group"><label>Password</label><input type="password" name="pass" id="in-pass"></div>
                    <label class="checkbox-row"><input type="checkbox" name="static_ip" id="in-static" onchange="document.getElementById('static-box').style.display=this.checked?'grid':'none'"><span>Static IP Assignment</span></label>
                    <div id="static-box" class="full-w" style="display:none; grid-template-columns: 1fr 1fr; gap: 0.75rem;">
                        <div class="form-group"><label>Address</label><input type="text" name="local_ip" id="in-ip"></div>
                        <div class="form-group"><label>Gateway</label><input type="text" name="gateway" id="in-gw"></div>
                        <div class="form-group"><label>Mask</label><input type="text" name="subnet" id="in-sn"></div>
                    </div>
                    <button type="button" onclick="doSave('f-wifi')" class="btn-cmd full-w">Save Network</button>
                </form>
            </div></div>
            <div class="card"><div class="card-header">BACnet & MQTT Configuration</div><div class="card-body">
                <form id="f-bac">
                    <input type="hidden" name="form_type" value="bacnet">
                    <div class="form-group"><label>Local MAC</label><input type="number" name="mac" id="in-mac"></div>
                    <div class="form-group"><label>Device ID</label><input type="number" name="did" id="in-did"></div>
                    <div class="form-group"><label>MQTT Server</label><input type="text" name="mqh" id="in-mqh"></div>
                    <div class="form-group"><label>MQTT Prefix</label><input type="text" name="mqpr" id="in-mqpr"></div>
                    <button type="button" onclick="doSave('f-bac')" class="btn-cmd full-w">Update Services</button>
                </form>
            </div></div>
        </div>
        <div id="t-sys" class="tab-content">
            <div class="card"><div class="card-header">Firmware Update</div><div class="card-body">
                <input type="file" id="fw-file">
                <button type="button" onclick="doUpdate()" class="btn-cmd full-w" style="margin-top:1rem">Flash OTA</button>
                <div id="up-progress"><div id="up-bar"></div></div>
            </div></div>
            <button class="btn-cmd" style="color:red; border-color:red; margin-top:1rem" onclick="if(confirm('Reboot?'))fetch('/reboot')">System Reboot</button>
        </div>
    </div>
    <script>
        let ws;
        function openTab(e, id) {
            const c=document.getElementsByClassName("tab-content"); for(let i=0; i<c.length; i++)c[i].style.display="none";
            const b=document.getElementsByClassName("tab-btn"); for(let i=0; i<b.length; i++)b[i].classList.remove("active");
            document.getElementById(id).style.display="block"; e.currentTarget.classList.add("active");
            if(id === 't-bac') refreshCache();
        }
        function doSave(fid) {
            fetch('/save', {method:'POST', body:new FormData(document.getElementById(fid))})
            .then(r=>r.text()).then(t=>{alert(t); location.reload();});
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
        function initWS() {
            ws = new WebSocket(`ws://${window.location.host}/ws-logs`);
            ws.onmessage = (e) => {
                const c = document.getElementById('console-out');
                const line = document.createElement('div'); line.textContent = e.data;
                c.appendChild(line); c.scrollTop = c.scrollHeight;
                if(c.childNodes.length > 500) c.removeChild(c.firstChild);
            };
            ws.onclose = () => setTimeout(initWS, 2000);
        }
        function updateStatus() {
            fetch('/api/status').then(r=>r.json()).then(d=>{
                document.getElementById('v-tag').innerText=d.ver;
                document.getElementById('s-rssi').innerText=d.rssi + " dBm";
                document.getElementById('s-ip').innerText=d.ip;
                document.getElementById('s-tokens').innerText=d.mstp_t;
                document.getElementById('s-pfm').innerText=d.mstp_p;
                document.getElementById('status-tag').style.color="var(--success)"; 
                document.getElementById('status-tag').innerText="Connected";
            }).catch(()=>{
                document.getElementById('status-tag').style.color="var(--error)";
                document.getElementById('status-tag').innerText="Disconnected";
            });
        }
        function refreshCache() {
            fetch('/api/cache').then(r=>r.json()).then(data=>{
                const container = document.getElementById('cache-list');
                container.innerHTML = '';
                if(data.length === 0) container.innerHTML = '<p style="color:var(--muted); font-size:0.7rem">No devices discovered yet.</p>';
                data.forEach(dev => {
                    const devCard = document.createElement('div');
                    devCard.className = 'card';
                    devCard.style.marginBottom = '1rem';
                    devCard.innerHTML = `<div class="card-header">Device ID: ${dev.id} (MAC: ${dev.mac}) ${dev.done ? '✅' : '⏳'}</div>`;
                    const body = document.createElement('div');
                    body.className = 'card-body';
                    if (dev.objs && dev.objs.length > 0) {
                        let html = '<table><tr><th>Obj Type</th><th>Instance</th><th>Value</th></tr>';
                        dev.objs.forEach(obj => {
                            html += `<tr><td>${obj.t}</td><td>${obj.i}</td><td style="color:var(--success); font-weight:bold">${obj.v.toFixed(2)}</td></tr>`;
                        });
                        html += '</table>';
                        body.innerHTML = html;
                    } else {
                        body.innerHTML = '<p style="color:var(--muted); font-size:0.6rem">No objects listed yet. Reading Object_List...</p>';
                    }
                    devCard.appendChild(body);
                    container.appendChild(devCard);
                });
            });
        }
        function loadConfig() {
            fetch('/api/config').then(r=>r.json()).then(c=>{
                document.getElementById('in-ssid').value = c.ssid || "";
                document.getElementById('in-static').checked = c.static_ip;
                document.getElementById('in-static').dispatchEvent(new Event('change'));
                document.getElementById('in-ip').value = c.local_ip;
                document.getElementById('in-gw').value = c.gateway;
                document.getElementById('in-sn').value = c.subnet;
                document.getElementById('in-mac').value = c.mac;
                document.getElementById('in-did').value = c.did;
                document.getElementById('in-mqh').value = c.mqh;
                document.getElementById('in-mqpr').value = c.mqpr;
            });
        }
        initWS(); setInterval(updateStatus, 3000); loadConfig(); updateStatus();
        setInterval(() => { if(document.getElementById('t-bac').classList.contains('active')) refreshCache(); }, 5000);
    </script>
</body>
</html>
)rawliteral";
#endif
