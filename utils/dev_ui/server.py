import json
import random
from http.server import HTTPServer, SimpleHTTPRequestHandler
import os

class MockHandler(SimpleHTTPRequestHandler):
    def do_GET(self):
        if self.path == '/' or self.path == '':
            self.path = '/index.html'
            return super().do_GET()
        if self.path == '/api/status':
            self.send_response(200)
            self.send_header('Content-type', 'application/json')
            self.end_headers()
            status = {"ver": "v4.5.6-SIM", "rssi": -62, "ip": "192.168.1.50", "mqtt": True, "heap": 185, "mac_id": 1, "mstp_t": 1250, "mqh": "192.168.1.10"}
            self.wfile.write(json.dumps(status).encode())
        elif self.path == '/api/objects':
            self.send_response(200)
            self.send_header('Content-type', 'application/json')
            self.end_headers()
            controllers = [{
                "device_id": 364004, "name": "AHU-MAIN-WING", "vendor": "Schneider Electric", "version": "v3.1.2", "enabled": True,
                "objects": [
                    {"type": 0, "inst": 1, "name": "Room_Temp", "val": 22.45, "unit": "°C", "poll": True},
                    {"type": 2, "inst": 10, "name": "SetPoint", "val": 21.0, "unit": "°C", "poll": True}
                ]
            }]
            self.wfile.write(json.dumps(controllers).encode())
        else:
            return super().do_GET()

    def do_POST(self):
        self.send_response(200)
        self.send_header('Content-type', 'text/plain')
        self.end_headers()
        self.wfile.write(b"OK (SIMULATED SAVE)")

if __name__ == '__main__':
    os.chdir(os.path.dirname(os.path.abspath(__file__)))
    server = HTTPServer(('0.0.0.0', 8000), MockHandler)
    print(f"🚀 Simulateur UI v4.5.6 (Logic Phase) démarré sur http://localhost:8000")
    server.serve_forever()
