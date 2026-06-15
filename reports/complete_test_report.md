# Rapport de Test d'Intégration Complet - BACnet2MQTT

Ce rapport détaille le cycle d'intégration et d'audit pour la passerelle **BACnet2MQTT** (Waveshare ESP32-S3).

## 1. Métriques de la Passerelle au moment du Test
- **Version du Firmware** : `v6.9.6`
- **Uptime de la Passerelle** : `122s`
- **Signal Wi-Fi (RSSI)** : `-52 dBm`
- **Mémoire Heap Libre** : `79 octets`
- **Jetons MS/TP vus (Liveness)** : `0`

## 2. Résumé de l'Audit Home Assistant
- **Total Objets Evalués** : 89
- **Entités Validées (OK)** : 42
- **Entités en Échec/Manquantes (ERROR/MISSING)** : 47
- **dont Entités de Polling Actif Indisponibles** : 15

> [!NOTE]
> Les entités marquées en **MISSING** ou **unknown** peuvent correspondre à des capteurs physiques absents ou déconnectés sur le bus de l'automate ECB-203 (ex: ComSensor 1 CO2, ComSensor 1 Humid, VoletAir1-4) pour lesquels le polling n'a pas pu acquérir de valeur réelle sur le réseau MS/TP.

---

## 3. Détail par Objet et État HA

