# RIC guidance protocol on a G01 (step 1: decode, read-only)

Follows on from [HUD-G01.md](HUD-G01.md). This collects what can be learned without sending anything to the cluster. The sources are the strings of this car's `NavNBTEvoController` (NBTevo_Y24073J) and NBT-AAD's J29 capture (`src/cluster/j29_graph.hpp`, `j29_graph_template.hpp`). Anything marked *inferred* has not been checked on a G01.

## Service and methods

The service is `BMW.INFOTAINMENT.REMOTEINSTRUMENTCLUSTER.RICServiceGuidance`, SOME/IP service 0xB006, instance 1 on the KOMBI (from `/etc/someip/services.conf`). In the nav program the client class is `CSomeIPClient_SomeIPRIC_IKE3D_RICServiceGuidance`, and it handles responses for these methods:

`create`, `dispose`, `initialize`, `checkResource`, `setResource`, `disposeResource`, `setGuidingData`, `setLaneGuidingData`, `setManeuverCtrlData`, `setManeuverData`, `setManeuverListData`, `setNaviInfoEventData`, `setRoadData`, `setRouteProfileData`

It also handles `trans*` variants of the data and resource calls (`transGuidingData`, `transLaneGuidingData`, `transManeuverData`, `transManeuverListData`, `transNaviInfoEventData`, `transResource`, `transRoadData`, `transRouteProfileData`) and one event, `sendEvent`. The `trans*` calls are probably the segmented form for large payloads (*inferred*).

The binary only has the names, not the numeric method IDs. The J29 capture has the IDs and the model types each one carried. Matching the two:

| Method ID | Model types seen (J29) | Body | Likely method |
|---|---|---|---|
| 0x0001 | not sent by NBT-AAD | | `create` / `dispose` (NBT-AAD: "never 0x0001 (create / dispose): the factory owns the instance") |
| 0x0002 | 3 | 9 bytes: handle, id, type | `checkResource` or `disposeResource` before a new text (*inferred*) |
| 0x0003 | 3 = TextResource | handle, id, type, len, tag, UTF-16BE text | `setResource` (*inferred*, the payload is a resource) |
| 0x0004 | none | 4 bytes: handle only | ends each block; possibly a commit (*inferred*, name unknown) |
| 0x0009 | 17 (destination info), 33 (guidance state) | | `setNaviInfoEventData` or `setManeuverCtrlData` (*inferred*) |
| 0x000A | 15 ManeuverDescription, 29 ManeuverDistance, 34 | | `setManeuverData` (*inferred*) |
| 0x000B | 7 guiding model | | `setGuidingData` (*inferred*) |
| 0x000C | 32 maneuver list | | `setManeuverListData` (*inferred*) |
| 0x000E | 40 | | `setRoadData` (*inferred*) |
| 0x0010 | 27 lane model | | `setLaneGuidingData` (NBT-AAD) |

The frame body layout NBT-AAD decoded is `[0:4]` instance handle (0xC8 on every J29 boot), `[4:8]` model id (`<type><index24>`), `[8]` model type, `[9:13]` payload length (body length minus 17), `[13:17]` tag, then the payload. Text is UTF-16BE behind a FEFF BOM, with its length in the byte before the BOM. A route start on the J29 was 97 frames. The block for each maneuver goes 0x0002/0x0003 (road name text), 0x000E, 0x000A (34, 29, 15), optionally 0x000B and 0x0010, then 0x000C, 0x0004 and 0x0009.

## What the cluster tells the head unit it can show

The head unit doesn't assume what the cluster can display. The cluster sends a configuration, and `RIC_CRicConfigInfoManager_IC` stores it. The nav program logs each part as it is read. These are the parts that matter for custom content:

