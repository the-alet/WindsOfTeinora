"""The human from MakeHuman's CC0 assets, converted into our own format (assets/human/): its body shaped by MakeHuman's
targets, its full skeleton (163 bones) fitted to it with MakeHuman's skin weights, eyes, brows, lashes, hair and
clothes fitted over the body (mhclo proxies), the skin hidden under the clothes, smooth normals, tangents, a baked
ambient occlusion, and the textures. The same file serves our engine and the Unreal port.

  .local/venv/Scripts/python.exe -I .utilities/makehuman.py --src .local/downloads/makehuman --out assets/human [--fetch]

--fetch first downloads what is missing into --src: files from github.com/makehumancommunity/makehuman (the base
mesh, targets, skeleton, weights, eyes) and the system asset pack makehuman_system_assets_cc0.zip (skins, brows, lashes,
hair, clothes) from mirror1.makehuman.net. Both CC0 1.0 (LICENSE.ASSETS.md in the MakeHuman repository).

Format of human.bin (little-endian), version 1:
  "TAELHUM1", u32 version
  u32 boneCount; per bone: char name[32], i32 parent, f32 head[3], f32 tail[3]          (metres, model space)
  f32 heart[3], f32 brain[3]                                                             (anatomy for the magic)
  u32 materialCount; per material: char name[32], char albedo[64], char normal[64], u32 flags, f32 roughness,
        f32 specular       flags: 1 alpha-tested, 2 two-sided, 4 skin, 8 eye, 16 hair, 32 cloth
  u32 vertexCount; per vertex (60 bytes): f32 pos[3], f32 normal[3], f32 tangent[4], f32 uv[2], u8 bones[4], u8 weights[4],
        f32 sky seen (baked ambient occlusion)
  u32 submeshCount; per submesh: u32 material, u32 firstIndex, u32 indexCount
  u32 indexCount; u32 indices[]
Model space: metres, the soles on y = 0, facing +z, its left toward +x.
"""
import argparse
import json
import math
import struct
import sys
import urllib.request
import zipfile
from pathlib import Path

import numpy as np
from PIL import Image

REPO_URL = "https://raw.githubusercontent.com/makehumancommunity/makehuman/master/makehuman/data/"
PACK_URL = "https://mirror1.makehuman.net/asset_packs/makehuman_system_assets/makehuman_system_assets_cc0.zip"
REPO_FILES = [
    "3dobjs/base.obj",
    "rigs/default.mhskel",
    "rigs/default_weights.mhw",
    "targets/macrodetails/african-male-young.target",
    "targets/macrodetails/asian-male-young.target",
    "targets/macrodetails/caucasian-male-young.target",
    "targets/macrodetails/universal-male-young-averagemuscle-averageweight.target",
]

HEIGHT = 1.75  # m to the top of the hair
VERTEX = struct.Struct("<3f3f4f2f4B4Bf")  # pos, normal, tangent (xyz, handedness), uv, bones, weights (unorm, sum 255), sky seen

FLAG_ALPHA, FLAG_TWO_SIDED, FLAG_SKIN, FLAG_EYE, FLAG_HAIR, FLAG_CLOTH = 1, 2, 4, 8, 16, 32


# ---------------------------------------------------------------- fetching

def fetch(src: Path):
    repo = src / "repo"
    for rel in REPO_FILES:
        dst = repo / rel
        if dst.exists():
            continue
        dst.parent.mkdir(parents=True, exist_ok=True)
        print("fetching", rel)
        urllib.request.urlretrieve(REPO_URL + rel, dst)
    zp = src / "makehuman_system_assets_cc0.zip"
    if not zp.exists():
        print("fetching the system asset pack (about 280 MB)")
        urllib.request.urlretrieve(PACK_URL, zp)


# ---------------------------------------------------------------- reading MakeHuman's formats

