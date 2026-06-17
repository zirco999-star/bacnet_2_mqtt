# Rapport de Test d'Activation en Masse des Objets BACnet - HA

- **Date** : 2026-06-15 17:24:03
- **Périphérique Cible** : ECB_203 (Device ID: 364004)
- **Total Objets Testés** : 89
- **Succès de Création HA** : 89 / 89
- **Échecs** : 0
- **Statut Final** : **Tous les objets restent activés (poll: true) pour les tests suivants.**

## Synthèse de la Découverte en Masse

| # | Type | Instance | Nom Objet | Découvert dans HA | Entity ID | Valeur HA | Remarques |
|---|------|----------|-----------|--------------------|-----------|-----------|-----------|
| 1 | AI | 1 | Temp_bureau | ✅ Oui | `sensor.ecb_203_temp_bureau_2` | `26.11` |  |
| 2 | AI | 2 | Offset_bureau | ✅ Oui | `sensor.ecb_203_offset_bureau_2` | `0.14` |  |
| 3 | AI | 3 | Temp_chambre | ✅ Oui | `sensor.ecb_203_temp_chambre_2` | `25.76` |  |
| 4 | AI | 4 | Offset_chambre | ✅ Oui | `sensor.ecb_203_offset_chambre_2` | `0.54` |  |
| 5 | AI | 5 | Sonde4-S | ✅ Oui | `sensor.ecb_203_sonde4_s_2` | `unknown` |  |
| 6 | AI | 6 | Offset4 | ✅ Oui | `sensor.ecb_203_offset4_2` | `unknown` |  |
| 7 | AI | 1001 | Analog_Input 1001 | ✅ Oui | `sensor.ecb_203_analog_input_1001_2` | `unknown` |  |
| 8 | AI | 1002 | Wireless Sensor 2 SpaceTemp | ✅ Oui | `sensor.ecb_203_wireless_sensor_2_spacetemp_2` | `327.00` |  |
| 9 | AI | 1003 | Wireless Sensor 2 SetPoint | ✅ Oui | `sensor.ecb_203_wireless_sensor_2_setpoint_2` | `unknown` |  |
| 10 | AI | 1004 | Wireless Sensor 3 SpaceTemp | ✅ Oui | `sensor.ecb_203_wireless_sensor_3_spacetemp_2` | `327.00` |  |
| 11 | AI | 1005 | AI:1005 | ✅ Oui | `sensor.ecb_203_ai_1005_2` | `unknown` |  |
| 12 | AI | 1009 | Wireless Sensor 1 SpaceTemp | ✅ Oui | `sensor.ecb_203_wireless_sensor_1_spacetemp_2` | `327.00` |  |
| 13 | AI | 5001 | Temp_salon | ✅ Oui | `sensor.ecb_203_temp_salon_2` | `25.75` |  |
| 14 | AI | 5002 | ComSensor 1 Humid | ✅ Oui | `sensor.ecb_203_comsensor_1_humid_2` | `unavailable` |  |
| 15 | AI | 5003 | ComSensor 1 CO2 | ✅ Oui | `sensor.ecb_203_comsensor_1_co2_2` | `unavailable` |  |
| 16 | AO | 1 | VoletAir1 | ✅ Oui | `number.ecb_203_voletair1_2` | `unknown` |  |
| 17 | AO | 2 | VoletAir2 | ✅ Oui | `number.ecb_203_voletair2_2` | `unknown` |  |
| 18 | AO | 3 | VoletAir3 | ✅ Oui | `number.ecb_203_voletair3_2` | `unknown` |  |
| 19 | AO | 4 | VoletAir4 | ✅ Oui | `number.ecb_203_voletair4_2` | `unknown` |  |
| 20 | AO | 5 | Vanne | ✅ Oui | `number.ecb_203_vanne_2` | `unknown` |  |
| 21 | AO | 7 | Ventilateur | ✅ Oui | `number.ecb_203_ventilateur_2` | `unknown` |  |
| 22 | AO | 8 | Ventil UTA2 | ✅ Oui | `number.ecb_203_ventil_uta2_2` | `unknown` |  |
| 23 | AV | 1 | ConsigneTemperature1 | ✅ Oui | `number.ecb_203_consignetemperature1_2` | `unknown` |  |
| 24 | AV | 2 | ConsigneTemperature2 | ✅ Oui | `number.ecb_203_consignetemperature2_2` | `20.0` |  |
| 25 | AV | 3 | ConsigneTemperature3 | ✅ Oui | `number.ecb_203_consignetemperature3_2` | `unknown` |  |
| 26 | AV | 4 | ConsigneTemperature4 | ✅ Oui | `number.ecb_203_consignetemperature4_2` | `20.0` |  |
| 27 | AV | 5 | Hysteresis regulation | ✅ Oui | `number.ecb_203_hysteresis_regulation_2` | `1.0` |  |
| 28 | AV | 6 | MaxVentilateur | ✅ Oui | `number.ecb_203_maxventilateur_2` | `70.0` |  |
| 29 | AV | 7 | PoidsVolet1 | ✅ Oui | `number.ecb_203_poidsvolet1_2` | `50.0` |  |
| 30 | AV | 8 | PoidsVolet2 | ✅ Oui | `number.ecb_203_poidsvolet2_2` | `50.0` |  |
| 31 | AV | 9 | PoidsVolet3 | ✅ Oui | `number.ecb_203_poidsvolet3_2` | `50.0` |  |
| 32 | AV | 10 | PoidsVolet4 | ✅ Oui | `number.ecb_203_poidsvolet4_2` | `50.0` |  |
| 33 | AV | 11 | VentilLimitebasse | ✅ Oui | `number.ecb_203_ventillimitebasse_2` | `50.0` |  |
| 34 | AV | 12 | TempoVolet | ✅ Oui | `number.ecb_203_tempovolet_2` | `40.0` |  |
| 35 | AV | 13 | ConsigneTemperatureEco1 | ✅ Oui | `number.ecb_203_consignetemperatureeco1_2` | `17.0` |  |
| 36 | AV | 14 | ConsigneTemperatureEco2 | ✅ Oui | `number.ecb_203_consignetemperatureeco2_2` | `17.0` |  |
| 37 | AV | 15 | ConsigneTemperatureEco3 | ✅ Oui | `number.ecb_203_consignetemperatureeco3_2` | `17.0` |  |
| 38 | AV | 16 | ConsigneTemperatureEco4 | ✅ Oui | `number.ecb_203_consignetemperatureeco4_2` | `17.0` |  |
| 39 | AV | 17 | Choix_T1T4_grandT4 | ✅ Oui | `number.ecb_203_choix_t1t4_grandt4_2` | `unknown` |  |
| 40 | AV | 18 | TempHorsGel | ✅ Oui | `number.ecb_203_temphorsgel_2` | `8.0` |  |
| 41 | AV | 19 | Version | ✅ Oui | `sensor.z1rc0n1um_app_version` | `2026.6.0` |  |
| 42 | AV | 20 | Subversion | ✅ Oui | `number.ecb_203_subversion_2` | `93.0` |  |
| 43 | AV | 21 | WirelessTemp2 | ✅ Oui | `number.ecb_203_wirelesstemp2_2` | `unknown` |  |
| 44 | AV | 22 | WirelessOffset2 | ✅ Oui | `number.ecb_203_wirelessoffset2_2` | `-10.0` |  |
| 45 | AV | 23 | WirelessTemp3 | ✅ Oui | `number.ecb_203_wirelesstemp3_2` | `unknown` |  |
| 46 | AV | 24 | WirelessOffset3 | ✅ Oui | `number.ecb_203_wirelessoffset3_2` | `-10.0` |  |
| 47 | AV | 25 | WirelessTemp4 | ✅ Oui | `number.ecb_203_wirelesstemp4_2` | `unknown` |  |
| 48 | AV | 26 | WirelessOffset4 | ✅ Oui | `number.ecb_203_wirelessoffset4_2` | `-10.0` |  |
| 49 | AV | 27 | TempFinale1 | ✅ Oui | `number.ecb_203_tempfinale1_2` | `25.75` |  |
| 50 | AV | 28 | TempFinale2 | ✅ Oui | `number.ecb_203_tempfinale2_2` | `26.11` |  |
| 51 | AV | 29 | TempFinale3 | ✅ Oui | `number.ecb_203_tempfinale3_2` | `25.76` |  |
| 52 | AV | 30 | TempFinale4 | ✅ Oui | `number.ecb_203_tempfinale4_2` | `unknown` |  |
| 53 | AV | 31 | OffsetFinal1 | ✅ Oui | `number.ecb_203_offsetfinal1_2` | `1.0` |  |
| 54 | AV | 32 | OffsetFinal2 | ✅ Oui | `number.ecb_203_offsetfinal2_2` | `0.14` |  |
| 55 | AV | 33 | OffsetFinal3 | ✅ Oui | `number.ecb_203_offsetfinal3_2` | `0.54` |  |
| 56 | AV | 34 | OffsetFinal4 | ✅ Oui | `number.ecb_203_offsetfinal4_2` | `unknown` |  |
| 57 | AV | 35 | ConsigneFinale1 | ✅ Oui | `number.ecb_203_consignefinale1_2` | `8.0` |  |
| 58 | AV | 36 | ConsigneFinale2 | ✅ Oui | `number.ecb_203_consignefinale2_2` | `8.0` |  |
| 59 | AV | 37 | ConsigneFinale3 | ✅ Oui | `number.ecb_203_consignefinale3_2` | `8.0` |  |
| 60 | AV | 38 | ConsigneFinale4 | ✅ Oui | `number.ecb_203_consignefinale4_2` | `8.0` |  |
| 61 | AV | 39 | Versionfordisplay | ✅ Oui | `number.ecb_203_versionfordisplay_2` | `0.93` |  |
| 62 | AV | 40 | NbrePieces | ✅ Oui | `number.ecb_203_nbrepieces_2` | `3.0` |  |
| 63 | AV | 41 | Ipadoffset | ✅ Oui | `number.ecb_203_ipadoffset_2` | `unknown` |  |
| 64 | AV | 42 | LowOffsetLimit | ✅ Oui | `number.ecb_203_lowoffsetlimit_2` | `-4.0` |  |
| 65 | AV | 43 | HighOffsetLimit | ✅ Oui | `number.ecb_203_highoffsetlimit_2` | `4.0` |  |
| 66 | AV | 44 | ConsigneTemperatureEcoETE1 | ✅ Oui | `number.ecb_203_consignetemperatureecoete1_2` | `27.0` |  |
| 67 | AV | 45 | ConsigneTemperatureEcoETE2 | ✅ Oui | `number.ecb_203_consignetemperatureecoete2_2` | `27.0` |  |
| 68 | AV | 46 | ConsigneTemperatureEcoETE3 | ✅ Oui | `number.ecb_203_consignetemperatureecoete3_2` | `27.0` |  |
| 69 | AV | 47 | ConsigneTemperatureEcoETE4 | ✅ Oui | `number.ecb_203_consignetemperatureecoete4_2` | `27.0` |  |
| 70 | AV | 48 | MinDelay | ✅ Oui | `number.ecb_203_mindelay_2` | `unknown` |  |
| 71 | AV | 49 | offset2ipad | ✅ Oui | `number.ecb_203_offset2ipad_2` | `1.0` |  |
| 72 | AV | 50 | offset3ipad | ✅ Oui | `number.ecb_203_offset3ipad_2` | `1.0` |  |
| 73 | AV | 51 | offset4ipad | ✅ Oui | `number.ecb_203_offset4ipad_2` | `unknown` |  |
| 74 | AV | 52 | offset1ipad | ✅ Oui | `number.ecb_203_offset1ipad_2` | `unknown` |  |
| 75 | AV | 53 | CoeffOffset | ✅ Oui | `number.ecb_203_coeffoffset_2` | `100.0` |  |
| 76 | BI | 5004 | ComSensor 1 Motion | ✅ Oui | `binary_sensor.ecb_203_comsensor_1_motion_2` | `unknown` |  |
| 77 | BO | 6 | Vanne BO:6 | ✅ Oui | `switch.ecb_203_vanne_bo_6` | `unknown` |  |
| 78 | BV | 1 | DemandeChaud1 | ✅ Oui | `switch.ecb_203_demandechaud1_2` | `unknown` |  |
| 79 | BV | 2 | DemandeChaud2 | ✅ Oui | `switch.ecb_203_demandechaud2_2` | `unknown` |  |
| 80 | BV | 3 | DemandeChaud3 | ✅ Oui | `switch.ecb_203_demandechaud3_2` | `unknown` |  |
| 81 | BV | 4 | DemandeChaud4 | ✅ Oui | `switch.ecb_203_demandechaud4_2` | `unknown` |  |
| 82 | BV | 5 | DemandeFroid1 | ✅ Oui | `switch.ecb_203_demandefroid1_2` | `unknown` |  |
| 83 | BV | 6 | DemandeFroid2 | ✅ Oui | `switch.ecb_203_demandefroid2_2` | `unknown` |  |
| 84 | BV | 7 | DemandeFroid3 | ✅ Oui | `switch.ecb_203_demandefroid3_2` | `unknown` |  |
| 85 | BV | 8 | DemandeFroid4 | ✅ Oui | `switch.ecb_203_demandefroid4_2` | `unknown` |  |
| 86 | BV | 11 | EnableChgOver | ✅ Oui | `switch.ecb_203_enablechgover_2` | `unknown` |  |
| 87 | MSV | 1 | ModeConfortEco | ✅ Oui | `select.ecb_203_modeconforteco_2` | `Confort` |  |
| 88 | MSV | 2 | ModeChangeOver | ✅ Oui | `select.ecb_203_modechangeover_2` | `Chaud` |  |
| 89 | MSV | 50 | ModeConfortEcoIpad | ✅ Oui | `select.ecb_203_modeconfortecoipad_2` | `Arret` |  |
