"""Builds vx1000.glb: a low-poly VX-style camcorder with a big fisheye adapter, for the Filmer's
held camera (Extension/Filmer/filmer_hold.h). Made from boxes and cylinders here, so it has no
outside source and no licence to credit.

    python3 assets/models/vx1000/make_vx1000.py assets/models/vx1000/vx1000.glb

Metres, Y up, the lens looking down -Z (as a glTF camera does). The origin is the middle of the top
handle, where the hand grips it: the game places the model there, at the filmer's palm.
"""
import json
import math
import struct
import sys

# name: base colour (linear RGB), metallic, roughness
MATERIALS = {
    "body": ((0.035, 0.035, 0.038), 0.15, 0.55),
    "panel": ((0.07, 0.07, 0.075), 0.2, 0.5),
    "rubber": ((0.012, 0.012, 0.012), 0.0, 0.9),
    "metal": ((0.32, 0.32, 0.34), 0.9, 0.35),
    "glass": ((0.02, 0.03, 0.06), 0.6, 0.05),
    "tally": ((0.8, 0.02, 0.01), 0.0, 0.4),
}


class Mesh:
    def __init__(self):
        self.parts = {name: ([], [], []) for name in MATERIALS}  # positions, normals, indices

    def add(self, material, positions, normals, triangles):
        p, n, i = self.parts[material]
        base = len(p)
        p.extend(positions)
        n.extend(normals)
        i.extend(base + t for t in triangles)

    def box(self, material, lo, hi):
        (x0, y0, z0), (x1, y1, z1) = lo, hi
        faces = [  # normal, four corners counter-clockwise seen from outside
            ((1, 0, 0), [(x1, y0, z0), (x1, y1, z0), (x1, y1, z1), (x1, y0, z1)]),
            ((-1, 0, 0), [(x0, y0, z1), (x0, y1, z1), (x0, y1, z0), (x0, y0, z0)]),
            ((0, 1, 0), [(x0, y1, z0), (x0, y1, z1), (x1, y1, z1), (x1, y1, z0)]),
            ((0, -1, 0), [(x0, y0, z1), (x0, y0, z0), (x1, y0, z0), (x1, y0, z1)]),
            ((0, 0, 1), [(x1, y0, z1), (x1, y1, z1), (x0, y1, z1), (x0, y0, z1)]),
            ((0, 0, -1), [(x0, y0, z0), (x0, y1, z0), (x1, y1, z0), (x1, y0, z0)]),
        ]
        for normal, corners in faces:
            self.add(material, corners, [normal] * 4, [0, 1, 2, 0, 2, 3])

    def bevel_box(self, material, lo, hi, r):
        """A box with its four edges along Z cut off at 45 degrees (an octagonal section)."""
        (x0, y0, z0), (x1, y1, z1) = lo, hi
        ring = [(x0 + r, y0), (x1 - r, y0), (x1, y0 + r), (x1, y1 - r), (x1 - r, y1), (x0 + r, y1), (x0, y1 - r), (x0, y0 + r)]
        self.prism(material, ring, z0, z1)

    def prism(self, material, ring, z0, z1):
        """A shape of section `ring` (x, y, counter-clockwise from +Z) from z0 to z1, ends capped."""
        count = len(ring)
        for k in range(count):
            (ax, ay), (bx, by) = ring[k], ring[(k + 1) % count]
            nx, ny = by - ay, -(bx - ax)
            length = math.hypot(nx, ny) or 1.0
            normal = (nx / length, ny / length, 0.0)
            self.add(material, [(ax, ay, z0), (bx, by, z0), (bx, by, z1), (ax, ay, z1)], [normal] * 4, [0, 1, 2, 0, 2, 3])
        cx = sum(x for x, _ in ring) / count
        cy = sum(y for _, y in ring) / count
        for z, normal, flip in ((z1, (0, 0, 1), False), (z0, (0, 0, -1), True)):
            positions = [(cx, cy, z)] + [(x, y, z) for x, y in ring]
            triangles = []
            for k in range(count):
                a, b = 1 + k, 1 + (k + 1) % count
                triangles += [0, b, a] if flip else [0, a, b]
            self.add(material, positions, [normal] * len(positions), triangles)

    def cylinder_z(self, material, centre, radius, z0, z1, sides=20, radius1=None, caps=True):
        """Along Z from z0 to z1; radius1 tapers it to a cone frustum."""
        r0, r1 = radius, radius if radius1 is None else radius1
        cx, cy = centre
        slope = (r0 - r1) / (z1 - z0) if z1 != z0 else 0.0
        for k in range(sides):
            a0, a1 = 2 * math.pi * k / sides, 2 * math.pi * (k + 1) / sides
            positions, normals = [], []
            for a, r, z in ((a0, r0, z0), (a1, r0, z0), (a1, r1, z1), (a0, r1, z1)):
                positions.append((cx + r * math.cos(a), cy + r * math.sin(a), z))
                length = math.hypot(1.0, slope)
                normals.append((math.cos(a) / length, math.sin(a) / length, slope / length))
            self.add(material, positions, normals, [0, 1, 2, 0, 2, 3])
        if caps:
            for z, r, normal, flip in ((z0, r0, (0, 0, -1), False), (z1, r1, (0, 0, 1), True)):
                positions = [(cx, cy, z)] + [(cx + r * math.cos(2 * math.pi * k / sides), cy + r * math.sin(2 * math.pi * k / sides), z) for k in range(sides)]
                triangles = []
                for k in range(sides):
                    a, b = 1 + k, 1 + (k + 1) % sides
                    triangles += [0, a, b] if flip else [0, b, a]
                self.add(material, positions, [normal] * len(positions), triangles)

    def dome_z(self, material, centre, radius, z_base, depth, sides=20, rings=6):
        """A dome on the -Z side of z_base: an ellipsoid cap of the given depth."""
        cx, cy = centre
        positions, normals, triangles = [], [], []
        for j in range(rings + 1):
            t = j / rings * (math.pi / 2)
            r, z = radius * math.cos(t), z_base - depth * math.sin(t)
            for k in range(sides):
                a = 2 * math.pi * k / sides
                positions.append((cx + r * math.cos(a), cy + r * math.sin(a), z))
                n = (math.cos(t) * math.cos(a) / radius, math.cos(t) * math.sin(a) / radius, -math.sin(t) / depth)
                length = math.sqrt(sum(v * v for v in n))
                normals.append(tuple(v / length for v in n))
        for j in range(rings):
            for k in range(sides):
                a, b = j * sides + k, j * sides + (k + 1) % sides
                c, d = a + sides, b + sides
                triangles += [a, c, b, b, c, d]
        self.add(material, positions, normals, triangles)


