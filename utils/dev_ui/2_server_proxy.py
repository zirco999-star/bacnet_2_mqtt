import http.server
import socketserver
import urllib.request
import urllib.error
import os
import base64

# Configuration de la passerelle matérielle
PORT = 8000
ESP32_IP = "http://192.168.1.50"
CREDENTIALS = "admin:admin1234"

class ProxyHTTPRequestHandler(http.server.SimpleHTTPRequestHandler):
    def do_GET(self):
        # Interception des appels API pour les rediriger vers l'ESP32-S3
        if self.path.startswith("/api/"):
            target_url = ESP32_IP + self.path
            print(f"[API] Routage transparent -> {target_url}")
            try:
                # Préparation de l'en-tête d'authentification
                auth_str = base64.b64encode(CREDENTIALS.encode()).decode()
                req = urllib.request.Request(target_url)
                req.add_header("Authorization", f"Basic {auth_str}")
                
                with urllib.request.urlopen(req) as response:
                    self.send_response(response.getcode())
                    self.send_header('Content-Type', response.info().get_content_type())
                    self.end_headers()
                    self.wfile.write(response.read())
            except urllib.error.URLError as e:
                status_code = getattr(e, 'code', 502)
                print(f"[ERREUR API] {target_url} : {e}")
                self.send_error(status_code, f"Erreur de communication BACnet Gateway: {e}")
        else:
            # Service des fichiers statiques locaux (HTML, CSS, JS)
            super().do_GET()

if __name__ == "__main__":
    # Force le répertoire de travail sur le dossier du script
    os.chdir(os.path.dirname(os.path.abspath(__file__)))
    
    print("=====================================================")
    print(" Serveur de Développement UI BACnet/MQTT (Waveshare) ")
    print("=====================================================")
    print(f"-> Édite le fichier local : {os.path.abspath('index.html')}")
    print(f"-> Ouvre ton navigateur   : http://localhost:{PORT}")
    print(f"-> Proxy API matériel     : {ESP32_IP}")
    print("=====================================================\n")
    
    # Allow address reuse
    socketserver.TCPServer.allow_reuse_address = True
    with socketserver.TCPServer(("0.0.0.0", PORT), ProxyHTTPRequestHandler) as httpd:
        try:
            httpd.serve_forever()
        except KeyboardInterrupt:
            print("\nArrêt du serveur proxy.")
