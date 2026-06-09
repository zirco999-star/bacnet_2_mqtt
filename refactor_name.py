import os
import re

def refactor_files(src_dir):
    for root, _, files in os.walk(src_dir):
        for file in files:
            if file.endswith(('.cpp', '.h', '.ino')):
                file_path = os.path.join(root, file)
                with open(file_path, 'r', encoding='utf-8') as f:
                    content = f.read()

                content = re.sub(r'\bobj\.name\b', 'obj.cName', content)
                content = re.sub(r'\bo\.name\b', 'o.cName', content)
                content = re.sub(r'\bdev\.objects\[([a-zA-Z0-9_]+)\]\.name\b', r'dev.objects[\1].cName', content)
                content = re.sub(r'\bbacnet_network_cache\[([a-zA-Z0-9_]+)\]\.name\b', r'bacnet_network_cache[\1].cName', content)
                content = re.sub(r'\bpage\.objects\[([a-zA-Z0-9_]+)\]\.name\b', r'page.objects[\1].cName', content)

                with open(file_path, 'w', encoding='utf-8') as f:
                    f.write(content)

if __name__ == '__main__':
    refactor_files('/mnt/save/bacnet_2_mqtt/src')
    refactor_files('/mnt/save/bacnet_2_mqtt/bacnet_2_mqtt.ino')