| OID (Type:Inst) | Nom de l'Objet | Composant / Entité HA | Polling | État HA | Statut | Anomalies / Erreurs |
| :--- | :--- | :--- | :---: | :---: | :---: | :--- |
| AI:1 | Temp_bureau | `sensor.ecb_203_temp_bureau_2` | Actif | `26.11` | ✅ OK | - |
| AI:2 | Offset_bureau | `sensor.ecb_203_offset_bureau_2` | Actif | `0.14` | ✅ OK | - |
| AI:3 | Temp_chambre | `sensor.ecb_203_temp_chambre_2` | Actif | `25.76` | ✅ OK | - |
| AI:4 | Offset_chambre | `sensor.ecb_203_offset_chambre_2` | Actif | `0.54` | ✅ OK | - |
| AI:5 | Sonde4-S | `sensor.ecb_203_sonde4_s_2` | Inactif | `unknown` | ❌ KO | Entité indisponible (unknown/unavailable) |
| AI:6 | Offset4 | `sensor.ecb_203_offset4_2` | Inactif | `unknown` | ❌ KO | Entité indisponible (unknown/unavailable) |
| AI:1001 | Wireless Sensor 2 SetPoint | `sensor.ecb_203_wireless_sensor_2_setpoint` | Actif | `-10.00` | ✅ OK | - |
| AI:1002 | Wireless Sensor 2 SpaceTemp | `sensor.ecb_203_wireless_sensor_2_spacetemp_2` | Actif | `327.00` | ✅ OK | - |
| AI:1003 | Wireless Sensor 3 SetPoint | `sensor.ecb_203_wireless_sensor_2_setpoint` | Actif | `-10.00` | ✅ OK | - |
| AI:1004 | Wireless Sensor 3 SpaceTemp | `sensor.ecb_203_wireless_sensor_3_spacetemp_2` | Actif | `327.00` | ✅ OK | - |
| AI:1005 | Wireless Sensor 1 SetPoint | `sensor.ecb_203_ai_1005` | Actif | `-10.00` | ✅ OK | - |
| AI:1009 | Wireless Sensor 1 SpaceTemp | `sensor.ecb_203_wireless_sensor_1_spacetemp_2` | Actif | `327.00` | ✅ OK | - |
| AI:5001 | Temp_salon | `sensor.ecb_203_temp_salon_2` | Actif | `25.75` | ✅ OK | - |
| AI:5002 | ComSensor 1 Humid | `sensor.ecb_203_comsensor_1_humid_2` | Inactif | `unavailable` | ❌ KO | Entité indisponible (unknown/unavailable) |
| AI:5003 | ComSensor 1 CO2 | `sensor.ecb_203_comsensor_1_co2_2` | Inactif | `unavailable` | ❌ KO | Entité indisponible (unknown/unavailable) |
| AO:1 | VoletAir1 | `number.ecb_203_voletair1_2` | Actif | `unknown` | ❌ KO | Entité indisponible (unknown/unavailable) |
| AO:2 | VoletAir2 | `number.ecb_203_voletair2_2` | Actif | `unknown` | ❌ KO | Entité indisponible (unknown/unavailable) |
| AO:3 | VoletAir3 | `number.ecb_203_voletair3_2` | Actif | `unknown` | ❌ KO | Entité indisponible (unknown/unavailable) |
| AO:4 | VoletAir4 | `number.ecb_203_voletair4_2` | Inactif | `unknown` | ❌ KO | Entité indisponible (unknown/unavailable) |
| AO:5 | Vanne | `number.ecb_203_vanne_2` | Actif | `unknown` | ❌ KO | Entité indisponible (unknown/unavailable) |
| AO:7 | Ventilateur | `number.ecb_203_ventilateur_2` | Actif | `unknown` | ❌ KO | Entité indisponible (unknown/unavailable) |
| AO:8 | Ventil UTA2 | `number.ecb_203_ventil_uta2_2` | Actif | `unknown` | ❌ KO | Entité indisponible (unknown/unavailable) |
| AV:1 | ConsigneTemperature1 | `number.ecb_203_consignetemperature1` | Actif | `21.0` | ✅ OK | - |
| AV:2 | ConsigneTemperature2 | `number.ecb_203_consignetemperature2_2` | Actif | `20.0` | ✅ OK | - |
| AV:3 | ConsigneTemperature3 | `number.ecb_203_consignetemperature3` | Actif | `20.0` | ✅ OK | - |
| AV:4 | ConsigneTemperature4 | `number.ecb_203_consignetemperature4_2` | Inactif | `20.0` | ❌ KO | Valeur incohérente : HA=20.0 != Gateway=0 |
| AV:5 | Hysteresis regulation | `number.ecb_203_hysteresis_regulation_2` | Actif | `1.0` | ❌ KO | Min incohérent : HA=-100.0 != Gateway=0, Max incohérent : HA=100.0 != Gateway=2, Step incohérent : HA=1.0 != Gateway=0.5 |
| AV:6 | MaxVentilateur | `number.ecb_203_maxventilateur_2` | Actif | `70.0` | ❌ KO | Min incohérent : HA=-100.0 != Gateway=0 |
| AV:7 | PoidsVolet1 | `number.ecb_203_poidsvolet1_2` | Actif | `50.0` | ✅ OK | - |
| AV:8 | PoidsVolet2 | `number.ecb_203_poidsvolet2_2` | Actif | `50.0` | ✅ OK | - |
| AV:9 | PoidsVolet3 | `number.ecb_203_poidsvolet3_2` | Actif | `50.0` | ✅ OK | - |
| AV:10 | PoidsVolet4 | `number.ecb_203_poidsvolet4_2` | Inactif | `50.0` | ❌ KO | Valeur incohérente : HA=50.0 != Gateway=0 |
| AV:11 | VentilLimitebasse | `number.ecb_203_ventillimitebasse_2` | Actif | `50.0` | ✅ OK | - |
| AV:12 | TempoVolet | `number.ecb_203_tempovolet_2` | Actif | `40.0` | ✅ OK | - |
| AV:13 | ConsigneTemperatureEco1 | `number.ecb_203_consignetemperatureeco1_2` | Actif | `17.0` | ✅ OK | - |
| AV:14 | ConsigneTemperatureEco2 | `number.ecb_203_consignetemperatureeco2_2` | Actif | `17.0` | ✅ OK | - |
| AV:15 | ConsigneTemperatureEco3 | `number.ecb_203_consignetemperatureeco3_2` | Actif | `17.0` | ✅ OK | - |
| AV:16 | ConsigneTemperatureEco4 | `number.ecb_203_consignetemperatureeco4_2` | Inactif | `17.0` | ❌ KO | Valeur incohérente : HA=17.0 != Gateway=0 |
| AV:17 | Choix_T1T4_grandT4 | `number.ecb_203_choix_t1t4_grandt4_2` | Inactif | `unknown` | ❌ KO | Entité indisponible (unknown/unavailable) |
| AV:18 | TempHorsGel | `number.ecb_203_temphorsgel_2` | Actif | `8.0` | ✅ OK | - |
| AV:19 | Version | `sensor.bacnet2mqtt_gateway_gateway_version` | Inactif | `v6.9.5` | ❌ KO | Erreur de conversion de valeur HA (v6.9.5) : could not convert string to float: 'v6.9.5' |
| AV:20 | Subversion | `number.ecb_203_subversion_2` | Inactif | `93.0` | ❌ KO | Valeur incohérente : HA=93.0 != Gateway=0 |
| AV:21 | WirelessTemp2 | `number.ecb_203_wirelesstemp2_2` | Inactif | `unknown` | ❌ KO | Entité indisponible (unknown/unavailable) |
| AV:22 | WirelessOffset2 | `number.ecb_203_wirelessoffset2_2` | Inactif | `-10.0` | ❌ KO | Valeur incohérente : HA=-10.0 != Gateway=0 |
| AV:23 | WirelessTemp3 | `number.ecb_203_wirelesstemp3_2` | Inactif | `unknown` | ❌ KO | Entité indisponible (unknown/unavailable) |
| AV:24 | WirelessOffset3 | `number.ecb_203_wirelessoffset3_2` | Inactif | `-10.0` | ❌ KO | Valeur incohérente : HA=-10.0 != Gateway=0 |
| AV:25 | WirelessTemp4 | `number.ecb_203_wirelesstemp4_2` | Inactif | `unknown` | ❌ KO | Entité indisponible (unknown/unavailable) |
| AV:26 | WirelessOffset4 | `number.ecb_203_wirelessoffset4_2` | Inactif | `-10.0` | ❌ KO | Valeur incohérente : HA=-10.0 != Gateway=0 |
| AV:27 | TempFinale1 | `number.ecb_203_tempfinale1_2` | Actif | `25.75` | ✅ OK | - |
| AV:28 | TempFinale2 | `number.ecb_203_tempfinale2_2` | Actif | `26.11` | ✅ OK | - |
| AV:29 | TempFinale3 | `number.ecb_203_tempfinale3_2` | Actif | `25.76` | ✅ OK | - |
| AV:30 | TempFinale4 | `number.ecb_203_tempfinale4_2` | Inactif | `unknown` | ❌ KO | Entité indisponible (unknown/unavailable) |
| AV:31 | OffsetFinal1 | `number.ecb_203_offsetfinal1_2` | Actif | `1.0` | ✅ OK | - |
| AV:32 | OffsetFinal2 | `number.ecb_203_offsetfinal2_2` | Actif | `0.14` | ✅ OK | - |
| AV:33 | OffsetFinal3 | `number.ecb_203_offsetfinal3_2` | Actif | `0.54` | ✅ OK | - |
| AV:34 | OffsetFinal4 | `number.ecb_203_offsetfinal4_2` | Inactif | `unknown` | ❌ KO | Entité indisponible (unknown/unavailable) |
| AV:35 | ConsigneFinale1 | `number.ecb_203_consignefinale1_2` | Actif | `8.0` | ✅ OK | - |
| AV:36 | ConsigneFinale2 | `number.ecb_203_consignefinale2_2` | Actif | `8.0` | ✅ OK | - |
| AV:37 | ConsigneFinale3 | `number.ecb_203_consignefinale3_2` | Actif | `8.0` | ✅ OK | - |
| AV:38 | ConsigneFinale4 | `number.ecb_203_consignefinale4_2` | Inactif | `8.0` | ❌ KO | Valeur incohérente : HA=8.0 != Gateway=0 |
| AV:39 | Versionfordisplay | `number.ecb_203_versionfordisplay_2` | Inactif | `0.93` | ❌ KO | Valeur incohérente : HA=0.93 != Gateway=0 |
| AV:40 | NbrePieces | `number.ecb_203_nbrepieces_2` | Inactif | `3.0` | ❌ KO | Valeur incohérente : HA=3.0 != Gateway=0 |
| AV:41 | Ipadoffset | `number.ecb_203_ipadoffset_2` | Inactif | `unknown` | ❌ KO | Entité indisponible (unknown/unavailable) |
| AV:42 | LowOffsetLimit | `number.ecb_203_lowoffsetlimit_2` | Actif | `-4.0` | ✅ OK | - |
| AV:43 | HighOffsetLimit | `number.ecb_203_highoffsetlimit_2` | Actif | `4.0` | ✅ OK | - |
| AV:44 | ConsigneTemperatureEcoETE1 | `number.ecb_203_consignetemperatureecoete1_2` | Actif | `27.0` | ✅ OK | - |
| AV:45 | ConsigneTemperatureEcoETE2 | `number.ecb_203_consignetemperatureecoete2_2` | Actif | `27.0` | ✅ OK | - |
| AV:46 | ConsigneTemperatureEcoETE3 | `number.ecb_203_consignetemperatureecoete3_2` | Actif | `27.0` | ✅ OK | - |
| AV:47 | ConsigneTemperatureEcoETE4 | `number.ecb_203_consignetemperatureecoete4_2` | Inactif | `27.0` | ❌ KO | Valeur incohérente : HA=27.0 != Gateway=0 |
| AV:48 | MinDelay | `number.ecb_203_mindelay_2` | Actif | `unknown` | ❌ KO | Entité indisponible (unknown/unavailable) |
| AV:49 | offset2ipad | `number.ecb_203_offset2ipad_2` | Actif | `1.0` | ✅ OK | - |
| AV:50 | offset3ipad | `number.ecb_203_offset3ipad_2` | Actif | `1.0` | ✅ OK | - |
| AV:51 | offset4ipad | `number.ecb_203_offset4ipad_2` | Inactif | `unknown` | ❌ KO | Entité indisponible (unknown/unavailable) |
| AV:52 | offset1ipad | `number.ecb_203_offset1ipad_2` | Actif | `unknown` | ❌ KO | Entité indisponible (unknown/unavailable) |
| AV:53 | CoeffOffset | `number.ecb_203_coeffoffset_2` | Actif | `100.0` | ❌ KO | Min incohérent : HA=-100.0 != Gateway=0 |
| BI:5004 | ComSensor 1 Motion | `binary_sensor.ecb_203_comsensor_1_motion_2` | Inactif | `unknown` | ❌ KO | Entité indisponible (unknown/unavailable) |
| BO:6 | Vanne BO:6 | `switch.ecb_203_vanne` | Actif | `on` | ❌ KO | Valeur incohérente : HA=on != Gateway=off (val=0) |
| BV:1 | DemandeChaud1 | `switch.ecb_203_demandechaud1_2` | Actif | `unknown` | ❌ KO | Entité indisponible (unknown/unavailable) |
| BV:2 | DemandeChaud2 | `switch.ecb_203_demandechaud2_2` | Actif | `unknown` | ❌ KO | Entité indisponible (unknown/unavailable) |
| BV:3 | DemandeChaud3 | `switch.ecb_203_demandechaud3_2` | Actif | `unknown` | ❌ KO | Entité indisponible (unknown/unavailable) |
| BV:4 | DemandeChaud4 | `switch.ecb_203_demandechaud4_2` | Inactif | `unknown` | ❌ KO | Entité indisponible (unknown/unavailable) |
| BV:5 | DemandeFroid1 | `switch.ecb_203_demandefroid1_2` | Actif | `unknown` | ❌ KO | Entité indisponible (unknown/unavailable) |
| BV:6 | DemandeFroid2 | `switch.ecb_203_demandefroid2_2` | Actif | `unknown` | ❌ KO | Entité indisponible (unknown/unavailable) |
| BV:7 | DemandeFroid3 | `switch.ecb_203_demandefroid3_2` | Actif | `unknown` | ❌ KO | Entité indisponible (unknown/unavailable) |
| BV:8 | DemandeFroid4 | `switch.ecb_203_demandefroid4_2` | Inactif | `unknown` | ❌ KO | Entité indisponible (unknown/unavailable) |
| BV:11 | EnableChgOver | `switch.ecb_203_enablechgover_2` | Actif | `unknown` | ❌ KO | Entité indisponible (unknown/unavailable) |
| MSV:1 | ModeConfortEco | `select.ecb_203_modeconforteco_2` | Actif | `Confort` | ✅ OK | - |
| MSV:2 | ModeChangeOver | `select.ecb_203_modechangeover_2` | Actif | `Chaud` | ✅ OK | - |
| MSV:50 | ModeConfortEcoIpad | `select.ecb_203_modeconfortecoipad_2` | Actif | `Arret` | ✅ OK | - |