def build():
    m = Mesh()
    # The body: a long box under the handle, its front where the lens starts.
    m.bevel_box("body", (-0.056, -0.205, -0.10), (0.056, -0.075, 0.15), 0.012)
    # A lighter side panel (the tape door) and the grip strap side.
    m.box("panel", (-0.0585, -0.19, -0.04), (-0.054, -0.10, 0.12))
    m.box("rubber", (0.054, -0.195, -0.07), (0.062, -0.09, 0.10))
    # The top handle, its two posts down to the body, and the microphone out front.
    m.bevel_box("rubber", (-0.016, -0.008, -0.085), (0.016, 0.018, 0.105), 0.006)
    m.box("body", (-0.014, -0.078, -0.085), (0.014, -0.006, -0.06))
    m.box("body", (-0.014, -0.078, 0.08), (0.014, -0.006, 0.105))
    m.cylinder_z("rubber", (0.0, 0.006), 0.012, -0.165, -0.085, sides=12)
    m.cylinder_z("metal", (0.0, 0.006), 0.0125, -0.168, -0.16, sides=12)
    # The viewfinder at the back, on the left, with its rubber eyecup.
    m.box("body", (-0.082, -0.10, 0.09), (-0.044, -0.06, 0.17))
    m.cylinder_z("rubber", (-0.063, -0.08), 0.019, 0.17, 0.195, sides=14)
    # A red tally light on the front of the body.
    m.box("tally", (0.03, -0.09, -0.103), (0.042, -0.082, -0.099))
    # The camera's own lens barrel, then the big fisheye on it: a wide metal-ringed body, a short
    # flared hood and the bulging front glass.
    m.cylinder_z("body", (0.0, -0.14), 0.038, -0.15, -0.10, sides=20)
    m.cylinder_z("metal", (0.0, -0.14), 0.041, -0.158, -0.15, sides=20)
    m.cylinder_z("body", (0.0, -0.14), 0.068, -0.215, -0.158, sides=24, radius1=0.045)
    m.cylinder_z("metal", (0.0, -0.14), 0.07, -0.222, -0.215, sides=24)
    m.cylinder_z("rubber", (0.0, -0.14), 0.074, -0.238, -0.222, sides=24)
    m.dome_z("glass", (0.0, -0.14), 0.062, -0.238, 0.04, sides=24, rings=6)
    return m


