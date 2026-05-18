from http.server import HTTPServer, SimpleHTTPRequestHandler
import json

class MockHandler(SimpleHTTPRequestHandler):
    def do_GET(self):
        if self.path == '/api/config':
            self.send_response(200)
            self.send_header('Content-type', 'application/json')
            self.end_headers()
            config = {
                "ssid": "MaBox_WiFi",
                "static_ip": True,
                "local_ip": "192.168.1.50",
                "gateway": "192.168.1.1",
                "subnet": "255.255.255.0"
            }
            self.wfile.write(json.dumps(config).encode())
        elif self.path == '/api/status':
            self.send_response(200)
            self.send_header('Content-type', 'application/json')
            self.end_headers()
            status = {
                "ip": "192.168.1.50",
                "rssi": -62,
                "heap": 245000,
                "uptime": 1234
            }
            self.wfile.write(json.dumps(status).encode())
        else:
            return SimpleHTTPRequestHandler.do_GET(self)

    def do_POST(self):
        if self.path == '/save':
            self.send_response(200)
            self.send_header('Content-type', 'text/plain')
            self.end_headers()
            self.wfile.write(b"OK. Simulator saved configuration.")
        else:
            self.send_response(404)
            self.end_headers()

port = 8000
print(f"🚀 Simulateur UI ZIRCON1UM démarré sur http://localhost:{port}")
print("Modifie 'index.html' et rafraîchis ton navigateur pour voir les changements.")
HTTPServer(('localhost', port), MockHandler).serve_forever()
