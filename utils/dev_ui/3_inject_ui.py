import os
import re

Z_UI_PATH = os.path.join(os.path.dirname(__file__), "../../src/z_ui.h")
HTML_INPUT = os.path.join(os.path.dirname(__file__), "index.html")

def inject_html():
    print("[3/3] Injection de la nouvelle UI dans le firmware...")
    try:
        if not os.path.exists(HTML_INPUT):
            print(f"❌ Erreur : Le fichier {HTML_INPUT} n'existe pas.")
            return

        with open(HTML_INPUT, "r", encoding="utf-8") as f:
            new_html = f.read()

        with open(Z_UI_PATH, "r", encoding="utf-8") as f:
            header_content = f.read()

        # Remplacement sécurisé par expression régulière (RegEx)
        # On capture les balises rawliteral autour du contenu
        pattern = r'(const char INDEX_HTML\[\] PROGMEM = R"rawliteral\().*?(\)rawliteral";)'
        
        # On insère le nouveau contenu. 
        # Note: new_html.replace('\\', '\\\\') est nécessaire si on utilise des strings C normaux, 
        # mais ici on utilise rawliteral R"(...)", donc c'est moins critique, 
        # sauf si le HTML contient lui-même des séquences qui casseraient le rawliteral.
        # On utilise une fonction de remplacement pour éviter les problèmes d'échappement regex
        
        header_start = header_content.split('const char INDEX_HTML[] PROGMEM = R"rawliteral(')[0]
        header_end = header_content.split(')rawliteral";')[1]
        
        updated_header = header_start + 'const char INDEX_HTML[] PROGMEM = R"rawliteral(\n' + new_html + '\n)rawliteral";' + header_end

        with open(Z_UI_PATH, "w", encoding="utf-8") as f:
            f.write(updated_header)
            
        print("✔️  Succès : Le fichier z_ui.h a été mis à jour avec le nouveau code HTML.")
        print("    Tu peux maintenant recompiler (Build) et flasher l'ESP32-S3 !")
        
    except Exception as e:
        print(f"❌ Erreur critique : {e}")

if __name__ == "__main__":
    inject_html()
