import requests
try:
    resp = requests.get('http://192.168.1.50/api/objects?did=364004', timeout=5)
    objects = resp.json()
    enabled_objs = [o for o in objects if o.get('enabled')]
    print(f"Total enabled objects: {len(enabled_objs)}")
    for o in enabled_objs:
        name = o.get('name', 'Unknown')
        if name.lower() in ['poidsvolet', 'vanne', 'voletairx'] or True:
            # We'll just print a summary of the first few, and specifically look for the ones mentioned
            print(f"ID: {o.get('type')}:{o.get('inst')}, Name: {name}, val: {o.get('val')}, min: {o.get('min')}, max: {o.get('max')}, step: {o.get('step')}")
except Exception as e:
    print("Error:", e)
