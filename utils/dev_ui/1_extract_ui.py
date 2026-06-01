import os
import re

# Chemins relatifs depuis le dossier utils/dev_ui/
Z_UI_PATH = os.path.join(os.path.dirname(__file__), "../../src/z_ui.h")
HTML_OUTPUT = "index.html"

def extract_html():
    print("[1/3] Extraction de l'UI depuis le firmware...")
    try:
        if not os.path.exists(Z_UI_PATH):
            print(f"❌ Erreur : {Z_UI_PATH} introuvable.")
            return

        with open(Z_UI_PATH, "r", encoding="utf-8") as f:
            content = f.read()
            
        # Recherche du contenu entre R"rawliteral( et )rawliteral";
        match = re.search(r'R"rawliteral\((.*?)\)rawliteral";', content, re.DOTALL)
        if match:
            html_content = match.group(1).strip()
            with open(HTML_OUTPUT, "w", encoding="utf-8") as f:
                f.write(html_content)
            print(f"✔️  Succès : Interface extraite et sauvegardée dans {HTML_OUTPUT}")
        else:
            print("❌ Erreur : Balises rawliteral introuvables dans z_ui.h.")
    except Exception as e:
        print(f"❌ Erreur critique : {e}")

if __name__ == "__main__":
    extract_html()
