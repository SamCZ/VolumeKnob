"""
Fusion 360 script that generates the 45-degree desk stand for the CrowPanel 1.28" rotary knob.

Run it in a Fusion design (Utilities > Scripts and Add-Ins, or the Fusion MCP script runner).
It builds the stand in the knob's own coordinate frame (the frame of `3D file/00-irad-128.stp` in
Elecrow's CrowPanel 1.28" repository, which can be imported alongside it for fit checks:
+Y = screen direction, back face of the white plate at Y = -7.71 mm), adds a copy in print
orientation (skirt on Z = 0), and exports STL / STEP / 3MF next to this file. Re-running it
replaces the stand components it created before.

The stand's front wall replaces the knob's outer white back plate (YAKELI-B, 2.0 mm), so the two
stock M2x6 screws go through the stand into the knob exactly as they went through that plate.
"""
import math
import os

import adsk.core
import adsk.fusion

MM = 0.1  # the Fusion API works in cm

# --- parameters (mm) ---------------------------------------------------------------------------
PLATE_OUT = -7.71   # outer face of the removed white plate: screw heads seat here
PLATE_IN = -5.71    # inner face: exactly 2.0 mm, same as the white plate
LIP_TOP = -4.20     # 1.5 mm locating lip around the knob
R_OUT = 25.5        # stand outer radius
R_CAV = 23.1        # cavity behind the plate (clears every connector opening and both screw heads)
R_POCKET = 23.30    # pocket for the knob's 46.02 mm acrylic stack
TILT = 45.0         # screen angle from the desk
DESK_C = -25.5      # desk plane offset along the desk 'up' vector (lifts the front rim ~2 mm)
TOL = 0.3           # print clearance per side for the rectangular openings
SCREW_D = 2.7       # M2 clearance
BUTTON_D = 3.2      # tact switch actuators
SKIRT_W = 5.0       # flange around the foot (stability and bed adhesion)
SKIRT_T = 2.0

FLOOR_T = 7.0         # solid floor at the back of the foot (mass; flat underside for tape)
FLOOR_FRONT_Y = -20.0 # thick floor starts 12 mm behind the plate, so connector plugs keep their room
FRONT_FLOOR_T = 2.0   # thin floor under the connectors: closes the bottom, leaves >= 10.9 mm for plugs
ACCESS_R = 3.5        # screwdriver holes through the floor, coaxial with the screws
CABLE_D = 10.0        # rear cable hole: 5 mm USB cable, and room to thread the 4-pin plug through
CABLE_U = -12.0       # hole centre height in desk 'up' coordinates (desk is DESK_C): 13.5 mm above the desk

SCREWS = [(13.51, 13.51), (-13.51, -13.51)]
BUTTONS = [(-4.15, -8.095), (4.555, -8.105)]
RECT_OPENINGS = [  # (x0, x1, z0, z1) copied from the white plate
    (11.20, 13.65, -5.90, 6.30),    # 12-pin FPC (power / programming)
    (-15.00, -11.40, 0.85, 8.05),   # 4-pin 1.25 mm
    (-15.10, -11.50, -9.85, -2.65), # 4-pin 1.25 mm
    (10.68, 14.28, -14.60, -7.00),  # 4-pin 1.25 mm
]

# Exports go next to this script; set OUT_DIR by hand if your runner does not define __file__.
OUT_DIR = os.path.dirname(os.path.abspath(__file__)) if '__file__' in globals() else os.getcwd()
NAME = 'Knob Stand 45deg'


def P(x, y, z):
    return adsk.core.Point3D.create(x * MM, y * MM, z * MM)


def V(x, y, z):
    return adsk.core.Vector3D.create(x, y, z)


