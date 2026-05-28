---
name: bacnet-testing
description: Autonomous testing and debugging for BACnet MS/TP and BACnet/IP components. Use when asked to validate BACnet communication, debug MS/TP frame exchanges, or perform automated regressions on the BACnet gateway.
---

# BACnet Testing Skill

This skill provides tools and workflows for autonomous testing of BACnet components, specifically focusing on the MS/TP to BACnet/IP gateway.

## Workflows

### 1. Running Full Autonomous Tests
Use this workflow to validate the gateway's logic, including MS/TP simulation and UDP/TCP communication.

- **Action**: Run `scripts/run_all_tests.py`.
- **Description**: This script orchestrates the entire test suite:
    1. Starts `mstp_simulator.py` in the background.
    2. Runs `pytest` on `test_suite.py`.
    3. Cleans up background processes.

### 2. Manual Debugging and Sniffing
Use this when you need to inspect live BACnet traffic or send specific requests.

- **Action**: Use `scripts/bacnet_debug_tool.py`.
- **Command examples**:
    - `python3 scripts/bacnet_debug_tool.py <target_ip> --sniff 60`: Listen for I-Am responses.
    - `python3 scripts/bacnet_debug_tool.py <target_ip> --whois`: Send a Who-Is request.

### 3. MS/TP Simulation
Use this to mock a physical BACnet MS/TP device for testing the gateway's serial integration.

- **Action**: Run `scripts/mstp_simulator.py`.
- **Description**: Creates a virtual PTY and responds to tokens. Useful for testing the UART bridge.

## Bundled Resources

### Scripts
- `run_all_tests.py`: Main orchestrator.
- `test_suite.py`: Pytest suite (UDP Who-Is, TCP Bridge connection).
- `bacnet_debug_tool.py`: Advanced packet sniffer and generator.
- `mstp_simulator.py`: MS/TP device emulator.

### Assets
- `native_mock/`: Mock headers and stubs for native Linux compilation of BACnet C++ components.

## Troubleshooting
- **Port Conflict**: If port 47808 is already in use, `bacnet_debug_tool.py` will try to listen on a random port, which might prevent it from receiving broadcasts. Ensure no other BACnet stack is running locally.
- **PTY Permissions**: Ensure the user has permissions to create pseudo-terminals for `mstp_simulator.py`.
