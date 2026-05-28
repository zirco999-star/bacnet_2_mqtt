#!/usr/bin/env python3
"""
@file       run_all_tests.py
@brief      Lance l'intégralité de la suite de tests autonomes.
@details    Démarre le simulateur MS/TP, exécute les tests pytest, et arrête le simulateur.
@author     Gemini CLI
@date       2026/05/11
"""

import subprocess
import time
import sys

def main():
    print("=============================================")
    print(" DÉMARRAGE DE LA SUITE DE TESTS AUTONOMES")
    print("=============================================\n")

    # 1. Démarrer le simulateur MS/TP en arrière-plan
    print("[*] Lancement du simulateur MS/TP...")
    simulator_proc = subprocess.Popen([sys.executable, "mstp_simulator.py"], 
                                      stdout=subprocess.PIPE, 
                                      stderr=subprocess.PIPE,
                                      text=True)
    
    # Laisser le simulateur s'initialiser
    time.sleep(2)

    # 2. Lancer la suite de tests via pytest
    print("\n[*] Lancement de pytest sur test_suite.py...")
    try:
        # On utilise subprocess.run pour afficher le flux en temps réel
        test_result = subprocess.run([sys.executable, "-m", "pytest", "-v", "-s", "test_suite.py"])
        if test_result.returncode == 0:
            print("\n✅ TOUS LES TESTS SONT PASSÉS AVEC SUCCÈS.")
        else:
            print("\n❌ DES ERREURS ONT ÉTÉ DÉTECTÉES PENDANT LES TESTS.")
    except Exception as e:
        print(f"\n❌ Erreur lors du lancement de pytest : {e}")

    # 3. Arrêter le simulateur
    print("\n[*] Arrêt du simulateur MS/TP...")
    simulator_proc.terminate()
    try:
        simulator_proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        simulator_proc.kill()
    
    print("\n=============================================")
    print(" FIN DE LA SUITE DE TESTS")
    print("=============================================")

if __name__ == "__main__":
    main()