class Obj:
    def __init__(self, path: Path):
        self.v, self.vt, self.faces, self.face_group = [], [], [], []
        group = None
        with open(path, encoding="utf-8", errors="replace") as f:
            for line in f:
                if line.startswith("v "):
                    self.v.append([float(x) for x in line.split()[1:4]])
                elif line.startswith("vt "):
                    self.vt.append([float(x) for x in line.split()[1:3]])
                elif line.startswith("g "):
                    group = line.split()[1] if len(line.split()) > 1 else None
                elif line.startswith("f "):
                    corners = []
                    for t in line.split()[1:]:
                        parts = t.split("/")
                        corners.append((int(parts[0]) - 1, int(parts[1]) - 1 if len(parts) > 1 and parts[1] else -1))
                    self.faces.append(corners)
                    self.face_group.append(group)
        self.v = np.array(self.v, dtype=np.float64)
        self.vt = np.array(self.vt, dtype=np.float64) if self.vt else np.zeros((1, 2))


def read_target(path: Path):
    idx, d = [], []
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if not line or line.startswith("#"):
            continue
        p = line.split()
        idx.append(int(p[0]))
        d.append([float(p[1]), float(p[2]), float(p[3])])
    return np.array(idx, dtype=np.int64), np.array(d, dtype=np.float64).reshape(-1, 3)


class Mhclo:
    """A proxy fitted over the base mesh: each of its vertices from three base vertices and an offset."""

    def __init__(self, path: Path):
        self.path = path
        self.obj_file = self.material = None
        self.scales = {}
        self.refs = []  # (i1, i2, i3, w1, w2, w3, d1, d2, d3)
        self.delete = set()
        section = None
        for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
            s = line.strip()
            if not s or s.startswith("#"):
                continue
            p = s.split()
            key = p[0]
            if key == "verts":
                section = "verts"
                continue
            if key == "delete_verts":
                section = "delete"
                continue
            if section == "verts":
                try:
                    nums = [float(x) for x in p]
                except ValueError:
                    section = None
                else:
                    if len(nums) == 1:
                        self.refs.append((int(nums[0]), int(nums[0]), int(nums[0]), 1.0, 0.0, 0.0, 0.0, 0.0, 0.0))
                    else:
                        self.refs.append((int(nums[0]), int(nums[1]), int(nums[2]), nums[3], nums[4], nums[5], nums[6], nums[7], nums[8]))
                    continue
            if section == "delete":
                ok = True
                i = 0
                while i < len(p):
                    if i + 2 < len(p) and p[i + 1] == "-":
                        self.delete.update(range(int(p[i]), int(p[i + 2]) + 1))
                        i += 3
                    elif p[i].isdigit():
                        self.delete.add(int(p[i]))
                        i += 1
                    else:
                        ok = False
                        break
                if ok:
                    continue
                section = None
            if key == "obj_file":
                self.obj_file = path.parent / p[1]
            elif key == "material":
                self.material = path.parent / p[1]
            elif key in ("x_scale", "y_scale", "z_scale"):
                self.scales[key[0]] = (int(p[1]), int(p[2]), float(p[3]))

    def fit(self, base_v: np.ndarray) -> np.ndarray:
        r = np.array(self.refs, dtype=np.float64)
        i = r[:, :3].astype(np.int64)
        w = r[:, 3:6]
        d = r[:, 6:9]
        scale = np.ones(3)
        for axis, a in (("x", 0), ("y", 1), ("z", 2)):
            if axis in self.scales:
                v1, v2, ref = self.scales[axis]
                scale[a] = abs(base_v[v1, a] - base_v[v2, a]) / ref
        return (base_v[i] * w[:, :, None]).sum(1) + d * scale


def read_mhmat(path: Path):
    m = {"diffuse": None, "normal": None, "transparent": False, "backfaceCull": True, "roughness": None, "diffuseColor": None}
    if path is None or not path.exists():
        return m
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        p = line.strip().split()
        if len(p) < 2:
            continue
        key, val = p[0], " ".join(p[1:])
        if key == "diffuseTexture":
            m["diffuse"] = path.parent / val
        elif key == "normalmapTexture":
            m["normal"] = path.parent / val
        elif key == "transparent":
            m["transparent"] = val.lower() == "true"
        elif key == "backfaceCull":
            m["backfaceCull"] = val.lower() == "true"
        elif key == "roughness":
            m["roughness"] = float(p[1])
        elif key == "diffuseColor":
            m["diffuseColor"] = [float(x) for x in p[1:4]]
    return m


