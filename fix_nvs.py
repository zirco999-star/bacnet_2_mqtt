import os

file_path = '/mnt/save/bacnet_2_mqtt/src/z_nvs.cpp'
with open(file_path, 'r', encoding='utf-8') as f:
    content = f.read()

content = content.replace('head.name', 'head.cName')
content = content.replace('page.objects[i].val', 'page.objects[i].ulVal')
content = content.replace('page.objects[i].poll', 'page.objects[i].xEnabled')
content = content.replace('page.objects[i].states_count', 'page.objects[i].ucExpectedStatesCount')

with open(file_path, 'w', encoding='utf-8') as f:
    f.write(content)
