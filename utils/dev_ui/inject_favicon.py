import os
import re

# Configuration des fichiers sources et cibles
DATA_DIR = "/home/dev/bacnet_2_mqtt/data/favicon"
HEADER_PATH = "/mnt/save/bacnet_2_mqtt/src/z_ui.h"

# Liste des fichiers à injecter : { "filename": "array_name" }
FAVICONS = {
    "favicon.ico": "favicon_ico",
    "favicon-96x96.png": "favicon_96",
    "web-app-manifest-192x192.png": "favicon_192",
    "apple-touch-icon.png": "favicon_apple"
}

def file_to_c_array(filepath, array_name):
    if not os.path.exists(filepath):
        print(f"❌ Erreur : {filepath} introuvable.")
        return None, 0

    with open(filepath, "rb") as f:
        data = f.read()
    
    data_len = len(data)
    hex_data = []
    for i in range(0, data_len, 16):
        chunk = data[i:i+16]
        hex_line = ", ".join([f"0x{b:02x}" for b in chunk])
        hex_data.append("    " + hex_line)
    
    hex_body = ",\n".join(hex_data)
    
    c_def = f"const uint8_t {array_name}[] PROGMEM = {{\n{hex_body}\n}};\nconst size_t {array_name}_len = {data_len};\n"
    return c_def, data_len

def inject_favicons():
    print(f"🔄 Début de l'injection des favicons haute résolution...")
    
    full_c_defs = []
    total_bytes = 0

    for filename, array_name in FAVICONS.items():
        src_path = os.path.join(DATA_DIR, filename)
        c_def, length = file_to_c_array(src_path, array_name)
        if c_def:
            full_c_defs.append(c_def)
            total_bytes += length
            print(f"  ✅ {filename} -> {array_name} ({length} octets)")

    if not full_c_defs:
        print("❌ Aucune donnée à injecter.")
        return

    # Lecture du header actuel
    with open(HEADER_PATH, "r", encoding="utf-8") as f:
        content = f.read()

    # On prépare le bloc complet
    all_defs_block = "\n" + "\n".join(full_c_defs)

    # Nettoyage des anciennes définitions si elles existent
    # On cherche les blocs complets favicon_ico, favicon_96, etc.
    content = re.sub(r'const uint8_t favicon_.*?\[\] PROGMEM = \{.*?\};\s*const size_t favicon_.*?_len = \d+;', '', content, flags=re.DOTALL)
    
    # Injection avant le #endif final
    if "#endif" in content:
        # On remplace le dernier #endif
        parts = content.rsplit("#endif", 1)
        content = parts[0] + all_defs_block + "\n#endif" + parts[1]
    else:
        content += "\n" + all_defs_block

    # Nettoyage des lignes vides multiples
    content = re.sub(r'\n\s*\n\s*\n', '\n\n', content)

    with open(HEADER_PATH, "w", encoding="utf-8") as f:
        f.write(content)

    print(f"✔️  Succès : {len(full_c_defs)} icônes injectées ({total_bytes} octets au total) dans {HEADER_PATH}")

if __name__ == "__main__":
    inject_favicons()
