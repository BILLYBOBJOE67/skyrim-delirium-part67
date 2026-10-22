//! Our own Delirium character, made from the XP32 skeleton (joint positions and bone rotations in joints.json) and
//! public anthropometry: segment masses, part sizes, joint ranges and joint strengths. No GTA IV data.
//! Writes an nm-character-1 file (with its build) and the rig that puts its parts on Skyrim's bones.
//!
//! What follows Delirium's behaviours (so they bend joints the right way) is only convention: which way each joint's
//! axes point, which joints are hinges, the mirrored right side, which way the zero pose faces, and that the zero pose
//! holds the arms out from the body. Every number is ours.
//!
//! usage: cargo run --release --example make_character -- <joints.json> <out character.json> <out rig.txt>
//! env: ARM_OUT=deg (zero pose: upper arms out from the body, default 55), TORQUE=x (all joint strengths, default 1)
//!      G_<JOINT>=x (gain factor) and MS_<JOINT>=x (muscle stiffness) per joint group, e.g. G_SPINE=0.9 MS_ANKLE=3

use delirium::prelude::*;
use delirium_physics::character::{BuildPart, Character, FORMAT, Frame, Joint, JointLimit, Part};

const NAMES: [&str; 21] = [
    "Buttocks", "Thigh_Left", "Shin_Left", "Foot_Left", "Thigh_Right", "Shin_Right", "Foot_Right", "Spine0", "Spine1", "Spine2",
    "Spine3", "Neck", "Head", "Clavicle_Left", "Upper_Arm_Left", "Lower_Arm_Left", "Hand_Left", "Clavicle_Right", "Upper_Arm_Right",
    "Lower_Arm_Right", "Hand_Right",
];
const PARENT: [i32; 21] = [-1, 0, 1, 2, 0, 4, 5, 0, 7, 8, 9, 10, 11, 10, 13, 14, 15, 10, 17, 18, 19];
const JOINTS: [&str; 20] = [
    "hip_left", "knee_left", "ankle_left", "hip_right", "knee_right", "ankle_right", "spine_0", "spine_1", "spine_2", "spine_3",
    "neck_lower", "neck_upper", "clavicle_left", "shoulder_left", "elbow_left", "wrist_left", "clavicle_right", "shoulder_right",
    "elbow_right", "wrist_right",
];
const RIGHT: [usize; 7] = [4, 5, 6, 17, 18, 19, 20];

// Segment masses, % of an 80 kg man (after Winter's segment tables; the trunk split over pelvis, four spine parts and
// the shoulder girdles; head and neck split 6.9 / 1.2). The trunk carries its weight low (abdomen over chest) and the
// feet include the boots: with the chest heavier the balancer reads a high centre of mass and shots tip her over
const MASS_PCT: [f32; 21] = [14.2, 10.0, 4.65, 2.0, 10.0, 4.65, 2.0, 9.0, 8.0, 7.0, 7.4, 1.2, 6.9, 1.5, 2.8, 1.6, 0.6, 1.5, 2.8, 1.6, 0.6];
const BODY_KG: f32 = 80.0;