def build_stand():
    """Returns the stand as a temporary BRep body in the knob frame."""
    tb = adsk.fusion.TemporaryBRepManager.get()
    DIFF = adsk.fusion.BooleanTypes.DifferenceBooleanType
    UNION = adsk.fusion.BooleanTypes.UnionBooleanType
    INTER = adsk.fusion.BooleanTypes.IntersectionBooleanType

    a = math.radians(TILT)
    U = V(0, math.cos(a), math.sin(a))   # desk 'up', expressed in the knob frame
    F = V(0, math.sin(a), -math.cos(a))  # towards the user

    def cyl(r, y0, y1, x=0.0, z=0.0):
        return tb.createCylinderOrCone(P(x, y0, z), r * MM, P(x, y1, z), r * MM)

    def box(cx, cy, cz, lx, ly, lz):
        obb = adsk.core.OrientedBoundingBox3D.create(P(cx, cy, cz), V(1, 0, 0), V(0, 1, 0), lx * MM, ly * MM, lz * MM)
        return tb.createBox(obb)

    def deskbox(cx, cf, cu, lx, lf, lu):
        """Box aligned with the desk: x, 'towards user' F and 'up' U, centre given in those coordinates."""
        c = P(cx, F.y * cf + U.y * cu, F.z * cf + U.z * cu)
        return tb.createBox(adsk.core.OrientedBoundingBox3D.create(c, V(1, 0, 0), F, lx * MM, lf * MM, lu * MM))

    def slab(u0, u1):
        return deskbox(0, 0, (u0 + u1) / 2, 400, 400, u1 - u0)

    def isect(*bodies):
        b = bodies[0]
        for t in bodies[1:]:
            tb.booleanOperation(b, t, INTER)
        return b

    # tube + skirt
    stand = cyl(R_OUT, -150, LIP_TOP)
    tb.booleanOperation(stand, isect(cyl(R_OUT + SKIRT_W, -150, LIP_TOP), slab(DESK_C, DESK_C + SKIRT_T)), UNION)
    tb.booleanOperation(stand, cyl(R_CAV, -160, PLATE_OUT), DIFF)
    tb.booleanOperation(stand, cyl(R_POCKET, PLATE_IN, LIP_TOP + 1), DIFF)

    # front wall = replacement for the white plate
    t0, t1 = PLATE_OUT - 1, PLATE_IN + 0.5
    for x, z in SCREWS:
        tb.booleanOperation(stand, cyl(SCREW_D / 2, t0, t1, x, z), DIFF)
    for x, z in BUTTONS:
        tb.booleanOperation(stand, cyl(BUTTON_D / 2, t0, t1, x, z), DIFF)
    for x0, x1, z0, z1 in RECT_OPENINGS:
        tb.booleanOperation(stand, box((x0 + x1) / 2, (t0 + t1) / 2, (z0 + z1) / 2,
                                       x1 - x0 + 2 * TOL, t1 - t0, z1 - z0 + 2 * TOL), DIFF)

    # solid floor across the back of the foot: one flat underside for double-sided tape
    floor_top = DESK_C + FLOOR_T
    tb.booleanOperation(stand, isect(cyl(R_OUT, -150, FLOOR_FRONT_Y), slab(DESK_C - 1, floor_top)), UNION)
    tb.booleanOperation(stand, isect(cyl(R_OUT, FLOOR_FRONT_Y - 1, PLATE_OUT), slab(DESK_C - 1, DESK_C + FRONT_FLOOR_T)), UNION)

    # rear cable hole, round, horizontal (straight back along -F)
    r = CABLE_D / 2
    def along_f(f):
        return P(0, F.y * f + U.y * CABLE_U, F.z * f + U.z * CABLE_U)
    tb.booleanOperation(stand, tb.createCylinderOrCone(along_f(-20), r * MM, along_f(-95), r * MM), DIFF)

    # screwdriver access through the floor, coaxial with each screw
    for x, z in SCREWS:
        tb.booleanOperation(stand, cyl(ACCESS_R, -160, FLOOR_FRONT_Y + 2, x, z), DIFF)

    # desk plane
    tb.booleanOperation(stand, slab(DESK_C - 300, DESK_C), DIFF)
    return stand


def print_transform():
    """Knob frame -> print frame: desk 'up' becomes +Z, 'towards user' becomes +Y, desk at Z = 0."""
    s, c = math.sin(math.radians(TILT)), math.cos(math.radians(TILT))
    m = adsk.core.Matrix3D.create()
    m.setWithCoordinateSystem(adsk.core.Point3D.create(0, 0, -DESK_C * MM), V(1, 0, 0), V(0, s, c), V(0, -c, s))
    return m


def run(_context: str):
    app = adsk.core.Application.get()
    design = adsk.fusion.Design.cast(app.activeProduct)
    if design.designType != adsk.fusion.DesignTypes.DirectDesignType:
        design.designType = adsk.fusion.DesignTypes.DirectDesignType
    root = design.rootComponent
    for occ in [o for o in root.occurrences if o.component.name.startswith(NAME)]:
        occ.deleteMe()
    tb = adsk.fusion.TemporaryBRepManager.get()

    stand = build_stand()
    occ = root.occurrences.addNewComponent(adsk.core.Matrix3D.create())
    occ.component.name = NAME
    occ.component.bRepBodies.add(stand).name = 'Stand'

    printed = tb.copy(stand)
    tb.transform(printed, print_transform())
    pocc = root.occurrences.addNewComponent(adsk.core.Matrix3D.create())
    pocc.component.name = NAME + ' (print orientation)'
    pbody = pocc.component.bRepBodies.add(printed)
    pbody.name = 'Stand'

    os.makedirs(OUT_DIR, exist_ok=True)
    em = design.exportManager
    stl = em.createSTLExportOptions(pbody, os.path.join(OUT_DIR, 'knob_stand_45.stl'))
    stl.meshRefinement = adsk.fusion.MeshRefinementSettings.MeshRefinementHigh
    em.execute(stl)
    em.execute(em.createSTEPExportOptions(os.path.join(OUT_DIR, 'knob_stand_45.step'), pocc.component))
    em.execute(em.createC3MFExportOptions(pbody, os.path.join(OUT_DIR, 'knob_stand_45.3mf')))
    pocc.isLightBulbOn = False

    bb = pbody.boundingBox
    print(f'stand: {pbody.volume * 1000:.0f} mm3 (~{pbody.volume * 1.24:.0f} g PLA if solid), '
          f'solid={pbody.isSolid}, lumps={pbody.lumps.count}')
    print(f'print size: {(bb.maxPoint.x - bb.minPoint.x) * 10:.1f} x {(bb.maxPoint.y - bb.minPoint.y) * 10:.1f} '
          f'x {(bb.maxPoint.z - bb.minPoint.z) * 10:.1f} mm, z min {bb.minPoint.z * 10:.2f}')
    print('exported to', OUT_DIR)
