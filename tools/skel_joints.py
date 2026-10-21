"""Read a Skyrim skeleton.nif and write the 21 joint positions nm_build_fit wants (metres, Z up, NM part order).

usage: python skel_joints.py <skeleton.nif> <out.json>

Each NM part's joint is where it meets its parent (thigh: hip, shin: knee, ...); Buttocks is the pelvis centre.
Also writes each bone's bind world rotation (row-major 3x3, world = R @ local, as NiMatrix3).
Skyrim has three spine bones where NM has four parts, so Spine3's joint is put halfway from Spine2 to the neck.
"""
import json
import sys

sys.path.insert(0, r"C:\path\to\PyNifly\io_scene_nifly")
from pyn.pynifly import NifFile  # noqa: E402

UNIT = 0.0142875  # metres per Skyrim game unit (Havok world scale)

# NM part -> Skyrim bone whose origin is that part's joint with its parent
BONES = {
    "Buttocks": "NPC Pelvis [Pelv]",
    "Thigh_Left": "NPC L Thigh [LThg]", "Shin_Left": "NPC L Calf [LClf]", "Foot_Left": "NPC L Foot [Lft ]",
    "Thigh_Right": "NPC R Thigh [RThg]", "Shin_Right": "NPC R Calf [RClf]", "Foot_Right": "NPC R Foot [Rft ]",
    "Spine0": "NPC Spine [Spn0]", "Spine1": "NPC Spine1 [Spn1]", "Spine2": "NPC Spine2 [Spn2]",
    "Spine3": None,  # halfway Spine2 -> Neck
    "Neck": "NPC Neck [Neck]", "Head": "NPC Head [Head]",
    "Clavicle_Left": "NPC L Clavicle [LClv]", "Upper_Arm_Left": "NPC L UpperArm [LUar]",
    "Lower_Arm_Left": "NPC L Forearm [LLar]", "Hand_Left": "NPC L Hand [LHnd]",
    "Clavicle_Right": "NPC R Clavicle [RClv]", "Upper_Arm_Right": "NPC R UpperArm [RUar]",
    "Lower_Arm_Right": "NPC R Forearm [RLar]", "Hand_Right": "NPC R Hand [RHnd]",
}


def main(nif_path, out_path):
    nif = NifFile(nif_path)
    nodes = nif.nodes

    def pos(name):
        n = nodes.get(name)
        if n is None:
            raise SystemExit(f"bone not found: {name}")
        t = n.global_transform
        return [t.translation[0] * UNIT, t.translation[1] * UNIT, t.translation[2] * UNIT]

    def rot(name):
        r = nodes[name].global_transform.rotation
        return [r[i][j] for i in range(3) for j in range(3)]

    joints, rots = {}, {}
    for part, bone in BONES.items():
        if bone is None:
            a, b = pos(BONES["Spine2"]), pos(BONES["Neck"])
            joints[part] = [(x + y) / 2 for x, y in zip(a, b)]
        else:
            joints[part] = pos(bone)
            rots[bone] = rot(bone)
    with open(out_path, "w") as f:
        json.dump({"source": nif_path, "unit_m": UNIT, "joints": joints, "bones": BONES, "rot": rots}, f, indent=1)
    for k, v in joints.items():
        print(f"{k:16} {v[0]:7.3f} {v[1]:7.3f} {v[2]:7.3f}")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