# ---------------------------------------------------------------- building

class Part:
    """One drawable piece: positions, uvs, triangles (corner indices into its own (pos, uv) vertices), weights."""

    def __init__(self, name, material):
        self.name, self.material = name, material
        self.pos = None       # (n, 3) welded positions
        self.uv = None        # (m, 2)
        self.corners = []     # list of (pos index, uv index) per triangle corner, in triangles of 3
        self.weights = None   # (n, bones) sparse as dict list: per pos index, {bone: w}


def triangulate(faces, keep):
    out = []
    for f, k in zip(faces, keep):
        if not k:
            continue
        for t in range(1, len(f) - 1):
            out.extend([f[0], f[t], f[t + 1]])
    return out


def base_weights(weights_json, bone_index, n):
    """Per base vertex: list of (bone, weight)."""
    per = [[] for _ in range(n)]
    for bone, pairs in weights_json["weights"].items():
        b = bone_index[bone]
        for v, w in pairs:
            if v < n:
                per[v].append((b, w))
    return per


def top4(pairs):
    # Proxies blend base weights with coefficients that may be negative: keep what pulls the right way.
    pairs = sorted([(b, w) for b, w in pairs if w > 0], key=lambda p: -p[1])[:4]
    total = sum(w for _, w in pairs)
    if total <= 0:
        return [(0, 1.0)]
    return [(b, w / total) for b, w in pairs]


def blend_weights(per_base, refs):
    out = []
    for r in refs:
        acc = {}
        for k in range(3):
            w = r[3 + k]
            if w == 0:
                continue
            for b, bw in per_base[r[k]]:
                acc[b] = acc.get(b, 0.0) + w * bw
        out.append(top4(list(acc.items())))
    return out


def smooth_normals(pos, tris):
    tris = np.asarray(tris, dtype=np.int64).reshape(-1, 3)
    a, b, c = pos[tris[:, 0]], pos[tris[:, 1]], pos[tris[:, 2]]
    fn = np.cross(b - a, c - a)  # area-weighted
    n = np.zeros_like(pos)
    for k in range(3):
        np.add.at(n, tris[:, k], fn)
    ln = np.linalg.norm(n, axis=1, keepdims=True)
    return n / np.maximum(ln, 1e-12)


def tangents(pos, nrm, uv, tris):
    tris = np.asarray(tris, dtype=np.int64).reshape(-1, 3)
    p0, p1, p2 = pos[tris[:, 0]], pos[tris[:, 1]], pos[tris[:, 2]]
    t0, t1, t2 = uv[tris[:, 0]], uv[tris[:, 1]], uv[tris[:, 2]]
    e1, e2 = p1 - p0, p2 - p0
    d1, d2 = t1 - t0, t2 - t0
    det = d1[:, 0] * d2[:, 1] - d2[:, 0] * d1[:, 1]
    r = np.where(np.abs(det) > 1e-12, 1.0 / np.where(np.abs(det) > 1e-12, det, 1.0), 0.0)
    sdir = (e1 * d2[:, 1:2] - e2 * d1[:, 1:2]) * r[:, None]
    tdir = (e2 * d1[:, 0:1] - e1 * d2[:, 0:1]) * r[:, None]
    tan, bit = np.zeros_like(pos), np.zeros_like(pos)
    for k in range(3):
        np.add.at(tan, tris[:, k], sdir)
        np.add.at(bit, tris[:, k], tdir)
    t = tan - nrm * (tan * nrm).sum(1, keepdims=True)
    lt = np.linalg.norm(t, axis=1, keepdims=True)
    fallback = np.cross(nrm, np.array([0.0, 1.0, 0.0]))
    t = np.where(lt > 1e-9, t / np.maximum(lt, 1e-12), fallback / np.maximum(np.linalg.norm(fallback, axis=1, keepdims=True), 1e-9))
    w = np.where((np.cross(nrm, t) * bit).sum(1) < 0.0, -1.0, 1.0)
    return np.concatenate([t, w[:, None]], axis=1)


