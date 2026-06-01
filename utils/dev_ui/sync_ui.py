import os
import sys

# Chemins des fichiers
UI_SOURCE = "utils/dev_ui/index.html"
UI_HEADER = "src/z_ui.h"

def sync():
    if not os.path.exists(UI_SOURCE):
        print(f"❌ Erreur : {UI_SOURCE} introuvable.")
        return

    print(f"🔄 Synchronisation de {UI_SOURCE} vers {UI_HEADER}...")

    with open(UI_SOURCE, "r", encoding="utf-8") as f:
        content = f.read()

    # Nettoyage minimal (optionnel, on garde les raw literals pour la simplicité)
    # On s'assure que le contenu ne contient pas la chaine de fin du raw literal
    if ")rawliteral\"" in content:
        print("❌ Erreur : Le contenu de l'HTML contient la séquence réservée ')rawliteral\"'.")
        return

    new_header_content = f"""#ifndef Z_UI_H
#define Z_UI_H

#include <Arduino.h>

const char INDEX_HTML[] PROGMEM = R"rawliteral({content})rawliteral";

#endif
"""

    with open(UI_HEADER, "w", encoding="utf-8") as f:
        f.write(new_header_content)

    print(f"✅ Succès ! {UI_HEADER} mis à jour.")
    print("🚀 Vous pouvez maintenant compiler avec ./utils/compil.sh")

if __name__ == "__main__":
    # On se place à la racine du projet si on est dans utils/dev_ui
    if os.path.basename(os.getcwd()) == "dev_ui":
        os.chdir("../..")
    sync()
