# 45° desk stand

A stand that holds the knob with its screen at 45° to the desk. Its front wall **replaces the
knob's outer white back plate**, so the two stock M2 screws are reused. The back is closed except
for a round cable hole, and the bottom is one flat face for double-sided tape.

| File | Use |
|---|---|
| `knob_stand_45.3mf` / `knob_stand_45.stl` | Print files, already in print orientation (bottom on the bed) |
| `knob_stand_45.step` | CAD exchange |
| `make_stand_fusion.py` | Fusion 360 script that regenerates everything; all dimensions are parameters at the top |

## Print

- 61 × 87 mm footprint, 41 mm tall; about 29 cm³ (37 g of PLA if printed solid).
- Print as exported, bottom down. **No supports**: walls overhang 45° at most. The top of the
  round Ø10 mm cable hole bridges fine on most printers; it may sag a few tenths of a millimetre.
- 0.2 mm layers, 4 walls. Use high infill (50 %+) if you want it heavier; the 7 mm floor at the
  back is solid plastic.
- The 2.0 mm front wall matches the thickness of the white plate, so the screw length stays right.

## Bottom and cable

- **Bottom:** fully closed, a single flat face (about 40 cm²) for double-sided tape. The only
  openings are the two Ø7 mm screwdriver holes, which the tape can cover after assembly. Under
  the connectors the floor is 2 mm thick, so every plug keeps at least 12 mm of room behind the
  back face (12-pin FPC 17.6 mm, 4-pin connectors 12.3 / 12.3 / 21.9 mm).
- **Cable:** a Ø10 mm hole straight out of the back, centred 13.5 mm above the desk. It fits the
  5 mm USB cable, and the 4-pin plug can be threaded through it. The cable can run from any of the
  three 4-pin connectors to this hole; this was checked in CAD with a 5 mm cable.

## Assemble

1. Remove the two M2 screws on the back of the knob and lift off the outer **white** plate
   (the outermost 2 mm layer). Keep the screws.
2. Push the cable's 4-pin plug in through the rear hole, out through the front, and plug it into
   the knob. All four connector openings and both button holes of the white plate are in the
   stand's front wall, enlarged 0.3 mm per side.
3. Seat the knob in the locating lip so the holes line up, pulling the slack back out through the
   rear hole.
4. Drive the screws in from underneath, through the two holes in the floor. Each hole is in line
   with its screw:
   - lower screw: about 15 mm reach;
   - upper screw: about 42 mm reach, so use a screwdriver with a shaft of at least 45 mm.
5. Tape the bottom to the desk.

## Checked in CAD

- No overlap with any part of the knob model (`3D file/00-irad-128.stp` in
  [Elecrow's repository](https://github.com/Elecrow-RD/CrowPanel-1.28inch-HMI-ESP32-Rotary-Display-240-240-IPS-Round-Touch-Knob-Screen)) except the screw heads seating
  on the wall, which is intended.
- Balance: the foot reaches 17.6 mm in front of the knob's axis and 68 mm behind it. The centre of
  mass of knob + stand is about 18 mm behind the front edge, before any tape.

## Orientation

The stand's holes only line up one way. Mounted, the two small button holes and two of the
4-pin connectors sit on the lower half of the back. This holds the knob upside down compared
with the factory orientation, so the firmware turns the picture 180°
(`kFlip180` in `src/board.cpp`).