def bake_occlusion(pos, nrm, tris, cell=0.004):
    """How much sky each vertex sees: the body rasterised into a grid, then looked through along a cone round the
    normal (0..1)."""
    tris = np.asarray(tris, dtype=np.int64).reshape(-1, 3)
    lo = pos.min(0) - 0.12
    dims = np.ceil((pos.max(0) + 0.12 - lo) / cell).astype(np.int64) + 1
    occ = np.zeros(dims, dtype=bool)
    a, b, c = pos[tris[:, 0]], pos[tris[:, 1]], pos[tris[:, 2]]
    longest = np.maximum(np.maximum(np.linalg.norm(b - a, axis=1), np.linalg.norm(c - b, axis=1)), np.linalg.norm(a - c, axis=1))
    steps = int(np.clip(np.ceil(longest.max() / (cell * 0.7)), 1, 64))
    for i in range(steps + 1):
        for j in range(steps + 1 - i):
            u, v = i / steps, j / steps
            p = a * (1 - u - v) + b * u + c * v
            q = np.floor((p - lo) / cell).astype(np.int64)
            occ[q[:, 0], q[:, 1], q[:, 2]] = True
    # Directions: the normal and eight round it, 50 degrees out.
    up = np.where(np.abs(nrm[:, 1:2]) < 0.9, np.array([[0.0, 1.0, 0.0]]), np.array([[1.0, 0.0, 0.0]]))
    t1 = np.cross(nrm, up)
    t1 /= np.linalg.norm(t1, axis=1, keepdims=True)
    t2 = np.cross(nrm, t1)
    dirs = [nrm]
    for k in range(8):
        ang = 2 * math.pi * k / 8
        d = nrm * math.cos(math.radians(50)) + (t1 * math.cos(ang) + t2 * math.sin(ang)) * math.sin(math.radians(50))
        dirs.append(d)
    reach = [0.012, 0.025, 0.045, 0.075, 0.11]
    seen = np.zeros(len(pos))
    for d in dirs:
        free = np.ones(len(pos), dtype=bool)
        for r in reach:
            q = np.floor((pos + d * r - lo) / cell).astype(np.int64)
            q = np.clip(q, 0, dims - 1)
            free &= ~occ[q[:, 0], q[:, 1], q[:, 2]]
        seen += free
    return 0.25 + 0.75 * seen / len(dirs)


def save_texture(src: Path, dst: Path, size: int, alpha: bool):
    im = Image.open(src)
    im = im.convert("RGBA" if alpha else "RGB")
    if max(im.size) > size:
        im = im.resize((size, size) if im.size[0] == im.size[1] else (size, int(size * im.size[1] / im.size[0])), Image.LANCZOS)
    if alpha:
        im.save(dst.with_suffix(".png"), optimize=True)
        return dst.with_suffix(".png").name
    im.save(dst.with_suffix(".jpg"), quality=90, optimize=True)
    return dst.with_suffix(".jpg").name


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", type=Path, required=True)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--fetch", action="store_true")
    ap.add_argument("--list", action="store_true", help="list the pack's skins, eyes, brows, lashes, hair and clothes")
    a = ap.parse_args()
    if a.fetch:
        fetch(a.src)
    repo = a.src / "repo"
    zp = a.src / "makehuman_system_assets_cc0.zip"
    pack = a.src / "pack"
    if a.list:
        with zipfile.ZipFile(zp) as z:
            for n in z.namelist():
                if n.endswith((".mhclo", ".mhmat", ".proxy")) or "/skins/" in n and n.endswith(".mhmat"):
                    print(n)
        return
    build(repo, zp, pack, a.out)


# Which of the pack's assets it wears (paths in the pack), and its shape: a young adult man, average muscle and weight,
# a mix of MakeHuman's three race targets leaning toward the skin it wears.
PACK_DIRS = ["skins/young_caucasian_male", "eyes/high-poly", "eyes/materials", "eyebrows/eyebrow001", "eyelashes/eyelashes01",
             "hair/short02", "clothes/male_casualsuit01", "clothes/shoes01"]
