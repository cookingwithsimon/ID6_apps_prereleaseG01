# HUD boost for the G01 (first version)

`hud-boost-qnx` puts live boost on a G01's head-up display: one text line, **BOOST 12.3 psi**, and
a 52×33 bar picture to the left of it that fills orange from 0 to 20 psi. It runs on the head unit and
uses the cluster's HUD guidance link (SOME/IP service 0xB006 at 160.48.199.96:30502), with its own
client id 0x7E01. [RIC-G01.md](RIC-G01.md) has how that link was decoded and tested.

Source: `native/src/hud_boost.c` and `native/src/hud_boost_frames.h`. Host test:
`native/tests/test_hud_boost.py` (`make -C source/native test`).

## What it sends

Only frame shapes that were tested on the car in RIC-G01.md steps 4 to 12:

| When | Frames |
| --- | --- |
| Start | The factory clear, then our own start (step 12): idle state and destination info (`--start 2`, the default, or left out with `--start 1`), text 03000000, a plain road model, an empty distance (hides the distance box), one turn with no map sketch, no arrow and no next turn, a one-entry list, commit, guidance state. |
| Each change, at most 5 times a second, and every 5 s | checkResource and setResource of the 52×33 picture 01000000 (RLE32, A,B,G,R pixels), the text 03000000, the road model pointing at both, the same turn and list, commit, guidance state (recipe A from step 4, picture from step 7). |
| Exit (Ctrl+C, SIGTERM, SIGHUP, time limit) | The factory clear. |

It never sends a route, a place name, a road-number picture or a distance. It reuses the same two
resource ids, so it never reaches the cluster's resource limit (step 4, refusal 0x81/0x26). If the
cluster refuses any frame it sends the clear and stops.

## Where boost comes from

UDS 0x22 reads of DME (0x12) DIDs 4201 (ambient, kPa × 256, once a second) and 4205 (boost pressure
absolute, kPa × 128, every tick) over the diagnostic gateway's HSFZ port 6801, with tester address
0xF4. Boost in psi = (4205/128 − 4201/256) × 0.145037738, worked out with integers only. The
gateway address comes from `--gateway`, a `gateway-ip.txt` next to the program, or the first
`gateway-ip.txt` in an app folder under `/var/opt/hmi/data/ID5/Plugins`.

It takes the loopback lease in `diagnostic_lease.h` before using the gateway, keeps the connection
while no other app is waiting, and hands it over (closes it) when another app asks. While another app
has the gateway it keeps showing the last reading for up to a second.

## Drive mode

The body controller (BDC, 0x40) reports the drive mode in DID D611. On Simon's G01 (2026-11-18) it
read 1 SPORT, 2 SPORT+, 3 SPORT Individual, 4 COMFORT, 7 ECO PRO, 9 ECO PRO Individual and
10 ADAPTIVE as he switched modes. These match strulock/m550-analyzer's G30 notes, which also list
5 and 6 (COMFORT+ / Individual) and 8 (ECO PRO+). 0 shows while starting up and 63 means no value.

- `--watch-mode` sends nothing to the cluster and prints each change.
- `--auto` shows boost only in SPORT, SPORT+ and SPORT Individual. In any other mode it sends the
  factory clear once and then leaves the HUD alone. It ignores 0 and 63 and keeps the last good mode.

## Factory nav

The factory nav logs its guidance to the system log. On Simon's car (2026-11-18), starting a route
logged `MARKER RouteGuidanceStarted`, `GuidanceNewRoute ...`, `attach TDA on guidance
viewer:GUIDING LABELS` and `MapViewer: API call --> enable route: routeId N`. Cancelling it logged
`attach EMPTY on guidance viewer:GUIDING LABELS for man id:0` and `disable route: routeId N`.
Reroutes enable a new route id and disable the old one.

Every HUD run starts `sloginfo -w` (read-only) and reads its output. It uses a free
pseudo-terminal when there is one, so lines arrive at once rather than in 4 KB lumps through a pipe.
`sloginfo -w` first replays the log kept since start-up, so a route that is already guiding is
seen too. The nav counts as guiding while guidance labels are attached or the map has a route id
enabled. While it guides, the worker sends the factory clear, closes its cluster connection and
sends nothing until 3 s after guidance ends. Until the replay has been read (1 s), or if `sloginfo`
can't be started, it shows nothing.

- `--watch-nav` prints `Factory nav: guiding` / `not guiding` and the log lines behind it, with no HUD.
- `--no-nav-check` turns the watch off. `--sloginfo PATH` picks another program (the host test uses
  a stand-in).

## Build

With the bmw-tools SDK, as for the other workers: the `hud-boost-qnx` target in
`native/CMakeLists.txt`. It needs nothing beyond `libc.so.3` and `libsocket.so.3` (it also uses
`posix_spawn` from libc), uses no floating point and calls no integer-division helper.

## Status

- Host tests pass against a stand-in cluster and gateway, and the frames match the Python test
  scripts byte for byte.
- `--watch-mode` and `--auto` ran on the car on 2026-11-18: the drive-mode values above, and boost
  on the HUD in the sport modes. It is started by hand over SSH from `/dev/shmem` (RAM, gone after a
  restart).
- The nav stand-aside is tested here against Simon's logged lines but not yet on the car. Also not
  yet seen: whether arriving at a destination logs the same `attach EMPTY` line as cancelling.
- Starting it automatically with the head unit comes next.
