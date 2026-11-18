# AgentReadMe

## Select the apps

Ask the owner for their choice of **MHD+ Suite** or **Bootmod3 Suite**, and whether they want **xDrive Control (experimental)** and **HUD Gauges**. Install only the selected tuning menu. Install xDrive Control (experimental) only when selected. BoostDeck, ShiftDeck, and Engine Fault Codes are independent apps.

## Identify the car

1. Confirm the installation is authorized and the car is parked, with iDrive awake. Connect wired ENET and identify the computer's ENET address, diagnostic gateway address, and head-unit SSH address.
2. Use an unlock tool that supports the exact unit, such as [Bavarian SSH Client](https://ssh.bavarian-aa.uk/). Verify the SSH host key and use locally supplied credentials.
3. Read the exact NBTevo software code, the active panel resolution (LayerManager screen 0), and SHA-256 hashes of both files under /opt/hmi/ID5/lib. Use the [read-only capture utility](source/tools/capture_headunit.py) when you need a structured report.

On some units SSH lands on the Jacinto processor (`uname -a` reports `hu-jacinto`), not the HMI OMAP, and `/opt/hmi/ID5/lib` is empty on that node. The HMI files are then reachable read-only over Qnet under `/net/hu-omap/fs/sda0/...`, and OMAP commands run with `on -f hu-omap`. The capture utility detects this and reads the right paths automatically. See [Split-node units](source/docs/PORTING.md#split-node-units).

The reference dashboard build uses **NBTevo_N18032I** at **1280 × 480**. The desktop installers check the exact firmware and framework pair before staging. See [Dashboard compatibility](source/docs/COMPATIBILITY.md) for the captured profiles and their validation.

| File | SHA-256 |
| --- | --- |
| libAppFramework.A.so | 93f4c7ab7e59c0adf7923b9b491de06165ba9d97bbe7412292e7ddc418ca2123 |
| libCoreFramework.A.so | 64ba154c68c772f9ca779fc18507f5b34715be58ded4a198b8a62b90d16c3ee0 |

This fork's source targets a 1440×540 panel. Plugins load the gateway from `gateway-ip.txt` beside the worker and send nothing when that file is missing or invalid. The packages under `packages/` remain the NBTevo_N18032I, 1280×480 builds. Do not install those packages on this G01.

Native builds use the bmw-tools LLVM SDK for QNX 6.5 ARMv7 (clang and lld) through `source/native/CMakeLists.txt`. Link an install against the head unit's own framework libraries. See [Build and deploy](source/docs/BUILD.md).

## Stage and verify

Download and extract the selected packages from [packages/](packages/). The dashboard ZIP contains BoostDeck, ShiftDeck, EngineCodes, and MhdMaps. Exhaust Flap is under MHD+ Suite → Options and uses the same gateway-ip.txt setting. When updating MhdMaps, move its earlier standalone ExhaustFlap plugin folder to the backup outside Plugins. Include MhdMaps only for the MHD choice. Bootmod3Suite and XDriveControl are separate ZIPs. Read their package notes for module and tune constraints. **The engine must be off before switching between AWD and RWD in either direction.** Install neither when the owner declines them.

Inventory /var/opt/hmi/data/ID5/Plugins for conflicting names and connector IDs. Back up any prior version being replaced to a separate directory outside Plugins. Stage complete app folders outside Plugins, upload over confirmed SSH, set the workers executable, and compare each readback with its local file before moving the folders into Plugins. Verify the final paths and bytes, and tell the owner where backups were saved. If anything fails, restore the previous folders before restarting. Do not add a second copy of an app.

After verification, have the owner hold the volume knob until iDrive goes dark, then check the selected menus while parked. Check live readings and settings. Menu installation alone does not verify an engine or drivetrain command. Get the owner's authorization before testing a command that changes ECU or drivetrain state.

## Optional HUD Gauges

Install the [HUD Gauges package](packages/hud-gauges.zip) only when the owner selects it and the car has the matching reference configuration. This G01 fork does not. The published worker expects a factory MOST HUDTerminal service 82/00 at node 0101, and it sends nothing when that host or the reference cluster layout is absent. On a G01, confirm a 82/00 host in the MOST registry while parked before treating HUD Gauges as installable. The package's `gateway-ip.txt` is the reference car's address.

Extract and verify SHA256SUMS. Stage only the HudTest directory and check for an existing HudTest or an app using connector ID 107. Preserve the prior app outside Plugins if replacing it. Keep the included gateway-ip.txt beside the worker. Set hud-shape-qnx executable, verify the uploaded files, and restart iDrive while parked. Open My Vehicle → HUD Gauges and check Show HUD gauges. Verify the tach and PSI display, that the checkbox persists across a restart, and that unchecking clears the overlay. If the display fails its parked checks, turn it off and restore the prior app folder or remove HudTest from Plugins, then restart iDrive.

## Uninstall

Each app is one folder under /var/opt/hmi/data/ID5/Plugins. Keep each folder intact so its local settings, including gateway-ip.txt and hud-enabled where present, can be restored.

| App | Folder |
| --- | --- |
| BoostDeck | BoostDeck |
| ShiftDeck | ShiftDeck |
| Engine Fault Codes | EngineCodes |
| MHD+ Suite (with Exhaust Flap and Max Cooling) | MhdMaps |
| Exhaust Flap from earlier standalone builds | ExhaustFlap |
| Bootmod3 Suite | Bootmod3Suite |
| xDrive Control (experimental) | XDriveControl |
| HUD Gauges | HudTest |

1. Ask the owner which apps to remove. Leave every other folder in Plugins untouched.
2. While the menus still exist, have the owner put the car back in the state they want to keep. Uncheck **My Vehicle → HUD Gauges → Show HUD gauges** and confirm the overlay clears. With the engine off, switch xDrive Control (experimental) back to AWD. Select the wanted map slot, exhaust flap, Max Cooling, Active Sound, and burble settings. Removing a menu does not undo a setting already sent to the DME or DSC.
3. Park the car, keep iDrive awake, and connect as in [Identify the car](#identify-the-car). On a split-node unit, use the OMAP address or the Qnet write path described in [Split-node units](source/docs/PORTING.md#split-node-units).
4. List Plugins and confirm that each folder to remove is one of the apps above. Move only those folders into a new backup directory outside Plugins, such as /var/opt/hmi/data/ID5/PluginBackups/<timestamp>. Keep the original folder names, avoid overwriting an existing backup, and record the backup path.
5. List Plugins again and verify that the removed folders are gone and the remaining ones are unchanged.
6. Have the owner hold the volume knob until iDrive goes dark. After the restart, check that the removed entries are gone from My Vehicle, the remaining apps still open, and the HUD shows no overlay.

To restore an app, make sure its folder name is available in Plugins, then move the intact folder back from the backup. Restart iDrive while parked and confirm that the restored menu opens.