def write_glb(mesh, path):
    binary = bytearray()
    buffer_views, accessors, primitives, materials = [], [], [], []

    def view(data, target):
        while len(binary) % 4:
            binary.append(0)
        buffer_views.append({"buffer": 0, "byteOffset": len(binary), "byteLength": len(data), "target": target})
        binary.extend(data)
        return len(buffer_views) - 1

    for index, (name, (colour, metallic, roughness)) in enumerate(MATERIALS.items()):
        materials.append({"name": name, "pbrMetallicRoughness": {"baseColorFactor": [*colour, 1.0],
                          "metallicFactor": metallic, "roughnessFactor": roughness}})
        positions, normals, indices = mesh.parts[name]
        if not indices:
            continue
        p = view(b"".join(struct.pack("<3f", *v) for v in positions), 34962)
        n = view(b"".join(struct.pack("<3f", *v) for v in normals), 34962)
        i = view(b"".join(struct.pack("<H", v) for v in indices), 34963)
        lo = [min(v[k] for v in positions) for k in range(3)]
        hi = [max(v[k] for v in positions) for k in range(3)]
        accessors.append({"bufferView": p, "componentType": 5126, "count": len(positions), "type": "VEC3", "min": lo, "max": hi})
        accessors.append({"bufferView": n, "componentType": 5126, "count": len(normals), "type": "VEC3"})
        accessors.append({"bufferView": i, "componentType": 5123, "count": len(indices), "type": "SCALAR"})
        primitives.append({"attributes": {"POSITION": len(accessors) - 3, "NORMAL": len(accessors) - 2},
                           "indices": len(accessors) - 1, "material": index})
    while len(binary) % 4:
        binary.append(0)
    document = {
        "asset": {"version": "2.0", "generator": "ReSkate make_vx1000.py"},
        "scene": 0, "scenes": [{"nodes": [0]}],
        "nodes": [{"name": "vx1000", "mesh": 0}],
        "meshes": [{"name": "vx1000", "primitives": primitives}],
        "materials": materials, "accessors": accessors, "bufferViews": buffer_views,
        "buffers": [{"byteLength": len(binary)}],
    }
    text = json.dumps(document, separators=(",", ":")).encode()
    while len(text) % 4:
        text += b" "
    with open(path, "wb") as f:
        f.write(struct.pack("<III", 0x46546C67, 2, 12 + 8 + len(text) + 8 + len(binary)))
        f.write(struct.pack("<II", len(text), 0x4E4F534A) + text)
        f.write(struct.pack("<II", len(binary), 0x004E4942) + binary)
    triangles = sum(len(i) for _, _, i in mesh.parts.values()) // 3
    print(f"{path}: {triangles} triangles")


if __name__ == "__main__":
    write_glb(build(), sys.argv[1] if len(sys.argv) > 1 else "vx1000.glb")