- `setBitmapConfig ... isRLE24, isRLE32, isPNG, isJPG`: which bitmap formats the cluster accepts.
- `setCompositeBitmapConfig ... mMaxCompositeBitmapImages`: how many bitmaps can be stacked into one picture.
- `setPictureinPictureConfig ... supColorDepth, supColorPackaging` and `mMaxResolution[...]`: the "picture in picture" slot and its size limits.
- `setTextConfig ... maxTextLength` and `setCompositeTextConfig`: text resource limits.
- `setManeuverDistanceConfig ... maxDistanceStringLength`: the distance field (NBT-AAD's 6WA test showed 8 characters, J29 about 3 plus the unit).
- `setManeuverHighIntersectionElementConfig ... isHighGuidingPictureSupported`: whether the cluster takes a head-unit-drawn junction picture.

## Pictures on the cluster and HUD

The head unit can already send its own pictures to the cluster and HUD in the factory navigation:

- **Bitmap and SVG resources:** `setBitmapImage, resourceID, dimension, imageData` and `setSvgImage, resourceID, dimension, imageData`, sent with `RicSetResource`. Icons are cached as `RICIcon_%u_%d.rle`.
- **Picture in picture for the cluster and HUD:** `setPIPElementForICHUD, resourceID, colorDepth, packagingType, dimension`. There is a separate `setPIPElementForCID` for the centre display.
- **Junction view:** the nav switches a maneuver between "HG" (high guiding, drawn by the cluster from road geometry) and "JV" (junction view, a picture). `setHighCCPPicturePositionICHUD` places the car marker on that picture, and `displaying JV in HUD instead of FCHUD!` shows the HUD can take it.
- **Debug dumps:** `/var/log/bitmap.rle` and `/var/log/bitmap4most_%d.seg%d.rle`, plus a `dumpBitmap` flag on the MOST bitmap path. The nav program can write out what it sends. This could help in step 2.

So the one place where the cluster accepts a picture we draw ourselves is a bitmap resource (RLE, and PNG or JPG if the config says so) used as a junction view or PiP for "ICHUD". The cluster decides where and how that is shown. Text fields (distance, road name) are the simpler fallback.

## Step 2: the factory navigation on this car (read-only capture)

On 2026-11-15, with the car parked, `tcpdump -p -i vlan73` on the head unit was streamed to a PC over SSH while a nav route was started, cancelled, started again and cancelled. That covered 58 s and 3,165 packets. Nothing was sent by us. The capture itself is not committed, because it contains street names near the owner's home.

**Who is who** (from the SOME/IP-SD offers):

| Host | Offers | What it is |
|---|---|---|
| 160.48.199.96 | 0xB006 RICServiceGuidance (TCP/UDP 30502), 0xB009 DisplaySink (30503), 0xB00A ConsumptionHistory (30504), 0xB00E RICServiceList (30502), 0xB011 KombiInterface_2 (30501) | **The KOMBI** |
| 160.48.199.97 | 0xB001 Telephone, 0xB002 DataCommunication, 0xB003 SatellitePositioning, 0xB007 RemoteServices | The ATM telematics unit (`BMW.ATM` in services.conf) |
| 160.48.199.16, .117, .93, .33, .34, .6 | many other services | other ECUs, not relevant here |

**The guidance session.** The factory head unit uses SOME/IP client ID 0x0001 with interface version 1 on one TCP connection to 160.48.199.96:30502. Every body starts with instance handle 0x000000C8, the same handle as on the J29. Each call is a request (message type 0x00), and every one in the capture was answered with a response (0x80) and return code 0 (E_OK). The method IDs and model types are the **same as the J29's**:

| Method | Calls | Seen |
|---|---|---|
| 0x0002 | 14 | 9-byte body (handle, model id, type). The answer is one byte, 00 or 01, so this is **`checkResource`** ("does the cluster still hold resource X?"). |
| 0x0003 | 13 | **`setResource`**: type 3 = text (road names, UTF-16BE) and **type 1 = bitmap** (below). |
| 0x0004 | 15 | handle only. Closes every block, after 0x000C or a lone distance update (a commit; name not known). |
| 0x0009 | 31 | type 17 (destination info, with a distance text) and type 33 (guidance state). |
| 0x000A | 37 | type 15 ManeuverDescription, 29 ManeuverDistance, 34. |
| 0x000B | 9 | type 7 guiding model; type 10 once. |
| 0x000C | 11 | type 32 maneuver list. A 42-byte list is the empty list sent on cancel. |
| 0x000E | 11 | type 40. |
| 0x0010 | 6 | type 27 lane model. |

A route start is about 50 calls in about 0.7 s. A cancel is `0x0009` (state) three times, an empty `0x000C`, `0x0004`, then `0x0009` type 17. That is the same shape as NBT-AAD's J29 `cancel[]` template.

**A bitmap resource, decoded.** One `setResource` carried model id 0x01000000, type 1, 5,638 bytes. Its payload after the 17-byte frame header is:

```
u32 model id (01 00 00 00) | u8 type (01) | u16 width (0x0034 = 52) | u16 height (0x0021 = 33) | u32 data length (0x15E8 = 5608) | RLE data
```

The RLE is 32-bit ARGB, alpha first. Each control byte `c` either repeats the next pixel `c & 0x7F` times (top bit set) or is followed by `c` literal pixels (top bit clear). This decodes to exactly 52 x 33 = 1,716 pixels and uses all 5,608 bytes. The picture is a red route-number shield with "77" on it, the road-number icon the nav shows next to a street name. So the head unit already sends the KOMBI its own RLE32 pictures over this service, in a format we can now write.

**DisplaySink (0xB009).** Each time guidance starts or stops, the KOMBI sends event 0x8001 with `3ff0000000000000 01 0115 00a0 00`, a double 1.0 then possibly 277 x 160, and the head unit answers with method 0x0001 `0000`. What the numbers mean is not known. They may be the size of the cluster's picture area.

## Step 3: first live test (parked, owner's go-ahead)

On 2026-11-15, with the car parked and no route active, a script on the owner's Mac connected to the KOMBI through an SSH tunnel via the head unit (`ssh -N -L 30602:160.48.199.96:30502 root@<HU>`), so nothing was stored on the car. It sent this car's own captured route start (57 frames) under its own SOME/IP client ID 0x7E01. The texts were replaced: road names became "TEST" and the next maneuver's distance became "1234". After 10 s it sent the factory clear (6 frames).

Result:

- The KOMBI accepted all 63 frames (response 0x80, E_OK).
- **The HUD** showed the factory turn graphic (turn arrow and road sketch), with **"TEST" as the road name and "1234 m" as the distance**, next to the speed and the speed-limit sign, with no route running in iDrive. The owner photographed it; the photo is not committed.
- The 6WB cluster (in its COMFORT layout) showed nothing new. The cluster's own guidance view may need a different cluster display mode. Not checked.
- The clear worked.

So a second client can put content on the G01's HUD without a factory route, the same way NBT-AAD does on the J29. Text in the road-name and distance fields is shown as sent.

## Step 4: update rate (parked, owner's go-ahead)

On 2026-11-15, after the same test route start, a script sent 20 updates at 1 per second and then 20 at 5 per second. Each update was three frames: `0x0003` with new text for the road-name resource `03000000`, `0x000A` type 29 with a new distance for maneuver 0, and `0x0004`. Every frame got E_OK, and an update took about 7 ms round trip through the SSH tunnel.

- **The distance changed on the HUD at both rates.** 5 updates a second is fine for a number that changes, such as a gauge value.
- **The road name stayed "TEST".** Re-sending a text resource under an id the cluster already holds doesn't change what it shows. In the factory capture each new road name gets a **new** resource id (`03000004`, `03000005`), a new road model (type 40, `27000003`, ...) that points at it, then the maneuver, the list and `0x0004`. A follow-up test (`ric-rate2.py`, 2 s per step) tried two ways to change it, and **both worked**:

- **A (works, simplest):** `0x0003` with new text under the same id `03000000`, then re-send the road model `0x000E` `27000000`, the distance, maneuver 0 (`0x000A` type 15 `0f000000`), the maneuver list `0x000C` and `0x0004`. So the cluster keeps the new text but only redraws when the models that use it are sent again. Six frames take 11 to 14 ms.
- **B (works):** a new text id each time (`03000101`, ...), with `27000000` pointed at it, then the distance and `0x0004`.
- B5, the 9th new text id that session, was refused with message type 0x81 (error) and return code 0x26. **The cluster holds a limited number of text resources.** The factory nav reuses the same ids (`03000000` to `03000005`) on every route, and `checkResource` answers 01 for them on the second route. So a custom display should reuse a fixed set of ids, as in A.

## Step 5: a live value on the HUD (parked, owner's go-ahead)

On 2026-11-15 `ric-gauge.py` read the DME over the gateway's HSFZ port (6801, through the same SSH tunnel; UDS 0x22 reads of the BoostDeck DIDs 4201, 4205 and 4300 only) and put boost in psi in the distance field and "BOOST psi" plus coolant temperature in the road-name field, 5 times a second. The HUD showed it. Coolant read 95 °C and boost 0.0 psi (no boost with the engine at rest). The gateway closed the first few connections, then accepted one; BoostDeck may still have held it.

**Distance unit.** The byte right after the distance text is the unit. The factory sends 3 for turn distances (metres) and 0 for the destination distance. The HUD showed 0 = km, 1 = mi, 2 = mls, 3 = m, 4 = yd, 5 = ft, and ft for every value from 6 up (255 included). There is no value without a unit, so the distance field always adds one. A number with no unit has to go in the road-name text instead. `ric-gauge.py --top` does that ("BOOST 0.0 95C") and sends an empty distance text, which **hides the distance box completely**. The owner's photos (not committed) show both layouts. The turn graphic from the replayed route stays on screen in both.

## Step 6: what draws the map and arrow (parked, owner's go-ahead)

The maneuver model (`0x000A` type 15) payload is: id, type, `00 00`, previous maneuver id, `00`, next maneuver id, **turn kind** (1 byte), then the ids of its road (type 40), intersection (type 34), guiding (type 7) and distance (type 29) models, then 7 zero bytes and an optional list of lane models (a byte count, then type 27 ids). `ffffffff` means "none". `ric-nomap.py` changed maneuver 0 one step at a time; the owner photographed each step:

| Step | Change | HUD |
|---|---|---|
| 1 | none | the map sketch, as before |
| 2 | guiding ref set to none | **the sketch is gone**, a plain right-turn arrow instead |
| 3 | intersection ref also none | the same arrow |
| 4 | turn kind 0 | **no arrow: only the text line** |
| 5 | turn kind 6 | a left-turn arrow |
| 6 | as 3, list with only this maneuver | right-turn arrow (kind 7) |

So the guiding model (type 7) draws the map sketch, and the turn kind picks the arrow icon (7 = right, 6 = left, 0 = none). For a text-only HUD, send maneuver 0 with no guiding or intersection model and turn kind 0.

## Still open

- ~~Whether the KOMBI shows guidance without an active factory route~~: yes (step 3).
- ~~How fast the text fields can be updated~~: the distance updates at 5 Hz, and the road-name text changes when the road model, maneuver and list are re-sent after it (step 4).
- Where the KOMBI puts a bitmap resource that is not a road shield (for example as a junction picture), and whether the HUD shows it.
- Why the cluster did not show the guidance (display mode, or coding).
- How the factory session reacts to a second client using the same instance 0xC8 (NBT-AAD's J29 work says it copes, with its own client ID 0x7E01).
- The cluster's configuration message (formats, sizes). It was not in this capture, so it is probably sent once at start-up.


