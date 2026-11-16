ProvPilot — Windows
===================
Visual flight simulator for Europe  |  https://www.md80.ch/provpilot/


CONTENTS
--------
  ProvPilot.exe   — main application
  ProvPilot.pck   — game data (must stay next to the .exe)
  README.txt      — this file
  bin\            — native GDExtension DLLs (must stay next to the .exe)

On first launch ProvPilot creates two more items in the same folder:
  aircraft\       — flight-dynamics, 3D model, sound and livery data per
                    aircraft (X-Plane style — editable on disk)
  provpilot.cfg   — your settings (scenery library, joystick, keyboard,
                    graphics, …)

The whole folder is portable: move or copy it to another machine and your
settings, scenery library and aircraft tweaks come along.


INSTALLATION
------------
1. Extract the ZIP. Keep ProvPilot.exe, ProvPilot.pck and bin\ together,
   for example in C:\Users\YourName\ProvPilot\

2. Run ProvPilot.exe.
   Windows may show a SmartScreen warning ("Windows protected your PC").
   This is expected for software without a Microsoft certificate: click
   "More info", then "Run anyway". The warning does not come back for
   this file.

3. Install scenery: Settings -> Downloads lists the base scenery packs
   (one per country: terrain, airports, roads where baked). Pick the
   countries you fly in and click Download; the packs land in
   <your folder>\scenery\<Country>\ and appear in the Scenery Library
   (Settings -> Scenery) automatically. Sizes run from 0.4 GB
   (Switzerland) to 4 GB (France). Higher-detail add-ons (1 m terrain,
   orthophotos, buildings) are separate packs and are not needed to fly.

4. Pick an aircraft and an airport on the welcome screen, click Fly!.
   The intro zoom doubles as the loading view while the terrain streams in.


AIRCRAFT
--------
  Cessna 172P                 — the trainer, with VOR / ADF / ILS radios
  Pilatus PC-7                — turboprop trainer
  Fairchild A-10              — FlightGear port, clickable cockpit
  McDonnell Douglas MD-81 / MD-82 / MD-83 / MD-87
                              — FlightGear port, airline liveries in
                                Settings -> Aircraft -> Livery
  Schleicher ASK 21           — glider (airborne start in Settings ->
                                Aircraft -> Start until winch and tow exist)
  Vehicles: a car and an Re 4/4 locomotive (welcome screen, Vehicles)


CONTROLS
--------
Every key is listed and rebindable in Settings -> Keyboard; joystick
axes and buttons in Settings -> Joystick. The defaults:

  Joystick        Aileron, elevator, rudder, throttle, brakes (Settings -> Joystick)
  W / S, A / D    Elevator and aileron (keyboard fallback), Y / X rudder
  U / O           Throttle up / down (fine)      P   Pause / resume
  F / Shift+F     Flaps down / up a notch
  G               Landing gear            B      Speed brakes
  . / ,           Wheel brakes on / off    L      Landing / taxi lights
  I / Shift+I     Magneto / starter       R / E  Start / stop the flight model
  C / Shift+C     Next / previous camera (cockpit, orbit, chase)
  Right mouse button + mouse   Pan the view (cockpit head, orbit, chase)
  Space + mouse   Look around in the cockpit (tap Space to recentre)
  Arrow keys      Head look / orbit; PageUp / PageDown zoom
  Ctrl + arrows   Move the eye point; Ctrl+Space resets it
  H               Cycle the HUD           N      VFR map
  Mouse           Click switches and knobs in the cockpit; left button or
                  wheel down turns a knob down / counter-clockwise, right
                  button or wheel up turns it up / clockwise
  Escape          Back to the welcome screen
  Settings -> Logbook lists your flights; Settings -> Realism sets the
  crash detection.
  F8              Scenery editor (developer mode)


SYSTEM REQUIREMENTS
-------------------
  OS      : Windows 10 or 11, 64-bit
  CPU     : Any modern x86-64 (Intel / AMD)
  GPU     : Vulkan 1.0 capable (any GPU of the last ten years; integrated
            graphics work at reduced settings)
  RAM     : 8 GB minimum, 16 GB recommended with detail add-ons
  Storage : ~300 MB for the application, 0.5-6 GB per base scenery country
  Joystick: Optional but recommended (any USB / HID joystick or yoke)

No additional runtime libraries or installers are required.


LICENSES
--------
ProVPilot is free software under the GNU General Public License, version 2.
The source is at https://github.com/Q-coder/provpilot (NOTICE.md there lists
every component).
The A-10, MD-80 family and ASK 21 are ports of FlightGear aircraft
(GPL-2.0); their licence texts and credits are in aircraft\<id>\ after the
first start. Cockpit instruments of the Cessna and PC-7: FlightGear (GPL-2.0).
Base terrain: NASA SRTM; coastline, roads, forests and airports:
(c) OpenStreetMap contributors (ODbL); airports and navaids: OurAirports;
ILS data: X-Plane nav.dat (GPL); Swiss data: swisstopo.
All product names are trademarks of their owners; no affiliation.
