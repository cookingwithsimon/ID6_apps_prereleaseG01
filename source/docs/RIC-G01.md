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

## What step 1 cannot answer

- The numeric method IDs on the G01, and whether they match the J29's. The service ID and method names match, so it is likely.
- The G01 cluster's actual configuration (formats, sizes, text lengths). It is sent at run time.
- Which of 160.48.199.96 and .97 is the KOMBI's RICServiceGuidance endpoint.
- Whether the cluster shows anything without an active route, and what the factory nav does if a second client writes to the instance.

Step 2 (watching the factory navigation start a route while parked, read-only) answers the first three. Only step 3 (a live test, needs the owner's go-ahead) answers the last.
