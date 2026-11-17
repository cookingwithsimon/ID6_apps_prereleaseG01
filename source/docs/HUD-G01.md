# How the HUD gets its data on a G01 (findings)

Car: 2018 G01 X3 M40i, NBT Evo head unit on NBTevo_Y24073J, I-level S15A-24-7-530, 6WB digital cluster, factory HUD (option 610).

This is a read-only investigation. Nothing was written to the car, the cluster or the HUD. The driving lock on plugin screens is out of scope and is not discussed here.

## Short answer

The head unit never talks to the HUD. In every BMW generation covered here the HUD is a display driven by the instrument cluster (KOMBI). The head unit sends content to the cluster, and the cluster draws the HUD picture and sends it down its own video line.

What differs between cars is how the head unit reaches the cluster:

| Car | Head unit to cluster | What the head unit can put there |
|---|---|---|
| F-series with 6WA (the upstream 340i) | MOST ring, function block 0x82 instance 00, "HUDTerminal" | Free-form vector drawing: polygons, circles, text areas, visibility. HUD Gauges uses this. |
| J29 Supra (NBT Evo, G-series platform) | IP over the head unit's link to the cluster, SOME/IP service 0xB006 at 160.48.199.96:30502 | A structured route-guidance model only (arrow, distance, street, lanes). No free drawing. |
| G01 with 6WB (this car, read on the car) | IP: SOME/IP "Remote Instrument Cluster" (RIC) services hosted by the KOMBI, incl. RICServiceGuidance 0xB006, on the vehicle VLAN 160.48.199.x | Navigation guidance models for the cluster and the HUD, plus icon/bitmap/SVG "resources" the guidance refers to. No free drawing found. |

So the upstream HUD Gauges worker cannot work on this car as built. On this car the head unit's factory navigation reaches the HUD by sending RIC guidance data to the cluster over SOME/IP, and the cluster draws the HUD.

## 1. What the upstream HUD Gauges package does

