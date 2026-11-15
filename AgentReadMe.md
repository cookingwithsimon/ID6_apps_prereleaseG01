# AgentReadMe

## Select the apps

Ask the owner for their choice of **MHD+ Suite** or **Bootmod3 Suite**, and whether they want **xDrive Control** and **HUD Gauges**. Install only the selected tuning menu. Install xDrive Control only when selected. BoostDeck, ShiftDeck, and Engine Fault Codes are independent apps.

## Identify the car

1. Confirm the installation is authorized and the car is parked, with iDrive awake. Connect wired ENET and identify the computer's ENET address, diagnostic gateway address, and head-unit SSH address.
2. Use an unlock tool that supports the exact unit, such as [Bavarian SSH Client](https://ssh.bavarian-aa.uk/). Verify the SSH host key and use locally supplied credentials.
3. Read the exact NBTevo software code, the active panel resolution (LayerManager screen 0), and SHA-256 hashes of both files under /opt/hmi/ID5/lib. Use the [read-only capture utility](source/tools/capture_headunit.py) when you need a structured report.

The packaged build is for **NBTevo_N18032I**, **1280 × 480**, with this framework pair.

| File | SHA-256 |
| --- | --- |
| libAppFramework.A.so | 93f4c7ab7e59c0adf7923b9b491de06165ba9d97bbe7412292e7ddc418ca2123 |
| libCoreFramework.A.so | 64ba154c68c772f9ca779fc18507f5b34715be58ded4a198b8a62b90d16c3ee0 |

This fork's source targets a 1440×540 panel. Plugins load the gateway from `gateway-ip.txt` beside the worker and send nothing when that file is missing or invalid. The packages under `packages/` remain the NBTevo_N18032I, 1280×480 builds. Do not install those packages on this G01.

Native builds use the bmw-tools LLVM SDK for QNX 6.5 ARMv7 (clang and lld) through `source/native/CMakeLists.txt`. Link an install against the head unit's own framework libraries. See [Build and deploy](source/docs/BUILD.md).

## Stage and verify

Download and extract the selected packages from [packages/](packages/). The dashboard ZIP contains BoostDeck, ShiftDeck, EngineCodes, and MhdMaps. Include MhdMaps only for the MHD choice. Bootmod3Suite and XDriveControl are separate ZIPs. Read their package notes for module and tune constraints. **The engine must be off before switching between AWD and RWD in either direction.** Install neither when the owner declines them.

Inventory /var/opt/hmi/data/ID5/Plugins for conflicting names and connector IDs. Back up any prior version being replaced to a separate directory outside Plugins. Stage complete app folders outside Plugins, upload over confirmed SSH, set the workers executable, and compare each readback with its local file before moving the folders into Plugins. Verify the final paths and bytes, and tell the owner where backups were saved. If anything fails, restore the previous folders before restarting. Do not add a second copy of an app.

After verification, have the owner hold the volume knob until iDrive goes dark, then check the selected menus while parked. Check live readings and settings. Menu installation alone does not verify an engine or drivetrain command. Get the owner's authorization before testing a command that changes ECU or drivetrain state.

## Optional HUD Gauges

Install the [HUD Gauges package](packages/hud-gauges.zip) only when the owner selects it and the car has the matching reference configuration. This G01 fork does not. The published worker expects a factory MOST HUDTerminal service 82/00 at node 0101, and it sends nothing when that host or the reference cluster layout is absent. On a G01, confirm a 82/00 host in the MOST registry while parked before treating HUD Gauges as installable. The package's `gateway-ip.txt` is the reference car's address.

Extract and verify SHA256SUMS. Stage only the HudTest directory and check for an existing HudTest or an app using connector ID 107. Preserve the prior app outside Plugins if replacing it. Keep the included gateway-ip.txt beside the worker. Set hud-shape-qnx executable, verify the uploaded files, and restart iDrive while parked. Open My Vehicle → HUD Gauges and check Show HUD gauges. Verify the tach and PSI display, that the checkbox persists across a restart, and that unchecking clears the overlay. If the display fails its parked checks, turn it off and restore the prior app folder or remove HudTest from Plugins, then restart iDrive.
