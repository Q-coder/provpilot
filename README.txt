ProvPilot — Windows
===================
Visual Flight Simulator  |  Cessna 172  |  Scenery: Schaffhausen, Switzerland
https://www.md80.ch/provpilot/


CONTENTS
--------
  ProvPilot.exe   — main application
  ProvPilot.pck   — game data (must stay next to the .exe)
  README.txt      — this file
  bin\            — native GDExtension DLLs (must stay next to the .exe)

On first launch ProvPilot creates two more items in the same folder:
  aircraft\       — flight-dynamics & 3D model data per aircraft
                    (X-Plane style — editable on disk)
  provpilot.cfg   — your settings (scenery library, joystick, graphics, …)

The whole folder is portable: move/copy it to another machine and your
settings and aircraft tweaks come along.


INSTALLATION
------------
1. Extract both ProvPilot.exe and ProvPilot.pck into the same folder.
   Example: C:\Users\YourName\ProvPilot\

2. Download the Schaffhausen demo scenery from https://www.md80.ch/provpilot/
   and extract it so you have a folder structure like:
     C:\Users\YourName\provpilot\scenery\Switzerland\terrain\
     C:\Users\YourName\provpilot\scenery\Switzerland\orthophoto\
     C:\Users\YourName\provpilot\scenery\Switzerland\buildings\
     ...

3. Run ProvPilot.exe.
   On first launch, Windows may show a SmartScreen warning:
     "Windows protected your PC"
   This is expected for unsigned software. Click "More info" then
   "Run anyway". You will not see this warning again for this file.

4. On first start, ProvPilot will ask you to locate your scenery.
   Click "Browse" and select the folder that CONTAINS the Switzerland/
   subfolder (e.g. C:\Users\YourName\provpilot\scenery\).
   The setting is saved; subsequent launches skip this step.

5. Select LSPF Schaffhausen from the airport list, then click "Fly!".
   The loading screen will appear while terrain tiles stream in (up to 45s).
   Once it dismisses itself, the simulation is live.


SYSTEM REQUIREMENTS
-------------------
  OS      : Windows 10 or 11, 64-bit
  CPU     : Any modern x86-64 (Intel/AMD)
  GPU     : OpenGL 3.3 compatible or better (integrated graphics work)
  RAM     : 4 GB minimum, 8 GB recommended
  Storage : ~1 GB for application + ~500 MB per scenery canton
  Joystick: Optional but recommended (any USB/HID joystick or yoke)

No additional runtime libraries or installers are required.


KEY BINDINGS — FLIGHT CONTROLS
-------------------------------
  Joystick axis   Aileron / Elevator (configured in Settings → Joystick)
  Joystick axis   Rudder pedals (optional, configurable)

  Arrow keys      Aileron (←/→) and Elevator (↑/↓) — keyboard fallback
  Z / X           Left / Right rudder — keyboard fallback

  T               Throttle +5%
  Shift+T         Throttle -5%

  F               Flaps down (one notch: UP → 10° → 20° → 40°)
  Shift+F         Flaps up (one notch)

  M               Mixture richer (+5%)
  Shift+M         Mixture leaner (-5%)

  I               Magneto clockwise (OFF→R→L→BOTH→START)
  Shift+I         Magneto counterclockwise


KEY BINDINGS — VIEWS
---------------------
  C               Cycle camera: Cockpit → External (orbit) → Chase → Cockpit
  Mouse drag      In Cockpit view: look around the cockpit
                  In External view: orbit around aircraft
  Mouse wheel     In External view: zoom in/out


KEY BINDINGS — NAVIGATION / UTILITIES
--------------------------------------
  Shift+Y         Toggle Slew Mode (teleport mode — physics paused)
  Shift+P         Open Position dialog (enter Swiss LV95 coordinates directly)
  Escape          Return to Welcome screen (select new airport / aircraft)
  Ctrl+Shift+F    Toggle FPS counter


SLEW MODE (Shift+Y)
-------------------
Slew mode freezes the flight model and lets you reposition the aircraft
freely. Useful for exploring the scenery without flying.

  W / S           Move forward / backward
  A / D           Move left / right
  Q / PageUp      Move up
  E / PageDown    Move down
  Hold Shift      Move 5× faster
  Joystick        Left stick = horizontal, right stick = up/down and yaw

Press Shift+Y again to resume normal flight. JSBSim reinitializes at the
new position with the airspeed you had before entering slew mode.


JOYSTICK CONFIGURATION
-----------------------
Go to Settings → Joystick to assign axes and buttons. ProvPilot supports
any HID joystick, yoke, or rudder pedals. Axis assignments are saved
per-device so you can switch between devices without reconfiguring.

If no joystick is connected, keyboard fallback (arrow keys + Z/X) is
active automatically.


SCENERY
-------
ProvPilot uses tile-based scenery in Swiss LV95 coordinates. Each canton
is a separate download. The demo covers Schaffhausen (canton SH), which
includes terrain elevation, aerial orthophotos, 3D buildings, roads,
vegetation and water.

  terrain/        .raw elevation tiles (1024×1024, 1 metre/pixel)
  orthophoto/     .webp aerial image tiles
  orthophoto_hires/ high-resolution aerial images (urban areas)
  buildings/      .glb 3D building meshes
  roads/          .glb road network meshes
  water/          .glb lake / river meshes
  vegetation/     .glb tree clusters

Additional scenery cantons (when available) are placed as siblings of
Switzerland/ and are automatically detected on the next scan
(Settings → Scenery → Scan).

Coverage of the Schaffhausen demo (~47.6°–47.9°N, 8.3°–8.8°E):
  LSPF  Schaffhausen-Schmerlat   (grass runway 07/25, 600 m)


SETTINGS FILE
-------------
Settings are stored in `provpilot.cfg` next to ProvPilot.exe (portable
install — the file moves with the folder). It also tracks which version
of the bundled aircraft data has been extracted into `aircraft\`.

Delete `provpilot.cfg` to reset all settings to defaults (scenery paths,
joystick assignments, tree density, etc.). Delete the `aircraft\` folder
(or just `aircraft\c172p\`) to force ProvPilot to re-extract the bundled
flight-dynamics and 3D model files on the next launch.


KNOWN LIMITATIONS
-----------------
  - Only the Cessna 172P is available in this demo.
  - Scenery is limited to Schaffhausen canton in the demo package.
  - No ATC, multiplayer, or weather simulation.
  - Night lighting is not yet implemented.


LICENSE / CREDITS
-----------------
  Flight dynamics : JSBSim (LGPL) — https://jsbsim.sourceforge.net/
  Engine          : Godot 4 (MIT) — https://godotengine.org/
  Scenery data    : swisstopo (terrain, orthophoto) — https://www.swisstopo.admin.ch/
                    OpenStreetMap contributors (roads, buildings)
  ProvPilot       : © 2024-2026 md80.ch — https://www.md80.ch/provpilot/
