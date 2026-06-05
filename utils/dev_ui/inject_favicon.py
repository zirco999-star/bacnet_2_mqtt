import os

ICON_PATH = "/home/dev/bacnet_2_mqtt/data/favicon/favicon.ico"
HEADER_PATH = "/mnt/save/bacnet_2_mqtt/src/z_ui.h"

def inject_favicon():
    print(f"🔄 Converting {ICON_PATH} to C array...")
    
    if not os.path.exists(ICON_PATH):
        print(f"❌ Error: {ICON_PATH} not found.")
        return

    with open(ICON_PATH, "rb") as f:
        data = f.read()
    
    data_len = len(data)
    hex_data = []
    for i in range(0, data_len, 16):
        chunk = data[i:i+16]
        hex_line = ", ".join([f"0x{b:02x}" for b in chunk])
        hex_data.append("    " + hex_line)
    
    hex_body = ",\n".join(hex_data)
    
    with open(HEADER_PATH, "r", encoding="utf-8") as f:
        content = f.read()

    # Define the new constants
    favicon_def = f"const uint8_t favicon_ico[] PROGMEM = {{\n{hex_body}\n}};\nconst size_t favicon_ico_len = {data_len};\n"

    # Insert before #endif
    if "#endif" in content:
        if "favicon_ico[]" in content:
            # Update existing
            import re
            content = re.sub(r'const uint8_t favicon_ico\[\].*?const size_t favicon_ico_len = \d+;', favicon_def, content, flags=re.DOTALL)
        else:
            # Append before #endif
            content = content.replace("#endif", favicon_def + "\n#endif")
    else:
        content += "\n" + favicon_def

    with open(HEADER_PATH, "w", encoding="utf-8") as f:
        f.write(content)

    print(f"✅ Successfully injected favicon ({data_len} bytes) into {HEADER_PATH}")

if __name__ == "__main__":
    inject_favicon()
