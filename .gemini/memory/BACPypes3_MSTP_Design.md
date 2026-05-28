# 🧠 Architecture BACpypes3 - Note Technique

## 🏗 Structure des Couches (Stack)
BACpypes3 utilise un pattern **Client/Server** asynchrone pour lier les couches.
Chaque couche hérite soit de `Client[T]`, soit de `Server[T]`, soit des deux.

1.  **Application Layer** (`app.py`): Gère les objets locaux et les services (Who-Is, ReadProperty).
2.  **ASAP** (`ApplicationServiceAccessPoint`): Pont entre l'Application et le Réseau.
3.  **Network Layer** (`netservice.py`): Gère le routage et les adaptateurs.
4.  **Network Adapter**: Binds l'application à une interface réseau spécifique.
5.  **Link Layer**: Gère le medium (IPv4, IPv6, VLAN, MSTP).

## 🔗 Mécanisme de Binding
La fonction `bind(*layers)` lie les couches du haut vers le bas.
- Un `Client` appelle `self.request(pdu)` -> appelle `server.indication(pdu)`.
- Un `Server` appelle `self.response(pdu)` -> appelle `client.confirmation(pdu)`.

## 📍 État du Support MS/TP
- BACpypes3 ne possède pas de machine d'état MS/TP native en Python (actuellement).
- La branche `mstp` utilise un agent externe ("Misty") via un socket UNIX/UDP.
- Pour un pont TCP <-> RS485 "dumb", nous devons implémenter la FSM MS/TP (Clause 9) en Python.

## 🛠 Plan pour le Transport TCP MSTP
Nous allons créer une stack personnalisée compatible BACpypes3 :
1.  **`TCPTransport` (Client)**: Gère la socket `asyncio`.
2.  **`MSTPFramer` (Server/Client)**: Gère le préambule `55 FF`, les types de trames et les CRC8/16.
3.  **`MSTPStateMachine` (Server/Client)**: Gère le jeton (Token), les Poll-For-Master et les timeouts.
4.  **`MSTPLinkLayer` (Server)**: Expose les PDUs à BACpypes3.

## 📝 Configuration standard (Best Practices)
- Utiliser `asyncio.Queue` pour le découplage.
- Respecter les timeouts de la norme (T_reply_timeout = 250ms par défaut).
- Utiliser `MSTPAddress` pour le source/destination dans les PDUs.
