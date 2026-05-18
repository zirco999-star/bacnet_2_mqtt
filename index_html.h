const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="fr">
<head>
    <meta charset="UTF-8"><meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>ZIRCON1UM | BACnet</title>
    <script src="https://cdn.tailwindcss.com"></script>
    <style>.log-c { height: 350px; overflow-y: auto; font-family: monospace; font-size: 0.75rem; }</style>
</head>
<body class="bg-slate-900 text-slate-100 font-sans">
    <nav class="bg-indigo-700 p-4 shadow-xl flex justify-between items-center">
        <h1 class="text-xl font-bold">ZIRCON1UM <span class="text-xs font-light">v1.9</span></h1>
        <div id="status" class="px-3 py-1 rounded-full text-xs font-bold bg-red-500">Hors-ligne</div>
    </nav>
    <main class="container mx-auto p-4 max-w-5xl grid grid-cols-1 md:grid-cols-3 gap-6">
        <section class="bg-slate-800 p-5 rounded-lg border border-slate-700 shadow-lg">
            <h2 class="text-indigo-400 font-bold mb-4 uppercase tracking-wider text-sm">Système</h2>
            <div class="space-y-2 text-sm">
                <div class="flex justify-between border-b border-slate-700 pb-1"><span>WiFi</span><span id="rssi" class="font-mono">-- dBm</span></div>
                <div class="flex justify-between border-b border-slate-700 pb-1"><span>IP</span><span id="ip" class="font-mono">0.0.0.0</span></div>
                <div class="flex justify-between"><span>Mémoire</span><span id="heap" class="font-mono text-green-400">-- KB</span></div>
            </div>
        </section>
        <section class="md:col-span-2 bg-slate-800 p-5 rounded-lg border border-slate-700 shadow-lg">
            <h2 class="text-indigo-400 font-bold mb-4 uppercase tracking-wider text-sm">Configuration</h2>
            <form action="/save" method="POST" class="grid grid-cols-1 md:grid-cols-2 gap-3 text-xs">
                <div><label class="text-slate-400 block mb-1">SSID WiFi</label><input type="text" name="ssid" class="w-full bg-slate-900 border border-slate-600 p-2 rounded"></div>
                <div><label class="text-slate-400 block mb-1">Password</label><input type="password" name="pass" class="w-full bg-slate-900 border border-slate-600 p-2 rounded"></div>
                <div class="flex items-center space-x-2 pt-4">
                    <input type="checkbox" name="static_ip" id="static_ip" class="w-4 h-4 text-indigo-600 bg-slate-900 border-slate-600 rounded">
                    <label for="static_ip" class="text-slate-400 uppercase font-bold text-[10px]">IP Statique</label>
                </div>
                <div><label class="text-slate-400 block mb-1">IP Locale</label><input type="text" name="local_ip" placeholder="192.168.1.50" class="w-full bg-slate-900 border border-slate-600 p-2 rounded"></div>
                <button type="submit" class="md:col-span-2 bg-indigo-600 hover:bg-indigo-500 text-white font-bold py-2 rounded transition mt-2">Sauvegarder et Redémarrer</button>
            </form>
        </section>
        <section class="md:col-span-3 bg-black p-3 rounded-lg border border-slate-800 shadow-inner">
            <div id="logs" class="log-c text-green-400 leading-tight"></div>
        </section>
    </main>
    <script>
        let ws;
        function connect() {
            ws = new WebSocket('ws://' + window.location.hostname + '/ws-logs');
            ws.onopen = () => { document.getElementById('status').className='px-3 py-1 rounded-full text-xs font-bold bg-green-500'; document.getElementById('status').innerText='En ligne'; };
            ws.onclose = () => { document.getElementById('status').className='px-3 py-1 rounded-full text-xs font-bold bg-red-500'; document.getElementById('status').innerText='Déconnecté'; setTimeout(connect, 2000);};
            ws.onmessage = (e) => {
                const l = document.getElementById('logs');
                const div = document.createElement('div'); div.textContent = e.data; l.appendChild(div);
                l.scrollTop = l.scrollHeight;
                if(l.children.length > 200) l.removeChild(l.firstChild);
                if(e.data.includes("Signal")) document.getElementById('rssi').innerText = e.data.split("Signal")[1].split("dBm")[0].trim() + " dBm";
                if(e.data.includes("Heap:")) document.getElementById('heap').innerText = e.data.split("Heap:")[1].trim();
                if(e.data.includes("IP:")) {
                    const ipMatch = e.data.match(/IP: ([0-9\.]+)/);
                    if(ipMatch) document.getElementById('ip').innerText = ipMatch[1];
                }
            };
        }
        window.onload = connect;
    </script>
</body>
</html>
)rawliteral";