RACE = {"caucasian": 0.7, "african": 0.15, "asian": 0.15}
SKIN = "skins/young_caucasian_male/young_caucasian_male.mhmat"
EYE_MATERIAL = "eyes/materials/brown.mhmat"
PROXIES = [  # (proxy, its name in our file, flags, roughness, specular, texture size)
    ("eyes/high-poly/high-poly.mhclo", "eyes", FLAG_EYE | FLAG_ALPHA, 0.05, 0.9, 512),
    ("eyebrows/eyebrow001/eyebrow001.mhclo", "brows", FLAG_HAIR | FLAG_ALPHA | FLAG_TWO_SIDED, 0.6, 0.2, 512),
    ("eyelashes/eyelashes01/eyelashes01.mhclo", "lashes", FLAG_HAIR | FLAG_ALPHA | FLAG_TWO_SIDED, 0.6, 0.2, 512),
    ("hair/short02/short02.mhclo", "hair", FLAG_HAIR | FLAG_ALPHA | FLAG_TWO_SIDED, 0.5, 0.35, 1024),
    ("clothes/male_casualsuit01/male_casualsuit01.mhclo", "clothes", FLAG_CLOTH, 0.85, 0.15, 2048),
    ("clothes/shoes01/shoes01.mhclo", "shoes", FLAG_CLOTH, 0.45, 0.4, 1024),
]


def extract(zp: Path, pack: Path, member_dir: str):
    with zipfile.ZipFile(zp) as z:
        for n in z.namelist():
            if n.startswith(member_dir) and not n.endswith("/") and not n.endswith(".thumb"):
                dst = pack / n
                if not dst.exists():
                    dst.parent.mkdir(parents=True, exist_ok=True)
                    dst.write_bytes(z.read(n))


def bones_in_order(skel):
    """Bone names, parents before children (depth first from the roots)."""
    bones = skel["bones"]
    children = {}
    for name, b in bones.items():
        children.setdefault(b["parent"], []).append(name)
    order = []

    def walk(n):
        order.append(n)
        for c in children.get(n, []):
            walk(c)

    for r in children.get(None, []):
        walk(r)
    return order


def unique_corners(faces):
    """(pos, uv) corner pairs of triangles -> vertex indices; the vertices' pos and uv indices."""
    key, src_pos, src_uv, tris = {}, [], [], []
    for corner in faces:
        k = corner
        i = key.get(k)
        if i is None:
            i = key[k] = len(src_pos)
            src_pos.append(corner[0])
            src_uv.append(corner[1])
        tris.append(i)
    return np.array(src_pos, dtype=np.int64), np.array(src_uv, dtype=np.int64), tris