fn main() {
    let a: Vec<String> = std::env::args().collect();
    assert!(a.len() == 4, "usage: make_character <joints.json> <out character.json> <out rig.txt>");
    let j: serde_json::Value = serde_json::from_str(&std::fs::read_to_string(&a[1]).unwrap()).unwrap();
    let env = |k: &str, d: f32| std::env::var(k).ok().and_then(|s| s.parse().ok()).unwrap_or(d);
    let (arm_out, torque) = (env("ARM_OUT", 55.0).to_radians(), env("TORQUE", 1.0));

    // Skyrim's bind pose: joint pivots (the root's is the pelvis), bone rotations (row-major, world = R * local)
    let vec = |v: &serde_json::Value| { let f: Vec<f32> = v.as_array().unwrap().iter().map(|x| x.as_f64().unwrap() as f32).collect(); Vec3::new(f[0], f[1], f[2]) };
    let pv: Vec<Vec3> = NAMES.iter().map(|n| vec(&j["joints"][n])).collect();
    let bone_rot = |bone: &str| {
        let r: Vec<f32> = j["rot"][bone].as_array().unwrap_or_else(|| panic!("no rotation for {bone}")).iter().map(|x| x.as_f64().unwrap() as f32).collect();
        Quat::from_mat3(&Mat3::from_cols_array(&r.try_into().unwrap()).transpose()).normalize()
    };
    let up = Vec3::Z;
    let left = (pv[1] - pv[4]).normalize();
    let fwd = left.cross(up).normalize(); // right x fwd = up
    let (right, back) = (-left, -fwd);
    let side = |k: usize| if RIGHT.contains(&k) { right } else { left }; // outward, for this part's side
    let mirror = |v: Vec3| v - 2.0 * v.dot(left) * left;

    // the far end of each part, and where it points
    let tip = |k: usize| -> Vec3 {
        match k {
            0 => pv[0] + up * 0.1,
            3 | 6 => pv[k] + fwd * 0.15,
            12 => pv[12] + up * 0.2,
            16 | 20 => pv[k] + (pv[k] - pv[k - 1]).normalize() * 0.18,
            _ => pv[(0..21).find(|&c| PARENT[c] == k as i32).unwrap()],
        }
    };
    let along = |k: usize| (tip(k) - pv[k]).normalize();

    // part frames (bind): the capsule runs along y. Pelvis and spine lie across the body, limbs point back to their
    // joint, z is the body's back for everything but the shoulder girdles
    let yz = |y: Vec3, zh: Vec3| { let y = y.normalize(); let z = (zh - y * zh.dot(y)).normalize(); Quat::from_mat3(&Mat3::from_cols(y.cross(z), y, z)).normalize() };
    let xy = |xh: Vec3, y: Vec3| { let y = y.normalize(); let x = (xh - y * xh.dot(y)).normalize(); Quat::from_mat3(&Mat3::from_cols(x, y, x.cross(y))).normalize() };
    let rot: Vec<Quat> = (0..21).map(|k| match k {
        0 => xy(up, left),
        7..=10 => xy(along(k), left),
        11 | 12 => yz(along(k), back),
        3 | 6 => yz(up, back),
        13 | 17 => xy(up, -along(k)),
        _ => yz(-along(k), back),
    }).collect();

    // sizes (m): capsule radius and half length (between the cap centres), or a box's half extents
    let len = |k: usize| (tip(k) - pv[k]).length();
    let limb = |k: usize, r: f32| (r, (len(k) * 0.5 - 0.8 * r).max(0.01));
    let size: Vec<(f32, f32, Option<Vec3>)> = (0..21).map(|k| match k {
        0 => (0.11, 0.06, None),
        1 | 4 => { let (r, h) = limb(k, 0.075); (r, h, None) }
        2 | 5 => { let (r, h) = limb(k, 0.055); (r, h, None) }
        3 | 6 => (0.04, 0.0, Some(Vec3::new(0.05, 0.04, 0.135))), // boxes: radius = half height (the spawn finds the floor from it)
        7 => (0.10, 0.05, None),
        8 => (0.10, 0.06, None),
        9 => (0.105, 0.065, None),
        10 => (0.10, 0.08, None),
        11 => (0.06, len(k) * 0.5, None), // neck: caps reach into the chest and the head (no self-collision pairs)
        12 => (0.085, 0.03, None),
        13 | 17 => { let (r, h) = limb(k, 0.04); (r, h, None) }
        14 | 18 => { let (r, h) = limb(k, 0.048); (r, h, None) }
        15 | 19 => { let (r, h) = limb(k, 0.04); (r, h, None) }
        _ => (0.03, 0.0, Some(Vec3::new(0.03, 0.08, 0.045))),
    }).collect();
    // part centres: the torso sits in front of the spine bones, the head above its joint, the foot ahead of the ankle
    let centre: Vec<Vec3> = (0..21).map(|k| match k {
        0 => pv[0],
        7..=9 => (pv[k] + tip(k)) * 0.5 + fwd * 0.06,
        10 => (pv[k] + tip(k)) * 0.5 + fwd * 0.05,
        12 => pv[12] + up * 0.09 + fwd * 0.03,
        3 | 6 => pv[k] + fwd * 0.065 - up * 0.045,
        16 | 20 => pv[k] + along(k) * 0.08,
        _ => (pv[k] + tip(k)) * 0.5,
    }).collect();

    // zero pose: the bind pose with the upper arms turned out to arm_out from straight down (pure abduction)
    let mut zrot = rot.clone();
    let (mut zc, mut zp) = (centre.clone(), pv.clone());
    for (sh, s) in [(14usize, 1.0f32), (18, -1.0)] {
        let u = along(sh);
        let now = (u.dot(side(sh))).atan2(-u.dot(up));
        let turn = Quat::from_axis_angle(fwd * s, arm_out - now);
        for k in [sh, sh + 1, sh + 2] {
            zrot[k] = (turn * rot[k]).normalize();
            zc[k] = pv[sh] + turn * (centre[k] - pv[sh]);
            zp[k] = pv[sh] + turn * (pv[k] - pv[sh]);
        }
    }
    // and the legs straight down (as Fred's zero pose has them): the balancer stands the pelvis at 0.99 of this pose's
    // pelvis-to-feet height, so a bent or splayed zero leg would make it crouch
    for h in [1usize, 4] {
        let th = Quat::from_rotation_arc(along(h), -up);
        for k in [h, h + 1, h + 2] {
            zrot[k] = (th * rot[k]).normalize();
            zc[k] = pv[h] + th * (centre[k] - pv[h]);
            zp[k] = pv[h] + th * (pv[k] - pv[h]);
        }
        let (kn, o) = (h + 1, zp[h + 1]);
        let tk = Quat::from_rotation_arc((zp[kn + 1] - o).normalize(), -up);
        for k in [kn, kn + 1] {
            zrot[k] = (tk * zrot[k]).normalize();
            zc[k] = o + tk * (zc[k] - o);
            zp[k] = o + tk * (zp[k] - o);
        }
        // the foot stays flat (shin upright over a flat foot: the ankle's neutral)
        let f = h + 2;
        zrot[f] = rot[f];
        zc[f] = zp[f] + (centre[f] - pv[f]);
    }
    let zalong = |k: usize| match k {
        1..=6 | 14..=16 | 18..=20 => (zrot[k] * (rot[k].inverse() * along(k))).normalize(),
        _ => along(k),
    };

    // joints: twist axis (z) along the child part, lean axis (x) the body's front; the ankle twists along the foot and
    // leans about up; knees and elbows are hinges about the body's right
    let hinge = |k: usize| matches!(k, 2 | 5 | 15 | 19);
    let axes = |k: usize| -> (Vec3, Vec3) {
        match k {
            3 | 6 => (fwd, up),
            2 | 5 => (right, right.cross(Vec3::Z)),
            15 | 19 => {
                let n = zalong(k - 1).cross(zalong(k)).normalize();
                let n = if n.dot(right) < 0.0 { -n } else { n };
                (n, n.cross(Vec3::Z))
            }
            _ => (zalong(k), fwd),
        }
    };
    // the joint frame as Delirium builds it (nmrs::joints): z twist, x lean made square to it, y = z x x
    let frame = |k: usize| { let (z, x0) = axes(k); let z = z.normalize(); let x = (x0 - z * x0.dot(z)).normalize(); (x, z.cross(x), z) };

    // ranges (deg), from an upright neutral, as rotations about body axes for the left side and the middle:
    // (axis, low, high). Delirium's raw angles: lean1 about the frame's y, lean2 about -x, twist about z.
    let bend = |a: Vec3, b: Vec3, about: Vec3| a.cross(b).dot(about).atan2(a.dot(b)).to_degrees();
    let knee_bent = bend(zalong(1), zalong(2), -right); // flexion: about the body's left (shin swings back)
    let elbow_bent = bend(zalong(14), zalong(15), axes(15).0);
    let hip_out = bend(-up, zalong(1), fwd);
    let motions = |k: usize| -> Vec<(Vec3, f32, f32)> {
        let al = zalong(k);
        match k {
            1 => vec![(right, -25.0, 120.0), (fwd, -25.0 - hip_out, 45.0 - hip_out), (al, -45.0, 40.0)],
            2 => vec![(-right, -3.0 - knee_bent, 145.0 - knee_bent)],
            // centred and wide in flexion: Delirium aims a joint from the middle of its range, and the balancer needs
            // room both ways at the ankle to catch a hit (an off-centre or narrow range drops her on shots)
            3 => vec![(right, -55.0, 45.0), (-up, -17.0, 17.0), (-fwd, -22.0, 22.0)],
            7 => vec![(-right, -15.0, 25.0), (-fwd, -10.0, 10.0), (up, -5.0, 5.0)],
            8 => vec![(-right, -12.0, 20.0), (-fwd, -12.0, 12.0), (up, -8.0, 8.0)],
            9 => vec![(-right, -10.0, 15.0), (-fwd, -12.0, 12.0), (up, -15.0, 15.0)],
            10 => vec![(-right, -10.0, 12.0), (-fwd, -10.0, 10.0), (up, -15.0, 15.0)],
            11 => vec![(-right, -30.0, 25.0), (-fwd, -20.0, 20.0), (up, -35.0, 35.0)],
            12 => vec![(-right, -30.0, 25.0), (-fwd, -20.0, 20.0), (up, -40.0, 40.0)],
            13 => vec![(-up, -25.0, 20.0), (fwd, -10.0, 35.0), (al, -20.0, 20.0)],
            14 => {
                let out = arm_out.to_degrees();
                vec![(al.cross(fwd).normalize(), -50.0, 110.0), (fwd, -30.0 - out, 120.0 - out), (al, -60.0, 70.0)]
            }
            15 => vec![(axes(15).0, -elbow_bent, 145.0 - elbow_bent)],
            16 => vec![(-fwd, -60.0, 60.0), (al.cross(fwd).normalize(), -30.0, 25.0), (al, -40.0, 40.0)],
            _ => panic!("no ranges for {k}"),
        }
    };
    // the same part on the left, for a right one
    let twin = |k: usize| match k { 4 | 5 | 6 => k - 3, 17..=20 => k - 4, _ => k };
    let limits = |k: usize| -> [[f32; 2]; 3] {
        let (x, y, z) = frame(k);
        let mut out = [[0.0f32; 2]; 3];
        let mut seen = [false; 3];
        for (a, lo, hi) in motions(twin(k)) {
            // a right part moves as the mirror image: about -M(a), same angle
            let a = if twin(k) != k { -mirror(a) } else { a };
            let a = a.normalize();
            let (slot, s) = if hinge(k) {
                (0, a.dot(z).signum())
            } else {
                let d = [a.dot(y), -a.dot(x), a.dot(z)];
                let m = (0..3).max_by(|&p, &q| d[p].abs().total_cmp(&d[q].abs())).unwrap();
                if d[m].abs() < 0.7 {
                    eprintln!("note: {} motion axis is {:.2} off its joint axis", NAMES[k], d[m].abs());
                }
                (m, d[m].signum())
            };
            assert!(!seen[slot], "{}: two motions on one axis", NAMES[k]);
            seen[slot] = true;
            let (l, h) = ((s * lo).to_radians(), (s * hi).to_radians());
            out[slot] = [l.min(h), l.max(h)];
        }
        out
    };

    // muscles: gain k (strength k^2, damping 2k; behaviours set their own), peak torques in N m (lean, twist) after
    // published isometric joint strengths for adult men
    let muscle = |i: usize| -> (f32, [f32; 2]) {
        match JOINTS[i].trim_end_matches("_left").trim_end_matches("_right") {
            "hip" => (12.0, [220.0, 70.0]),
            "knee" => (12.0, [200.0, 0.0]),
            "ankle" => (9.0, [150.0, 40.0]),
            "spine_0" => (12.0, [220.0, 90.0]),
            "spine_1" => (12.0, [200.0, 80.0]),
            "spine_2" => (12.0, [180.0, 70.0]),
            "spine_3" => (12.0, [160.0, 60.0]),
            "neck_lower" => (8.0, [50.0, 25.0]),
            "neck_upper" => (8.0, [40.0, 20.0]),
            "clavicle" => (10.0, [70.0, 30.0]),
            "shoulder" => (10.0, [90.0, 45.0]),
            "elbow" => (10.0, [75.0, 0.0]),
            _ => (9.0, [15.0, 8.0]), // wrist
        }
    };

    // parts that can touch (not neighbours): arms with the torso, legs and head; legs with each other and the trunk
    let mut pairs: Vec<[usize; 2]> = vec![];
    let mut add = |a: &[usize], b: &[usize]| for &p in a { for &q in b { if p != q && !pairs.iter().any(|&[x, y]| (x, y) == (p.min(q), p.max(q))) { pairs.push([p.min(q), p.max(q)]); } } };
    for arm in [[14usize, 15, 16], [18, 19, 20]] {
        add(&arm[1..], &[0, 7, 8, 9, 10]);
        add(&arm[..1], &[0, 7, 8]);
        add(&arm, &[1, 2, 4, 5, 12]);
    }
    add(&[15, 16], &[19, 20]);
    add(&[1, 2, 3], &[4, 5, 6]);
    add(&[1, 4], &[8, 9, 10]);
    add(&[2, 3, 5, 6], &[7, 8, 9, 10, 12]);
    add(&[1, 4], &[12]);

    // the character: asset coordinates flip x and y, and the zero pose faces the other way to Skyrim's bind pose
    // (Delirium's convention), which cancel: the asset holds the zero pose in Skyrim's own coordinates
    let mrows = |q: Quat, c: Vec3| { let m = Mat3::from_quat(q); [m.row(0).to_array(), m.row(1).to_array(), m.row(2).to_array(), c.to_array()] };
    let joints: Vec<Joint> = (0..20).map(|i| {
        let k = i + 1;
        let (z, x0) = axes(k);
        let (g, max) = muscle(i);
        let grp = JOINTS[i].trim_end_matches("_left").trim_end_matches("_right").trim_end_matches(|c: char| c.is_ascii_digit()).trim_end_matches('_').to_uppercase();
        let env = |k: String, d: f32| std::env::var(k).ok().and_then(|v| v.parse().ok()).unwrap_or(d);
        let (g, ms) = (g * env(format!("G_{grp}"), 1.0), env(format!("MS_{grp}"), 1.0));
        let r = RIGHT.contains(&k) && !hinge(k);
        Joint {
            name: JOINTS[i].into(), index: i as u32, three: !hinge(k), pos: zp[k].to_array(), twist_axis: z.to_array(), lean_axis: x0.to_array(),
            limits: limits(k), flip: [false, r, r], muscle_stiffness: ms, damping: 2.0 * g, strength: g * g, max_torque: max.map(|m| m * torque),
        }
    }).collect();
    let parts: Vec<Part> = (0..21).map(|k| Part { name: NAMES[k].into(), frame: mrows(zrot[k], zc[k]), mass: MASS_PCT[k] * BODY_KG / 100.0 }).collect();
    let fr = |p: Vec3, q: Quat| Frame { pos: p.to_array(), rot: q.to_array() };
    let build: Vec<BuildPart> = (0..21).map(|k| {
        let (radius, half, boxed) = size[k];
        let (anchor, anchor_parent) = if PARENT[k] < 0 {
            (rot[0].inverse() * ((pv[1] + pv[4]) * 0.5 - centre[0]), Vec3::ZERO)
        } else {
            let p = PARENT[k] as usize;
            (rot[k].inverse() * (pv[k] - centre[k]), rot[p].inverse() * (pv[k] - centre[p]))
        };
        BuildPart {
            name: NAMES[k].into(), parent: (PARENT[k] >= 0).then(|| PARENT[k] as usize), mass: MASS_PCT[k] * BODY_KG / 100.0, radius, half,
            offset: fr(Vec3::ZERO, Quat::IDENTITY), rest: Quat::IDENTITY.to_array(), anchor_parent: anchor_parent.to_array(), anchor: anchor.to_array(),
            limit: JointLimit::Cone { swing: 1.0, twist: 0.5 }, lower: k <= 6, reach: matches!(k, 14 | 15 | 18 | 19), bind: rot[k].to_array(),
            boxed: boxed.map(|e| e.to_array()), inertia: [0.0; 3], frame: fr(centre[k], rot[k]), body_limit: None,
        }
    }).collect();
    let ch = Character { format: FORMAT.into(), joints, parts, self_collision: pairs, build: Some(build) };
    let text = serde_json::to_string_pretty(&ch).unwrap();
    std::fs::write(&a[2], &text).unwrap();

    // the rig (plugin): ref bone rotation = part rotation * offset, the joint in the part's and the parent's frame.
    // Skyrim's chest bone (Spine2) carries the clavicles and neck, like our Spine3: Spine3 drives it, Spine2 only reads it
    let (spn2, neck) = ("NPC Spine2 [Spn2]", "NPC Neck [Neck]");
    let f = (pv[1] - pv[4]).cross(Vec3::Z);
    let mut rig = format!("yaw {}\n", f.y.atan2(f.x));
    for k in 0..21 {
        let (rb, write, ja, jb) = match NAMES[k] {
            "Spine2" => (spn2.to_string(), 0, spn2.to_string(), spn2.to_string()),
            "Spine3" => (spn2.to_string(), 1, spn2.to_string(), neck.to_string()),
            n => {
                let b = j["bones"][n].as_str().unwrap_or_else(|| panic!("no bone for {n}")).to_string();
                (b.clone(), 1, b.clone(), b)
            }
        };
        let off = (rot[k].inverse() * bone_rot(&rb)).normalize();
        let b = &ch.build.as_ref().unwrap()[k];
        let (an, ip) = (b.anchor, b.anchor_parent);
        rig += &format!("{}|{rb}|{write}|{ja}|{jb}|{} {} {} {}|{} {} {}|{} {} {}\n", NAMES[k], off.x, off.y, off.z, off.w, an[0], an[1], an[2], ip[0], ip[1], ip[2]);
    }
    std::fs::write(&a[3], rig).unwrap();

    // checks with Delirium's own joint code: each frame axis turns the angle it should, and the bind pose is in range
    let parsed = Character::parse(&text).unwrap();
    let (mut asset, prt, mss, slf) = parsed.asset();
    delirium::nmrs::set(delirium::nmrs::HOMER, asset.clone(), prt, mss, slf);
    let (b, frames) = parsed.build().unwrap();
    let real = delirium::nmrs::joints(&b, &asset).expect("joints");
    for jt in asset.iter_mut() {
        jt.limits = [(0.0, 0.0); 3];
        jt.flip = [false; 3];
    }
    let raw = delirium::nmrs::Character::new(delirium::nmrs::joints(&b, &asset).expect("joints"), &frames);
    // axes checked at the zero pose (all angles 0 there), the range at the bind pose
    let zf: Vec<Transform> = (0..21).map(|k| Transform::from_translation(zc[k]).with_rotation(zrot[k])).collect();
    let mut bad = 0;
    for i in 0..20 {
        let jd = &raw.joints[i];
        let w = zf[jd.parent].rotation * jd.in_parent;
        let base0 = raw.angle(i, &zf);
        assert!(base0.iter().all(|v| v.abs() < 1e-3), "{} not zero at the zero pose: {base0:?}", JOINTS[i]);
        let base = raw.angle(i, &frames);
        for (e, want) in [(Vec3::X, [0.0, -1.0, 0.0]), (Vec3::Y, [1.0, 0.0, 0.0]), (Vec3::Z, [0.0, 0.0, 1.0])] {
            if !jd.three && e != Vec3::Z {
                continue;
            }
            let want = if jd.three { want } else { [1.0, 0.0, 0.0] };
            let mut f2 = zf.clone();
            f2[jd.child].rotation = (Quat::from_axis_angle(w * e, 0.3) * zf[jd.child].rotation).normalize();
            let d = raw.angle(i, &f2);
            let got: Vec<f32> = (0..3).map(|c| d[c] / 0.3).collect();
            if (0..3).any(|c| (got[c] - want[c]).abs() > 0.05) {
                bad += 1;
                println!("AXIS {} {:?}: got {:.2?}, want {:?}", JOINTS[i], e, got, want);
            }
        }
        let l = &real[i].limits;
        let n = if real[i].three { 3 } else { 1 };
        let out: Vec<String> = (0..n).filter(|&c| base[c] < l[c].0 - 1e-3 || base[c] > l[c].1 + 1e-3)
            .map(|c| format!("{} {:.0} not in {:.0}..{:.0}", ["lean1", "lean2", "twist"][c], base[c].to_degrees(), l[c].0.to_degrees(), l[c].1.to_degrees())).collect();
        if !out.is_empty() {
            println!("bind pose: {} {}", JOINTS[i], out.join(", "));
        }
    }
    let kg: f32 = ch.parts.iter().map(|p| p.mass).sum();
    println!("{} parts, {kg:.1} kg, {} collision pairs, knee bent {knee_bent:.0}, elbow bent {elbow_bent:.0}, hip out {hip_out:.0} deg; axis checks {}",
        ch.parts.len(), ch.self_collision.len(), if bad == 0 { "ok" } else { "FAILED" });
    println!("wrote {} and {}", a[2], a[3]);
}
