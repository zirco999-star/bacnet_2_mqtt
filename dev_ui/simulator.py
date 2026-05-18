from http.server import HTTPServer, SimpleHTTPRequestHandler
import os

class MockHandler(SimpleHTTPRequestHandler):
    def do_GET(self):
        # Toujours servir ui_final.html pour la racine
        if self.path == '/' or self.path == '/index.html':
            self.send_response(200)
            self.send_header('Content-type', 'text/html; charset=utf-8')
            self.end_headers()
            with open('ui_final.html', 'rb') as f:
                self.wfile.write(f.read())
        else:
            super().do_GET()

os.chdir(os.path.dirname(os.path.abspath(__file__)))
port = 8000
print(f"🚀 Simulateur UI v2.2.1 DEMARRÉ sur http://localhost:{port}")
print("Fichier source : ui_final.html")
print("Appuie sur Ctrl+C pour arrêter.")
HTTPServer(('0.0.0.0', port), MockHandler).serve_forever()