def build(repo: Path, zp: Path, pack: Path, out: Path):
    for d in PACK_DIRS:
        extract(zp, pack, d + "/")
    base = Obj(repo / "3dobjs/base.obj")
    V = base.v.copy()
    # Its shape.
    for race, w in RACE.items():
        idx, d = read_target(repo / f"targets/macrodetails/{race}-male-young.target")
        V[idx] += w * d
    idx, d = read_target(repo / "targets/macrodetails/universal-male-young-averagemuscle-averageweight.target")
    V[idx] += d

    # Its skeleton, fitted to that shape.
    skel = json.loads((repo / "rigs/default.mhskel").read_text(encoding="utf-8"))
    order = bones_in_order(skel)
    bone_index = {n: i for i, n in enumerate(order)}
    joint = {name: V[np.array(vs, dtype=np.int64)].mean(0) for name, vs in skel["joints"].items()}
    heads = np.array([joint[skel["bones"][n]["head"]] for n in order])
    tails = np.array([joint[skel["bones"][n]["tail"]] for n in order])
    parents = [bone_index[skel["bones"][n]["parent"]] if skel["bones"][n]["parent"] else -1 for n in order]
    weights_json = json.loads((repo / "rigs/default_weights.mhw").read_text(encoding="utf-8"))
    per_base = base_weights(weights_json, bone_index, len(V))

    # What it wears, fitted over the body; the skin under the clothes is hidden.
    parts = []  # (name, positions, uv table, triangle corners (pos, uv), weights per position, material)
    hidden = set()
    materials = []
    for rel, name, flags, rough, spec, size in PROXIES:
        clo = Mhclo(pack / rel)
        P = clo.fit(V)
        obj = Obj(clo.obj_file)
        if len(obj.v) != len(P):
            raise SystemExit(f"{rel}: {len(obj.v)} vertices in its obj, {len(P)} in its mhclo")
        corners = []
        for f in obj.faces:
            for t in range(1, len(f) - 1):
                corners.extend([f[0], f[t], f[t + 1]])
        mat = read_mhmat(pack / EYE_MATERIAL if name == "eyes" else clo.material)
        hidden |= clo.delete
        parts.append((name, P, obj.vt, corners, blend_weights(per_base, clo.refs), len(materials)))
        materials.append((name, mat, flags, rough, spec, size))
    # The body, its faces under the clothes left out.
    body_faces = [f for f, g in zip(base.faces, base.face_group) if g == "body"]
    body_all = []
    body_kept = []
    for f in body_faces:
        for t in range(1, len(f) - 1):
            tri = [f[0], f[t], f[t + 1]]
            body_all.extend(tri)
            if not any(c[0] in hidden for c in tri):
                body_kept.extend(tri)
    body_weights = [top4(per_base[i]) for i in range(len(V))]
    parts.insert(0, ("skin", V, base.vt, body_kept, body_weights, len(materials)))
    materials.append(("skin", read_mhmat(pack / SKIN), FLAG_SKIN, 0.5, 0.35, 2048))

    # Into metres: the soles on y = 0, the top of the hair at HEIGHT.
    lo = min(np.asarray(p[1])[np.unique([c[0] for c in p[3]])][:, 1].min() for p in parts)
    hi = max(np.asarray(p[1])[np.unique([c[0] for c in p[3]])][:, 1].max() for p in parts)
    scale = HEIGHT / (hi - lo)
    to_m = lambda p: (np.asarray(p) - np.array([0.0, lo, 0.0])) * scale

    # Each part's vertices: positions welded for smooth normals, split where its uvs split.
    out.mkdir(parents=True, exist_ok=True)
    all_vertices, all_indices, submeshes = [], [], []
    body_normals = smooth_normals(to_m(V), [c[0] for c in body_all])
    for name, P, VT, corners, weights, mat in parts:
        Pm = to_m(P)
        used = sorted({c[0] for c in corners})
        if not used:
            continue
        nrm = body_normals if name == "skin" else smooth_normals(Pm, [c[0] for c in corners])
        src_pos, src_uv, tris = unique_corners(corners)
        pos = Pm[src_pos]
        uv = np.stack([VT[np.maximum(src_uv, 0), 0], 1.0 - VT[np.maximum(src_uv, 0), 1]], axis=1)
        n = nrm[src_pos]
        tan = tangents(pos, n, uv, tris)
        first = len(all_indices)
        base_index = len(all_vertices)
        for k in range(len(pos)):
            all_vertices.append((pos[k], n[k], tan[k], uv[k], weights[src_pos[k]]))
        all_indices.extend(base_index + t for t in tris)
        submeshes.append((mat, first, len(tris)))
        print(f"{name}: {len(pos)} vertices, {len(tris) // 3} triangles")

    # How much sky each vertex sees.
    P_all = np.array([v[0] for v in all_vertices])
    N_all = np.array([v[1] for v in all_vertices])
    ao = bake_occlusion(P_all, N_all, all_indices)

    # Where its life magic is densest: the heart, in the chest a little to its left; the brain, in the skull.
    def depth_at(y, xmax=0.03):
        sel = (np.abs(P_all[:, 0]) < xmax) & (np.abs(P_all[:, 1] - y) < 0.015)
        return P_all[sel, 2].min(), P_all[sel, 2].max()
    spine01 = to_m(heads[bone_index["spine01"]])
    hy = spine01[1] - 0.03
    back, front = depth_at(hy)
    heart = np.array([0.018, hy, back + 0.58 * (front - back)])
    head_h, head_t = to_m(heads[bone_index["head"]]), to_m(tails[bone_index["head"]])
    by = head_h[1] + 0.55 * (head_t[1] - head_h[1])
    sback, sfront = depth_at(by)
    brain = np.array([0.0, by, sback + 0.45 * (sfront - sback)])
    print("heart", heart.round(3), "brain", brain.round(3), "height", round(float(P_all[:, 1].max()), 3))

    # The textures.
    mat_records = []
    for name, mat, flags, rough, spec, size in materials:
        alpha = bool(flags & FLAG_ALPHA)
        albedo = ""
        if mat["diffuse"] is not None and mat["diffuse"].exists():
            src = mat["diffuse"]
            ao_map = src.with_name(src.name.replace("_diffuse", "_ao"))
            if ao_map.exists() and not alpha:  # the cloth's creases darkened into its colour
                im = Image.open(src).convert("RGB")
                a = Image.open(ao_map).convert("L").resize(im.size)
                arr = np.asarray(im, dtype=np.float64) * (np.asarray(a, dtype=np.float64)[:, :, None] / 255.0) ** 0.7
                tmp = out / f"{name}_tmp.png"
                Image.fromarray(np.clip(arr, 0, 255).astype(np.uint8)).save(tmp)
                albedo = save_texture(tmp, out / f"{name}_albedo", size, alpha)
                tmp.unlink()
            else:
                albedo = save_texture(src, out / f"{name}_albedo", size, alpha)
        normal = ""
        if mat["normal"] is not None and mat["normal"].exists():
            normal = save_texture(mat["normal"], out / f"{name}_normal", size, False)
        mat_records.append((name, albedo, normal, flags | (FLAG_TWO_SIDED if not mat["backfaceCull"] else 0), rough, spec))

    # The file.
    def text(s, n):
        b = s.encode("utf-8")[: n - 1]
        return b + b"\0" * (n - len(b))
    with open(out / "human.bin", "wb") as f:
        f.write(b"TAELHUM1")
        f.write(struct.pack("<I", 1))
        f.write(struct.pack("<I", len(order)))
        for i, n in enumerate(order):
            f.write(text(n, 32))
            f.write(struct.pack("<i3f3f", parents[i], *to_m(heads[i]), *to_m(tails[i])))
        f.write(struct.pack("<3f3f", *heart, *brain))
        f.write(struct.pack("<I", len(mat_records)))
        for name, albedo, normal, flags, rough, spec in mat_records:
            f.write(text(name, 32) + text(albedo, 64) + text(normal, 64))
            f.write(struct.pack("<Iff", flags, rough, spec))
        f.write(struct.pack("<I", len(all_vertices)))
        for k, (p, n, t, uv, w) in enumerate(all_vertices):
            bones = [b for b, _ in w] + [0] * (4 - len(w))
            ws = [int(round(x * 255)) for _, x in w] + [0] * (4 - len(w))
            ws[0] += 255 - sum(ws)
            f.write(VERTEX.pack(*p, *n, *t, *uv, *bones, *ws, float(ao[k])))
        f.write(struct.pack("<I", len(submeshes)))
        for mat, first, count in submeshes:
            f.write(struct.pack("<III", mat, first, count))
        f.write(struct.pack("<I", len(all_indices)))
        f.write(np.asarray(all_indices, dtype=np.uint32).tobytes())
    (out / "LICENSE.txt").write_text(
        "The human in this folder is made from MakeHuman's assets (makehumancommunity.org): its base mesh, targets,\n"
        "skeleton and skin weights, skin, eyes, eyebrows, eyelashes, hair and clothes, all released under CC0 1.0\n"
        "Universal (LICENSE.ASSETS.md in github.com/makehumancommunity/makehuman). Converted by .utilities/makehuman.py.\n",
        encoding="utf-8")
    print("wrote", out / "human.bin", f"{len(order)} bones, {len(all_vertices)} vertices, {len(all_indices) // 3} triangles")


if __name__ == "__main__":
    main()