`packages/hud-gauges.zip` holds `HudTest/` with a plugin (`libHudTest.so`), a worker (`hud-shape-qnx`) and a `gateway-ip.txt` of `169.254.21.3` (the reference 340i's gateway). Both ELFs are ARM, built with QNX GCC 4.4.2, not stripped.

- `libHudTest.so` is only the My Vehicle menu entry and checkbox. It spawns `/var/opt/hmi/data/ID5/Plugins/HudTest/hud-shape-qnx --draw-live` with `LD_LIBRARY_PATH=/opt/sys/lib:/opt/hmi/lib:/opt/car/lib:...` and keeps a `hud-enabled` flag file.
- `hud-shape-qnx` does two jobs:
  - **Data in:** RPM, boost and gear come from the gateway diagnostic link (HSFZ TCP 6801 to the address in `gateway-ip.txt`), like the other apps in this repo.
  - **Picture out:** it opens the head unit's MOST stack through DSI (`dlopen` of `/fs/sda0/opt/sys/lib/libsys_dsi_colibry.cpp-ne.so.8`, server `DSYSMOSTServer.MOSTSubSystem`), registers as a MOST client, reads the MOST registry and looks for the node hosting **FBlock 0x82 instance 00**. It then sends `TerminalInterface` methods to that node: `CreateInstance`, `SetVisibility`, `SwitchVisibility`, `TextareaSetText`, `ImageAddPolygon`, `ImageAddCircle`, `ImageSetLineColor`, `ImageSetFillColor`, `ImageRemoveAllItems` and so on.
- Its log strings show the guards: `no 82/00 host on a ring node in the registry`, `this cluster is not laid out as the profile says: nothing is sent until the daemon restarts`, and `profile mismatch: the factory navigation put text on instance %u`. It also yields when it sees the factory navigation using the terminal.
- The class names (`nbt::hudterm::HudTerminal`, `nbt::most::Client`, `nbt::DsiClient`) and the NBT-AAD notice in `NOTICE.txt` show it is built on NBT-AAD's `src/cluster/hud_terminal.*` and `hudterm.hpp` (github.com/Sq00ky/Supra-iDrive6-Android-Auto, milestone-7, PolyForm Noncommercial).

NBT-AAD's `hudterm.hpp` documents the protocol: every call is a MOST method sent as StartResultAck, the first parameter is a u16 instance handle, and the cluster answers with ResultAck echoing the payload. The target must be a ring node (0x0100 to 0x0FFF). The instance layout (1 = guidance panel, 2 to 4 = street/distance/unit text, 5/6 = double-buffered picture, 7/8 = route overlay, 9/10 = lanes) was read from an F20 6WA and is fixed per cluster in `hud6wa_profile.hpp`. HUD Gauges reuses those instances to draw a tach arc and a boost number in the nav-guidance panel, which the 6WA mirrors to the HUD.

In short, HUD Gauges is "draw into the cluster's nav panel over MOST". It needs (a) a MOST ring, (b) a cluster that hosts FBlock 82/00 and (c) the 6WA instance layout.

## 2. How the HUD is wired

- **F-series (BMW service information, F25 "Head-Up Display Unit"):** "The Head-Up Display is connected with the instrument cluster (KOMBI) by a six-pin plug connection." "An LVDS data line is used to transmit the image information from the instrument cluster to the Head-Up Display." The cluster takes the head unit's content from the bus and renders it.
- **G-series:** BMW's G30 bus notes say "The instrument cluster does not require Ethernet" and, on navigation cars, "The data transfer takes place from the Head Unit High to the instrument cluster via an APIX data cable." (bmw5g30.com, Ethernet page.) APIX is a serial video link with a bidirectional side channel. On this car the head unit sees the cluster as IP hosts on 160.48.199.x (section 4). Whether that IP traffic is carried over the APIX side channel or a plain Ethernet line was not checked.
- **PSdZData 4.61.20, S15A (G01/G02/G08), `odx-v/S15A.odx-v`:**
  - The KOMBI has a logical link `KOMBI_60_ETHERNET_63`, described as "Kombi (via Ethernet, IP gateway, 0x63)", where 0x63 is the head unit. So diagnostics reach the cluster through the head unit, which fits a cluster sitting behind the head unit's APIX/IP link rather than on the main vehicle Ethernet.
  - There is **no HUD logical link at all** in S15A (no `HUD_3D_*`; address 0x3D is used by CDM2). The generic sequence files list a `HUD` base variant at 0x3D, but the S15A vehicle file does not. The HUD is not a programmable ECU of its own on this series in this data, which is consistent with it being a display fed by the cluster.
  - S15A still defines `BUS_MOST`, but no S15A ECU has a `_MOST` logical link (the HU links are D-CAN, Ethernet and IK-CAN). So in PSdZData there is no MOST ring device to host FBlock 82 on a G01.

## 3. What NBT-AAD found on the J29

The J29 Supra runs the same NBT Evo software family on a G-series-style network. NBT-AAD's cluster/HUD arrows on the J29 do not use MOST. They send a route-guidance model to "the KOMBI's guidance service, SOME/IP `0xB006` at `160.48.199.96:30502`", with method whitelisting and a fixed instance handle `0xC8` (`src/cluster/j29_client.*`, `j29_graph.hpp`). The readme says "On any other car the KOMBI refuses the first frame or isn't there". What this service takes is a guidance graph (maneuver type, distance, road name, lanes), not shapes or free text in arbitrary positions. NBT-AAD drew its own pictures only on the F-series 6WA.

## 4. This car

Read on the car on 2026-11-15 with a read-only probe over SSH (`ifconfig`, `netstat -an`, `pidin arg`, file listings). Nothing was sent to any other ECU.

**Network.** Besides the ENET port (`dwc0`, 169.254.199.119), the head unit has these interfaces:

| Interface | Address | Notes |
|---|---|---|
| `vlan73` on `dwc0` | 160.48.199.119/25 | The vehicle IP network. `autoarpd` runs on it with the SOME/IP-SD multicast group 239.192.255.251:30490. |
| `vlan77`, `vlan80`, `vlan86` on `dwc0` | 160.48.199.225/28, .185/29, IPv6 only | Other vehicle VLANs. |
| `veom0` | 160.48.199.254/30 | A point-to-point link to 160.48.199.253, with about 15 TCP sessions. This looks like the head unit's internal link to its second processor. |
| `uap0`, `wfd0` | 172.16.224.1, 172.16.222.1 | Wi-Fi hotspot and Wi-Fi Direct. |

**Who the head unit talks to.** `netstat` shows live TCP sessions from 160.48.199.119 to:

- **160.48.199.96** on ports 30501, 30502 (two sessions), 30503 and 30504
- **160.48.199.97** on ports 30501, 30502 and 30503 (two sessions)

There is also UDP traffic to .96 (30501 to 30504), .97 (30503, 30505), .93, .16 and .117 (30501). Ports 30490 to 30510 are the SOME/IP range, and `someip_sddaemon -c /etc/someip/someip.conf` and `someip_enhtestability_server` are running. NBT-AAD's J29 cluster guidance service is at exactly **160.48.199.96:30502**, so on this G01 the head unit is already holding a session to the same address and port. The most likely reading is that 160.48.199.96 is the 6WB cluster and that the factory navigation sends its guidance there over SOME/IP, as on the J29. What 160.48.199.97 is has not been identified yet.

**MOST.** `/opt/sys/bin/most` runs with `--devmode=client --serverRoleName=MOSTSubSystemServer --SIPCProtocol=tcp`, and no `/dev/most*` device exists. The MOST stack here is a client of a server reached over TCP, and PSdZData shows no MOST devices on S15A. So there is no ring node hosting FBlock 82/00 for HUD Gauges to find.

**Video.** `dev-ethervideo` is an H.264 decoder fed from `/dev/icam2` (max 1440x544), so it brings the camera picture into the head unit. It is not a path to the cluster or HUD.

**Software.** The head unit has `libsys_someip`, `libcommonapi21_someip` and `libsys_dsi_commonapi21` (the SOME/IP stack the apps use through DSI), and `libimp_sys_most_base_nbt` (the MOST library NBT-AAD read the HUDTerminal encoding from). So both stacks are installed; which one drives the cluster is decided at run time (next sections).

### SOME/IP service list (`/etc/someip/services.conf`)

The head unit maps its CommonAPI services to SOME/IP ids in `/etc/someip/services.conf`. The ones the KOMBI hosts:

| Service | SOME/IP id | Instance |
|---|---|---|
| `BMW.INFOTAINMENT.KombiInterface` | 45061 (0xB005) | BMW.Kombi |
| `BMW.INFOTAINMENT.REMOTEINSTRUMENTCLUSTER.RICServiceGuidance` | **45062 (0xB006)** | BMW.Kombi |
| `BMW.INFOTAINMENT.REMOTEINSTRUMENTCLUSTER.DisplaySink` | 45065 (0xB009) | BMW.Kombi |
| `BMW.INFOTAINMENT.ConsumptionHistory` | 45066 (0xB00A) | BMW.Kombi |
| `BMW.INFOTAINMENT.REMOTEINSTRUMENTCLUSTER.RICServiceList` | 45070 (0xB00E) | BMW.Kombi |
| `BMW.INFOTAINMENT.KombiInterface_2` | 45073 (0xB011) | BMW.Kombi2 |

Endpoints are not fixed in `someip.conf`. They are found with SOME/IP service discovery (`someip_sddaemon`, multicast 239.192.255.251:30490), so the KOMBI's address comes from the network. Given the live sessions above and NBT-AAD's J29 result, 160.48.199.96 is most likely the KOMBI. `/opt/conn/etc/hosts` names other hosts (hu-jacinto .98, hu-intel .99, rse .38, TCB .225 to .227) but not .96 or .97.

### The factory navigation (`NavNBTEvoController`, Y24073J)

The nav controller binary (16.8 MB, stripped) has both cluster paths built in, and picks one from coding and the cluster type:

- **MOST HUDTerminal path (F-series):** `handling HUDTerminal.TerminalInterfaceCall`, `HUDTerminal.ImageAddBitmapCall`, `HUDTerminal.PotentiometerSetValueCall`, two communicators `mIcHudComm4Kombi` and `mIcHudComm4HUD`, and `initIcHUDcommunicator_IC ... unsupported instrument cluster ID: %d`. This is what the 6WA uses and what HUD Gauges copies.
- **RIC SOME/IP path (G-series):** proxies `BMW_INFOTAINMENT_REMOTEINSTRUMENTCLUSTER_RICServiceGuidanceProxy` and `..._DisplaySinkProxy`, and the calls `RicCreate`, `RicInitialize`, `RicSetManeuverData`, `RicSetManeuverListData`, `RicSetLaneGuidingData`, `RicSetGuidingData`, `RicSetRoadData`, `RicSetRouteProfileData`, `RicSetNaviInfoEventData`, `RicSetResource`, `RicCheckResource`, `RicDisposeResource`, `RicDispose`. Its jobs are named per display: `CRicPiPDisplayableICHUDJob`, `CRicPiPComICHUDDataJob` (cluster and HUD) and `CRicPiPDisplayableCIDJob` (central display). It is gated by coding: `Coding does not match, RICCtrl will not be activated` / `RICCtrl activation`.
- **HUD selection by coding:** `FPK claims DSELECTION_FPK_HUD, but HUD not coded! correction, using: DSELECTION_FPK!` and `FPK_PLUS_HUD not coded (disp.=%d) -> ignoring HUD`. FPK is BMW's name for the free-programmable (digital) cluster. So the head unit only targets the HUD when the cluster is coded as "FPK + HUD", and the cluster then puts the guidance on the HUD.
- **Resources:** the RIC path uploads pictures the guidance refers to: `BitmapImageChunk`, `SvgImageChunk`, `35Up_RCText chunk`, icons cached as `RICIcon_%u_%d.rle`. "35Up" is BMW's name for this G-series architecture.

So on this car the chain is: factory navigation, then RIC guidance models and resources over SOME/IP (0xB006 and friends) to the 6WB KOMBI, then the KOMBI renders the cluster panel and the HUD picture and drives the HUD.

## 5. What this means for a G01 HUD display

- HUD Gauges as published will not draw anything on this car. With no 82/00 host in the registry it stays offline and sends nothing, which is the safe outcome the fork README already describes.
- The route on a G01 is RIC over SOME/IP, the same family NBT-AAD drives on the J29 (service 0xB006). It carries navigation guidance: maneuver, distance, lanes, road and route data, info events, and image resources (bitmap, SVG, text chunks) that the guidance points at. It is not a drawing canvas like the 6WA's HUDTerminal.
- A HUD gauge on this car would therefore have to be expressed as guidance content: for example a short text in a guidance field, or a bitmap resource shown where a junction picture or icon would go. Whether the KOMBI shows an uploaded bitmap on the HUD outside an active route, how often it can be refreshed, and what it does to the factory navigation are all unknown. NBT-AAD only ever sent guidance models on the J29 and never used resources.
- The RIC client in the head unit is part of `NavNBTEvoController`. A plugin would need its own SOME/IP client (as NBT-AAD's `j29_client` does) and must not disturb the factory navigation's session.
- Next read-only steps while parked: (1) confirm which of 160.48.199.96 and .97 offers 0xB006 by reading the SOME/IP-SD offers on vlan73 (passive capture only); (2) dump the RIC method and field layout from `NavNBTEvoController` (proxy class `RICServiceGuidanceProxy`) to see the resource and text formats; (3) compare with NBT-AAD's captured J29 frames in `j29_graph_template.hpp`. Sending anything to the KOMBI, even a guidance frame, is a separate decision for the owner.

## Sources

- `packages/hud-gauges.zip` in this repo (strings and symbols of `hud-shape-qnx` and `libHudTest.so`).
- NBT-AAD, milestone-7: `src/cluster/hudterm.hpp`, `hud_terminal.hpp`, `hud6wa_profile.hpp`, `j29_client.hpp`, `readme.md` ("Turn arrows in the cluster and HUD (J29)").
- Read-only probes on the car, 2026-11-15: `ifconfig`, `netstat -an`, `pidin arg`, `/etc/someip/services.conf`, `/etc/someip/someip.conf`, `/opt/sys/etc/ethervideo.cfg`, `/opt/conn/etc/hosts`, and strings of `/fs/sda0/opt/nav/eceusa/bin/NavNBTEvoController`.
- PSdZData 4.61.20, `mainseries/S15A/S15A_26_11_520_V_004_000_001/odx/src/odx-v/S15A.odx-v` and `mapping/cseq.xml`.
- [F25 Head-Up Display Unit, description and operation](https://manuals.autorepairshq.com/BMW/2011/X3%20xDrive%2028i%20%28F25%29%20L6-3.0L%20%28N52T%29/Repair%20and%20Diagnosis/Instrument%20Panel%2C%20Gauges%20and%20Warning%20Indicators/Heads%20Up%20Display%20Unit/Description%20and%20Operation)
- [BMW 5 Series G30, Ethernet bus systems](https://www.bmw5g30.com/ethernet-1867.html)
