//! Phase 0: does Homer's body, fitted to the Skyrim skeleton, stand, react and fall like Homer?
//!
//! usage: fit-test <Fred.art> <homer_build.txt> <joints.json>
//! Runs the same scenarios on Homer's own body and on the fitted one and prints a table.

use delirium_physics::*;
use std::ffi::CString;

const DT: f32 = 1.0 / 60.0;

fn c(s: &str) -> CString {
    CString::new(s).unwrap()
}

fn err() -> String {
    let mut b = vec![0u8; 512];
    unsafe { nm_last_error(b.as_mut_ptr() as *mut _, b.len() as i32) };
    String::from_utf8_lossy(&b).trim_end_matches('\0').to_string()
}

#[derive(Clone, Copy)]
enum Scene {
    Stand,
    Shove(f32),
    /// impulse, direction: 0 backward (hit from the front), 1 forward (from behind), 2 to the left side
    Push(f32, u8),
    Shot,
}

struct Out {
    head_min: f32,
    head_end: f32,
    fell_at: Option<f32>,
    balancing_end: bool,
    max_ke: f32,
    finite: bool,
    ms: f64,
}

unsafe fn part_pos(g: *mut NmAgent, i: i32) -> [f32; 3] {
    let mut p = [0.0f32; 3];
    unsafe { nm_agent_get_part_transform(g, i, p.as_mut_ptr(), std::ptr::null_mut()) };
    p
}

unsafe fn run(build: *const NmBuild, w: *mut NmWorld, scene: Scene, secs: f32) -> Out {
    unsafe {
        let g = nm_agent_create(build, [0.0f32; 3].as_ptr(), 0.0, 0.3);
        assert!(!g.is_null(), "agent: {}", err());
        nm_agent_set_mode(g, NM_MODE_LEAD);
        nm_msg_begin(g, c("bodyBalance").as_ptr());
        nm_msg_set_bool(g, c("start").as_ptr(), 1);
        nm_msg_send(g, w);
        let head0 = part_pos(g, 12)[2];
        let mut o = Out { head_min: f32::MAX, head_end: 0.0, fell_at: None, balancing_end: false, max_ke: 0.0, finite: true, ms: 0.0 };
        let steps = (secs / DT) as usize;
        let t0 = std::time::Instant::now();
        for k in 0..steps {
            let t = k as f32 * DT;
            if k == 60 {
                let chest = part_pos(g, 10);
                match scene {
                    Scene::Stand => {}
                    Scene::Shove(n) => {
                        nm_agent_apply_push(g, chest.as_ptr(), [n, 0.0, 0.0].as_ptr());
                    }
                    Scene::Push(n, d) => {
                        // the body's own axes: left = left hip - right hip, forward = left x up
                        let (l, r) = (part_pos(g, 1), part_pos(g, 4));
                        let side = [l[0] - r[0], l[1] - r[1]];
                        let len = (side[0] * side[0] + side[1] * side[1]).sqrt().max(1e-4);
                        let (left, fwd) = ([side[0] / len, side[1] / len], [side[1] / len, -side[0] / len]);
                        let dir = match d { 0 => [-fwd[0], -fwd[1]], 1 => fwd, _ => left };
                        nm_agent_apply_push(g, chest.as_ptr(), [n * dir[0], n * dir[1], 0.0].as_ptr());
                    }
                    Scene::Shot => {
                        nm_agent_apply_push(g, chest.as_ptr(), [0.0f32, 40.0, 0.0].as_ptr());
                        nm_agent_task_shot(g, w, c("*").as_ptr(), 10, chest.as_ptr(), [0.0f32, 1.0, 0.0].as_ptr(), 200.0);
                    }
                }
            }
            let rc = nm_agent_step(g, w, DT);
            let h = part_pos(g, 12)[2];
            if std::env::var("DBG").is_ok() && (59..66).contains(&k) {
                let (mut v, mut w2) = ([0.0f32; 3], [0.0f32; 3]);
                nm_agent_get_part_velocity(g, 10, v.as_mut_ptr(), w2.as_mut_ptr());
                eprintln!("  k={k} awake={} chest v={v:?} head={h:.4}", nm_agent_is_awake(g));
            }
            if std::env::var("DBG").is_ok() && k % 30 == 0 {
                eprintln!("  k={k} rc={rc} head={h:.4} awake={} ke={:.2} bal={} fallen={}", nm_agent_is_awake(g),
                    nm_agent_get_kinetic_energy(g), nm_agent_is_balancing(g), nm_agent_has_fallen(g));
            }
            if !h.is_finite() {
                o.finite = false;
                break;
            }
            o.head_min = o.head_min.min(h);
            o.max_ke = o.max_ke.max(nm_agent_get_kinetic_energy(g));
            if o.fell_at.is_none() && (nm_agent_has_fallen(g) != 0 || h < head0 * 0.4) {
                o.fell_at = Some(t);
            }
        }
        o.ms = t0.elapsed().as_secs_f64() * 1e3 / steps as f64;
        o.head_end = part_pos(g, 12)[2] / head0;
        o.head_min /= head0;
        o.balancing_end = nm_agent_is_balancing(g) != 0;
        nm_agent_destroy(g);
        o
    }
}

/// One part of a rig line, parsed.
#[derive(Clone)]
struct RigLine {
    head: String, // name|ref|write|jointA|jointB
    off: glam::Quat,
    anchor: glam::Vec3,
    inp: glam::Vec3,
}

fn parse_rig(rig: &str) -> (String, Vec<RigLine>) {
    use glam::{Quat, Vec3};
    let mut lines = rig.lines();
    let hdr = lines.next().unwrap().to_string();
    let v = lines.map(|l| {
        let f: Vec<&str> = l.split('|').collect();
        let n = |k: usize| f[k].split(' ').map(|x| x.parse::<f32>().unwrap()).collect::<Vec<f32>>();
        RigLine { head: f[..5].join("|"), off: Quat::from_slice(&n(5)), anchor: Vec3::from_slice(&n(6)), inp: Vec3::from_slice(&n(7)) }
    }).collect();
    (hdr, v)
}

fn format_rig(hdr: &str, rig: &[RigLine]) -> String {
    let mut s = format!("{hdr}\n");
    for r in rig {
        s += &format!("{}|{} {} {} {}|{} {} {}|{} {} {}\n", r.head, r.off.x, r.off.y, r.off.z, r.off.w, r.anchor.x, r.anchor.y, r.anchor.z,
            r.inp.x, r.inp.y, r.inp.z);
    }
    s
}

/// A Skyrim pose per part: ref bone world rotation, joint bones A and B (metres).
type Pose = Vec<(glam::Quat, glam::Vec3, glam::Vec3)>;

fn read_pose(path: &str) -> Pose {
    use glam::{Mat3, Quat, Vec3};
    let txt = std::fs::read_to_string(path).unwrap_or_else(|e| panic!("{path}: {e}"));
    let nums = |s: &str| s.split(' ').filter(|x| !x.is_empty()).map(|x| x.parse::<f32>().unwrap()).collect::<Vec<f32>>();
    txt.lines().skip(1).take(21).map(|l| {
        let f: Vec<&str> = l.split('|').collect();
        let m = nums(f[1]);
        let q = Quat::from_mat3(&Mat3::from_cols_array(&m.try_into().unwrap()).transpose()).normalize();
        (q, Vec3::from_slice(&nums(f[3])), Vec3::from_slice(&nums(f[4])))
    }).collect()
}

fn bind_pose(rig: &str, j: &serde_json::Value) -> Pose {
    use glam::{Mat3, Quat, Vec3};
    let bpos = |bone: &str| {
        let part = j["bones"].as_object().unwrap().iter().find(|(_, v)| v.as_str() == Some(bone)).unwrap().0;
        let v: Vec<f32> = j["joints"][part].as_array().unwrap().iter().map(|x| x.as_f64().unwrap() as f32).collect();
        Vec3::new(v[0], v[1], v[2])
    };
    let rot = |bone: &str| {
        let r: Vec<f32> = j["rot"][bone].as_array().unwrap().iter().map(|x| x.as_f64().unwrap() as f32).collect();
        Quat::from_mat3(&Mat3::from_cols_array(&r.try_into().unwrap()).transpose()).normalize()
    };
    rig.lines().skip(1).map(|l| {
        let f: Vec<&str> = l.split('|').collect();
        (rot(f[1]), bpos(f[3]), bpos(f[4]))
    }).collect()
}

/// Parts placed from a pose through the rig, as the plugin does.
unsafe fn place(body: *const NmBuild, rig: &[RigLine], pose: &Pose) -> Vec<(glam::Vec3, glam::Quat)> {
    let mut out: Vec<(glam::Vec3, glam::Quat)> = vec![];
    for (i, r) in rig.iter().enumerate() {
        let q = (pose[i].0 * r.off.inverse()).normalize();
        let p = if i == 0 {
            pose[0].1 - q * r.anchor
        } else {
            let mut par = 0;
            unsafe { nm_build_part_info(body, i as i32, std::ptr::null_mut(), std::ptr::null_mut(), std::ptr::null_mut(), &mut par) };
            let (pp, pq) = out[par as usize];
            pp + pq * r.inp - q * r.anchor
        };
        out.push((p, q));
    }
    out
}

/// How far each joint snaps (deg, relative to its parent) in the first steps after placing a pose and holding it.
unsafe fn snaps(body: *const NmBuild, rig: &[RigLine], pose: &Pose, parent: &[i32; 21]) -> [f32; 21] {
    use glam::Quat;
    unsafe {
        let start = place(body, rig, pose);
        let w = nm_world_create();
        nm_world_add_floor(w, 0.0, 0.0, -5.0, 50.0);
        let g = nm_agent_create(body, [0.0f32; 3].as_ptr(), 0.0, 0.3);
        nm_agent_set_mode(g, NM_MODE_LEAD);
        let mut tf = vec![];
        for (i, (p, q)) in start.iter().enumerate() {
            nm_agent_set_part_state(g, i as i32, p.to_array().as_ptr(), q.to_array().as_ptr(), [0.0f32; 3].as_ptr(), [0.0f32; 3].as_ptr());
            tf.extend_from_slice(&p.to_array());
            tf.extend_from_slice(&q.to_array());
        }
        nm_agent_set_zero_pose(g, tf.as_ptr(), 21);
        for _ in 0..4 {
            nm_agent_step(g, w, DT);
        }
        let get = |i: usize| {
            let mut q = [0.0f32; 4];
            nm_agent_get_part_transform(g, i as i32, std::ptr::null_mut(), q.as_mut_ptr());
            Quat::from_slice(&q)
        };
        let mut s = [0.0f32; 21];
        for i in 1..21 {
            let p = parent[i] as usize;
            s[i] = (get(p).inverse() * get(i)).angle_between(start[p].1.inverse() * start[i].1).to_degrees();
        }
        nm_agent_destroy(g);
        nm_world_destroy(w);
        s
    }
}

/// Twist each single-child part (and leaf) about its own bone axis to the angle where the poses snap least.
/// Visuals don't change (bone = part * offset both ways); only which way the physics part faces about its bone.
unsafe fn calibrate(body: *const NmBuild, rig: &str, poses: &[Pose]) -> String {
    use glam::Quat;
    let (hdr, mut rig) = parse_rig(rig);
    let mut parent = [-1i32; 21];
    for i in 1..21 {
        unsafe { nm_build_part_info(body, i as i32, std::ptr::null_mut(), std::ptr::null_mut(), std::ptr::null_mut(), &mut parent[i]) };
    }
    let kids = |i: usize| (0..21).filter(|&c| parent[c] == i as i32).collect::<Vec<usize>>();
    let cost = |rig: &[RigLine], i: usize| -> f32 {
        // this joint and its children's joints (its twist moves both), over every pose
        let mut c = 0.0;
        for p in poses {
            let s = unsafe { snaps(body, rig, p, &parent) };
            c += s[i] + kids(i).iter().map(|&k| s[k]).sum::<f32>();
        }
        c
    };
    let total = |rig: &[RigLine]| -> (f32, [f32; 21]) {
        let mut per = [0.0f32; 21];
        for p in poses {
            let s = unsafe { snaps(body, rig, p, &parent) };
            for i in 0..21 {
                per[i] += s[i] / poses.len() as f32;
            }
        }
        (per.iter().sum::<f32>() / 20.0, per)
    };
    let name = |r: &RigLine| r.head.split('|').next().unwrap().to_string();
    let (t0, per0) = total(&rig);
    // CALIB_SKIP=Thigh,Shin: leave parts whose name contains any of these as they are
    let skip_list = std::env::var("CALIB_SKIP").unwrap_or_default();
    let skip = |r: &RigLine| skip_list.split(',').filter(|w| !w.is_empty()).any(|w| r.head.split('|').next().unwrap().contains(w));
    for pass in 0..2 {
        for i in 1..21usize {
            let k = kids(i);
            if k.len() > 1 || skip(&rig[i]) {
                continue; // the chest: its children span a plane, no single axis
            }
            let axis = match k[..] {
                [c] => rig[c].inp - rig[i].anchor,
                _ => -rig[i].anchor,
            };
            if axis.length() < 1e-4 {
                continue;
            }
            let axis = axis.normalize();
            let base = rig[i].off;
            let (mut best, mut best_c) = (0.0f32, cost(&rig, i));
            let span = if pass == 0 { 180 } else { 15 };
            let stepd = if pass == 0 { 10 } else { 3 };
            for d in (-span..=span).step_by(stepd) {
                if d == 0 {
                    continue;
                }
                let a = (d as f32).to_radians();
                rig[i].off = (Quat::from_axis_angle(axis, a) * base).normalize();
                let c = cost(&rig, i);
                if c < best_c - 0.5 {
                    (best, best_c) = (a, c);
                }
            }
            rig[i].off = (Quat::from_axis_angle(axis, best) * base).normalize();
            if best != 0.0 {
                println!("  calib pass {pass}: {} twist {:+.0} deg (cost {:.1})", name(&rig[i]), best.to_degrees(), best_c);
            }
        }
    }
    // then a small tilt too, about the part's own axes (coordinate descent): for parts whose rest direction differs
    // from Skyrim's bone (the clavicles: sternum vs neck base). The physics part then points a little off the bone;
    // the bone itself is still reproduced exactly. A small cost per degree keeps it from moving without reason.
    if std::env::var("CALIB_TILT").is_ok() {
        for i in 1..21usize {
            if kids(i).len() > 1 || skip(&rig[i]) {
                continue;
            }
            let start = rig[i].off;
            let reg = |q: Quat| 0.3 * (q * start.inverse()).normalize().angle_between(Quat::IDENTITY).to_degrees();
            let mut best_c = cost(&rig, i);
            for round in 0..3 {
                let step = [8.0f32, 4.0, 2.0][round];
                for ax in [glam::Vec3::X, glam::Vec3::Y, glam::Vec3::Z] {
                    loop {
                        let base = rig[i].off;
                        let mut moved = false;
                        for s in [-step, step] {
                            let q = (base * Quat::from_axis_angle(ax, s.to_radians())).normalize();
                            if reg(q) > 0.3 * 40.0 {
                                continue;
                            }
                            rig[i].off = q;
                            let c = cost(&rig, i) + reg(q);
                            if c < best_c + reg(base) - 0.5 {
                                best_c = c - reg(q);
                                moved = true;
                                break;
                            }
                            rig[i].off = base;
                        }
                        if !moved {
                            break;
                        }
                    }
                }
            }
            let turned = (rig[i].off * start.inverse()).normalize().angle_between(Quat::IDENTITY).to_degrees();
            if turned > 0.5 {
                println!("  tilt: {} {:.0} deg (cost {:.1})", name(&rig[i]), turned, best_c);
            }
        }
    }
    let (t1, per1) = total(&rig);
    println!("calibration: mean joint snap {t0:.1} -> {t1:.1} deg");
    for i in 1..21 {
        println!("  {:16} {:5.1} -> {:5.1}", name(&rig[i]), per0[i], per1[i]);
    }
    format_rig(&hdr, &rig)
}

/// The game side's part <-> bone table (text, one line per part, '|' separated since bone names have spaces):
/// `part|ref bone|write|joint bone A|joint bone B|offset qx qy qz qw|anchor x y z|in parent x y z`
/// ref bone rotation = part rotation * offset (offset measured in the bind pose, on the bind-pose fit, whose parts are
/// the same rigid frames as the scaled build's). Positions go down the chain so no joint opens: the root sits with
/// `anchor` (its hips' midpoint, = Skyrim's pelvis bone) on bone A; a child at parent pos + parent R * `in parent`
/// - its R * `anchor` (its joint, in its own frame and in its parent's). A/B: the bones a joint is checked against.
/// `write` = 1: the part drives its ref bone in game. Header `yaw` = the build's facing (radians, CCW from +X).
unsafe fn write_rig(path: &str, homer: *const NmBuild, body: *const NmBuild, joints: &[f32], j: &serde_json::Value, analyse: bool) -> String {
    use glam::{Mat3, Quat, Vec3};
    unsafe {
        let bind = nm_build_fit(homer, joints.as_ptr());
        assert!(!bind.is_null(), "bind fit: {}", err());
        let frame = |b: *const NmBuild, i: usize| {
            let (mut p, mut q) = ([0.0f32; 3], [0.0f32; 4]);
            nm_build_part_frame(b, i as i32, p.as_mut_ptr(), q.as_mut_ptr());
            (Vec3::from(p), Quat::from_array(q))
        };
        let joint = |b: *const NmBuild, i: usize| {
            let mut p = [0.0f32; 3];
            nm_build_part_joint(b, i as i32, p.as_mut_ptr());
            Vec3::from(p)
        };
        let bone_of = |name: &str| j["bones"][name].as_str().map(str::to_string);
        let rot = |bone: &str| {
            let r: Vec<f32> = j["rot"][bone].as_array().unwrap_or_else(|| panic!("no rotation for {bone}")).iter().map(|x| x.as_f64().unwrap() as f32).collect();
            Quat::from_mat3(&Mat3::from_cols_array(&r.try_into().unwrap()).transpose()).normalize()
        };
        let (spn2, neck) = ("NPC Spine2 [Spn2]", "NPC Neck [Neck]");
        // The body's parts turned onto Skyrim's bind pose, like nm_build_fit, except the chest: fit orients it by the
        // clavicles, which on Skyrim start at the sternum in front of the neck (tilts it ~60 deg). Here: neck direction
        // and shoulder line (upper arm joints). Single-child parts: the arc onto Skyrim's bone, from the parent's turn.
        let sky = |i: usize| Vec3::new(joints[i * 3], joints[i * 3 + 1], joints[i * 3 + 2]);
        let mut parent = [-1i32; 21];
        for i in 0..21 {
            nm_build_part_info(body, i as i32, std::ptr::null_mut(), std::ptr::null_mut(), std::ptr::null_mut(), &mut parent[i]);
        }
        let basis = |side: Vec3, up: Vec3| {
            let x = side.normalize();
            let z = (up - x * up.dot(x)).normalize();
            Quat::from_mat3(&Mat3::from_cols(x, z.cross(x), z)).normalize()
        };
        let mut turn = [Quat::IDENTITY; 21];
        for k in 0..21 {
            let t0 = if parent[k] < 0 { Quat::IDENTITY } else { turn[parent[k] as usize] };
            let kids: Vec<usize> = (0..21).filter(|&c| parent[c] == k as i32).collect();
            turn[k] = match (k, kids.len()) {
                (0, _) => basis(sky(1) - sky(4), sky(7) - sky(0)) * basis(joint(body, 1) - joint(body, 4), joint(body, 7) - joint(body, 0)).inverse(),
                (10, _) => basis(sky(14) - sky(18), sky(11) - sky(10)) * basis(joint(body, 14) - joint(body, 18), joint(body, 11) - joint(body, 10)).inverse(),
                (_, 0) => t0,
                (_, _) => {
                    let c = kids[0];
                    let ours = t0 * (joint(body, c) - joint(body, k));
                    Quat::from_rotation_arc(ours.normalize(), (sky(c) - sky(k)).normalize()) * t0
                }
            }
            .normalize();
        }
        let bind_rot = |i: usize| (turn[i] * frame(body, i).1).normalize();
        let mut out = String::new();
        let (l, r) = (joint(body, 1), joint(body, 4));
        let fwd = (l - r).cross(Vec3::Z);
        out += &format!("yaw {}\n", fwd.y.atan2(fwd.x));
        for i in 0..21 {
            let mut b = vec![0u8; 64];
            nm_part_name(i as i32, b.as_mut_ptr() as *mut _, 64);
            let name = String::from_utf8_lossy(&b).trim_end_matches('\0').to_string();
            // Skyrim's chest bone (Spine2) carries the clavicles and neck, like NM's Spine3: Spine3 drives it,
            // NM's Spine2 only reads it at the start
            let (rb, write, ja, jb) = match name.as_str() {
                "Spine2" => (spn2.to_string(), 0, spn2.to_string(), spn2.to_string()),
                "Spine3" => (spn2.to_string(), 1, spn2.to_string(), neck.to_string()),
                n => {
                    let b = bone_of(n).unwrap_or_else(|| panic!("no bone for {n}"));
                    (b.clone(), 1, b.clone(), b)
                }
            };
            let off = (bind_rot(i).inverse() * rot(&rb)).normalize();
            let (p, q) = frame(body, i);
            let mut par = -1;
            nm_build_part_info(body, i as i32, std::ptr::null_mut(), std::ptr::null_mut(), std::ptr::null_mut(), &mut par);
            let (anchor, inp) = if par < 0 {
                (q.inverse() * ((joint(body, 1) + joint(body, 4)) * 0.5 - p), Vec3::ZERO)
            } else {
                let (pp, pq) = frame(body, par as usize);
                (q.inverse() * (joint(body, i) - p), pq.inverse() * (joint(body, i) - pp))
            };
            out += &format!("{name}|{rb}|{write}|{ja}|{jb}|{} {} {} {}|{} {} {}|{} {} {}\n", off.x, off.y, off.z, off.w,
                anchor.x, anchor.y, anchor.z, inp.x, inp.y, inp.z);
        }
        // CALIB=pose1.txt;pose2.txt...: twist each single-child part about its own bone so Skyrim poses (these plus the
        // bind pose) sit inside the build's joint limits instead of snapping
        if let Ok(list) = std::env::var("CALIB") {
            let mut poses = vec![bind_pose(&out, j)];
            for p in list.split(';').filter(|s| !s.is_empty()) {
                poses.push(read_pose(p));
            }
            out = calibrate(body, &out, &poses);
        }
        std::fs::write(path, out.clone()).unwrap();
        println!("rig written to {path}");
        if !analyse {
            nm_build_destroy(bind);
            return out;
        }

        // What the plugin does: parts placed from the bones (here the bind pose) through the rig, then simulated.
        let bpos = |bone: &str| {
            let part = j["bones"].as_object().unwrap().iter().find(|(_, v)| v.as_str() == Some(bone)).unwrap().0;
            let v: Vec<f32> = j["joints"][part].as_array().unwrap().iter().map(|x| x.as_f64().unwrap() as f32).collect();
            Vec3::new(v[0], v[1], v[2])
        };
        // BIND_HEADING=h: the actor turned to Skyrim heading h (forward = (sin h, cos h)), agent yaw as the plugin does
        let h: f32 = std::env::var("BIND_HEADING").ok().map(|s| s.parse().unwrap()).unwrap_or(0.0);
        let turn = Quat::from_rotation_z(-h);
        let rig_yaw: f32 = out.lines().next().unwrap()[4..].parse().unwrap();
        let bpos = |bone: &str| turn * bpos(bone);
        let rot = |bone: &str| turn * rot(bone);
        let w = nm_world_create();
        nm_world_add_floor(w, 0.0, 0.0, 0.0, 50.0);
        let yaw = if std::env::var("BIND_HEADING").is_ok() { h.cos().atan2(h.sin()) - rig_yaw } else { 0.0 };
        let g = nm_agent_create(body, [0.0f32; 3].as_ptr(), yaw, 0.3);
        let mut placed: Vec<(Vec3, Quat)> = vec![];
        let (mut gap, mut worst) = (0.0f32, String::new());
        for (i, line) in out.lines().skip(1).enumerate() {
            let f: Vec<&str> = line.split('|').collect();
            let nums = |k: usize| f[k].split(' ').map(|x| x.parse::<f32>().unwrap()).collect::<Vec<f32>>();
            let r = (rot(f[1]) * Quat::from_slice(&nums(5)).inverse()).normalize();
            let (a, inp) = (Vec3::from_slice(&nums(6)), Vec3::from_slice(&nums(7)));
            let p = if i == 0 {
                bpos(f[3]) - r * a
            } else {
                let mut par = 0;
                nm_build_part_info(body, i as i32, std::ptr::null_mut(), std::ptr::null_mut(), std::ptr::null_mut(), &mut par);
                let (pp, pr) = placed[par as usize];
                pp + pr * inp - r * a
            };
            placed.push((p, r));
            nm_agent_set_part_state(g, i as i32, p.to_array().as_ptr(), r.to_array().as_ptr(), [0.0f32; 3].as_ptr(), [0.0f32; 3].as_ptr());
            // where NM's joint lands against Skyrim's bone (the root: its hips' midpoint against the pelvis bone)
            let d = (p + r * a).distance((bpos(f[3]) + bpos(f[4])) * 0.5);
            if std::env::var("GAPS").is_ok() {
                let q = p + r * a;
                let s = (bpos(f[3]) + bpos(f[4])) * 0.5;
                println!("  {:16} {d:.3} m   ours {:6.3} {:6.3} {:6.3}  skyrim {:6.3} {:6.3} {:6.3}  parent {}", f[0], q.x, q.y, q.z, s.x, s.y, s.z, parent[i]);
            }
            if d > gap {
                (gap, worst) = (d, f[0].to_string());
            }
        }
        println!("worst joint vs Skyrim bone: {worst}");
        if std::env::var("GAPS").is_ok() {
            for i in 0..21usize {
                let sky = Vec3::new(joints[i * 3], joints[i * 3 + 1], joints[i * 3 + 2]);
                let (bp, bq) = frame(bind, i);
                let (sp, sq) = frame(body, i);
                // the joint in each build's own part frame: same direction if the two share rigid frames
                let lb = bq.inverse() * (joint(bind, i) - bp);
                let ls = sq.inverse() * (joint(body, i) - sp);
                println!("  bind-fit joint {i:2} off Skyrim {:.3} m; joint dir bind vs scaled {:.1} deg", joint(bind, i).distance(sky),
                    if lb.length() > 1e-4 && ls.length() > 1e-4 { lb.angle_between(ls).to_degrees() } else { 0.0 });
            }
        }
        nm_msg_begin(g, c("bodyBalance").as_ptr());
        nm_msg_set_bool(g, c("start").as_ptr(), 1);
        nm_msg_send(g, w);
        let h0 = part_pos(g, 12)[2];
        let mut fin = true;
        // BIND_PUSH="x y" (N s): the plugin's F7, at the chest on the first step
        if let Ok(s) = std::env::var("BIND_PUSH") {
            let v: Vec<f32> = s.split(' ').map(|x| x.parse().unwrap()).collect();
            nm_agent_apply_push(g, part_pos(g, 10).as_ptr(), [v[0], v[1], 0.0f32].as_ptr());
        }
        let mut fell_at = None;
        for k in 0..180 {
            nm_agent_step(g, w, DT);
            fin &= part_pos(g, 12)[2].is_finite();
            if fell_at.is_none() && nm_agent_has_fallen(g) != 0 {
                fell_at = Some(k as f32 * DT);
            }
            if std::env::var("BIND_TRACE").is_ok() && k % 15 == 0 {
                println!("  t {:.2}: head {:.2} pelvis {:.2} balancing {}", k as f32 * DT, part_pos(g, 12)[2], part_pos(g, 0)[2], nm_agent_is_balancing(g));
            }
        }
        println!("fell at {fell_at:?}");
        println!("start from bind pose: worst joint gap {:.3} m, head {h0:.2} -> {:.2} m after 3 s, finite {fin}, balancing {}",
            gap, part_pos(g, 12)[2], nm_agent_is_balancing(g));
        nm_agent_destroy(g);

        // REPLAY=<SkyrimDelirium_pose.txt>: an in-game pose the plugin saved on F7, placed exactly as the plugin does.
        // Runs it with the build's own zero pose and with the game pose as the zero pose (nm_agent_set_zero_pose);
        // reports falls and how far the posture drifts (mean part angle vs start, relative to the pelvis).
        if let Ok(path) = std::env::var("REPLAY") {
            let txt = std::fs::read_to_string(&path).unwrap();
            let mut lines = txt.lines();
            let hdr: Vec<&str> = lines.next().unwrap().split(' ').collect();
            let heading: f32 = hdr[1].parse().unwrap();
            // the NPC's scale (pose header): body and rig offsets scaled to it, as the plugin does (NO_SCALE=1: off)
            let scale: f32 = if std::env::var("NO_SCALE").is_ok() { 1.0 } else { hdr.get(3).map_or(1.0, |x| x.parse().unwrap()) };
            let rig_lines: Vec<&str> = out.lines().skip(1).collect();
            let nums = |s: &str| s.split(' ').filter(|x| !x.is_empty()).map(|x| x.parse::<f32>().unwrap()).collect::<Vec<f32>>();
            let mut start: Vec<(Vec3, Quat)> = vec![];
            for (i, l) in lines.enumerate().take(21) {
                let f: Vec<&str> = l.split('|').collect();
                let m = nums(f[1]);
                let bone = Quat::from_mat3(&Mat3::from_cols_array(&m.try_into().unwrap()).transpose()).normalize();
                let (ja, jb) = (Vec3::from_slice(&nums(f[3])), Vec3::from_slice(&nums(f[4])));
                let rf: Vec<&str> = rig_lines[i].split('|').collect();
                let rn = |k: usize| nums(rf[k]);
                let r = (bone * Quat::from_slice(&rn(5)).inverse()).normalize();
                let (a, inp) = (Vec3::from_slice(&rn(6)) * scale, Vec3::from_slice(&rn(7)) * scale);
                let p = if i == 0 {
                    ja - r * a
                } else {
                    let (pp, pr) = start[parent[i] as usize];
                    pp + pr * inp - r * a
                };
                let _ = jb;
                start.push((p, r));
            }
            let rig_yaw: f32 = out.lines().next().unwrap()[4..].parse().unwrap();
            let push: Vec<f32> = std::env::var("BIND_PUSH").ok().map(|s| nums(&s)).unwrap_or(vec![0.0, 0.0]);
            for zero in [false, true] {
                let w = nm_world_create();
                nm_world_add_floor(w, 0.0, 0.0, 0.0, 50.0);
                // BODY_FILE=path: the body as the plugin gets it (nm_build_load), not the one built in memory here
                let src = match std::env::var("BODY_FILE") {
                    Ok(f) => {
                        let b = nm_build_load(c(&f).as_ptr());
                        assert!(!b.is_null(), "load {f}: {}", err());
                        b as *const NmBuild
                    }
                    Err(_) => body,
                };
                let src = if (scale - 1.0).abs() > 1e-3 {
                    let b = nm_build_scale(src, [scale; 21].as_ptr(), scale.powi(3));
                    assert!(!b.is_null(), "scale: {}", err());
                    b as *const NmBuild
                } else {
                    src
                };
                let g = nm_agent_create(src, [0.0f32; 3].as_ptr(), heading.cos().atan2(heading.sin()) - rig_yaw, 0.3);
                nm_agent_set_mode(g, NM_MODE_LEAD);
                let mut tf = vec![];
                for (i, (p, r)) in start.iter().enumerate() {
                    nm_agent_set_part_state(g, i as i32, p.to_array().as_ptr(), r.to_array().as_ptr(), [0.0f32; 3].as_ptr(), [0.0f32; 3].as_ptr());
                    tf.extend_from_slice(&p.to_array());
                    tf.extend_from_slice(&r.to_array());
                }
                if zero {
                    assert_eq!(nm_agent_set_zero_pose(g, tf.as_ptr(), 21), 0, "zero pose: {}", err());
                }
                if std::env::var("NO_BALANCE").is_err() {
                    nm_msg_begin(g, c("bodyBalance").as_ptr());
                    nm_msg_set_bool(g, c("start").as_ptr(), 1);
                    nm_msg_send(g, w);
                }
                if push[0] != 0.0 || push[1] != 0.0 {
                    nm_agent_apply_push(g, part_pos(g, 10).as_ptr(), [push[0], push[1], 0.0f32].as_ptr());
                }
                let drift = |g: *mut NmAgent| {
                    let mut q0 = [0.0f32; 4];
                    nm_agent_get_part_transform(g, 0, std::ptr::null_mut(), q0.as_mut_ptr());
                    let pel = Quat::from_slice(&q0);
                    let mut s = 0.0;
                    for i in 1..21 {
                        let mut q = [0.0f32; 4];
                        nm_agent_get_part_transform(g, i, std::ptr::null_mut(), q.as_mut_ptr());
                        let now = pel.inverse() * Quat::from_slice(&q);
                        let was = start[0].1.inverse() * start[i as usize].1;
                        s += now.angle_between(was).to_degrees();
                    }
                    s / 20.0
                };
                let mut fell = None;
                let mut worst = 0.0f32;
                for k in 0..std::env::var("STEPS").map_or(180, |s| s.parse::<usize>().unwrap()) {
                    nm_agent_step(g, w, DT);
                    if fell.is_none() && nm_agent_has_fallen(g) != 0 {
                        fell = Some(k as f32 * DT);
                    }
                    if k == 59 || k == 179 {
                        worst = worst.max(drift(g));
                    }
                    if std::env::var("BIND_TRACE").is_ok() && k % 30 == 0 {
                        println!("    t {:.1}: head {:.2} drift {:.0} deg", k as f32 * DT, part_pos(g, 12)[2], drift(g));
                    }
                }
                println!("replay {} zero pose: fell at {fell:?}, head {:.2} m, posture drift {:.0} deg (1 s) .. {:.0} deg (3 s, max {worst:.0})",
                    if zero { "GAME" } else { "build" }, part_pos(g, 12)[2], 0.0, drift(g));
                if std::env::var("DRIFT_PARTS").is_ok() {
                    // each part's turn against its parent, now vs the start pose
                    let mut s = String::new();
                    let get = |i: usize| {
                        let mut q = [0.0f32; 4];
                        nm_agent_get_part_transform(g, i as i32, std::ptr::null_mut(), q.as_mut_ptr());
                        Quat::from_slice(&q)
                    };
                    for i in 1..21usize {
                        let p = parent[i] as usize;
                        let now = get(p).inverse() * get(i);
                        let was = start[p].1.inverse() * start[i].1;
                        s += &format!(" {}={:.0}", rig_lines[i].split('|').next().unwrap(), now.angle_between(was).to_degrees());
                    }
                    println!("   per joint (deg):{s}");
                    // world turn of the pelvis, legs and lower spine: angle and axis (x right, y forward, z up)
                    let mut w = String::new();
                    for i in [0usize, 7, 1, 2, 3, 4, 5, 6] {
                        let d = (get(i) * start[i].1.inverse()).normalize();
                        let (ax, an) = d.to_axis_angle();
                        let an = if an > std::f32::consts::PI { an - std::f32::consts::TAU } else { an };
                        w += &format!(" {}={:.0}@({:.1},{:.1},{:.1})", rig_lines[i].split('|').next().unwrap(), an.to_degrees(), ax.x, ax.y, ax.z);
                    }
                    println!("   world turn:{w}");
                }
                nm_agent_destroy(g);
                nm_world_destroy(w);
            }
        }
        nm_build_destroy(bind);
        out
    }
}

fn main() {
    let a: Vec<String> = std::env::args().collect();
    if a.len() < 4 {
        eprintln!("usage: fit-test <Fred.art> <homer_build.txt> <joints.json>");
        std::process::exit(2);
    }
    let j: serde_json::Value = serde_json::from_str(&std::fs::read_to_string(&a[3]).unwrap()).unwrap();
    unsafe {
        assert!(nm_init(c(&a[1]).as_ptr()) >= 0, "init: {}", err());
        let homer = nm_build_load(c(&a[2]).as_ptr());
        assert!(!homer.is_null(), "build: {}", err());
        let mut joints = vec![];
        for i in 0..21 {
            let mut b = vec![0u8; 64];
            nm_part_name(i, b.as_mut_ptr() as *mut _, 64);
            let name = String::from_utf8_lossy(&b).trim_end_matches('\0').to_string();
            let v = j["joints"][&name].as_array().unwrap_or_else(|| panic!("no joint for {name}"));
            joints.extend(v.iter().map(|x| x.as_f64().unwrap() as f32));
        }
        // Skyrim spaces its 3 spine bones unlike Homer's 4 spine parts, and the fit cubes each length ratio into mass.
        // Put Spine0..3 on Skyrim's pelvis -> neck line at Homer's own spacing instead (LEAD captures the offsets anyway).
        if std::env::var("SPINE_HOMER").is_ok() {
            let hp = |i: i32| {
                let mut p = [0.0f32; 3];
                nm_build_part_frame(homer, i, p.as_mut_ptr(), std::ptr::null_mut());
                p
            };
            let (h0, h1) = (hp(0), hp(11));
            let hl: f32 = (0..3).map(|k| (h1[k] - h0[k]).powi(2)).sum::<f32>().sqrt();
            let s0: [f32; 3] = [joints[0], joints[1], joints[2]];
            let s1: [f32; 3] = [joints[33], joints[34], joints[35]];
            for i in 7..11 {
                let p = hp(i as i32);
                let t = (0..3).map(|k| (p[k] - h0[k]) * (h1[k] - h0[k])).sum::<f32>() / (hl * hl);
                for k in 0..3 {
                    joints[i * 3 + k] = s0[k] + t * (s1[k] - s0[k]);
                }
            }
        }
        if std::env::var("JOINTS").is_ok() {
            for i in 0..21 {
                let mut p = [0.0f32; 3];
                nm_build_part_frame(homer, i, p.as_mut_ptr(), std::ptr::null_mut());
                let k = i as usize * 3;
                eprintln!("{i:2} homer {:6.3} {:6.3} {:6.3}   skyrim {:6.3} {:6.3} {:6.3}", p[0], p[1], p[2], joints[k], joints[k + 1], joints[k + 2]);
            }
        }
        let fitted = if std::env::var("BIND_FIT").is_ok() {
            nm_build_fit(homer, joints.as_ptr())
        } else {
            // Homer's own standing pose, bones stretched to Skyrim's lengths, girth/mass by overall height
            let hj = |i: usize| {
                let mut p = [0.0f32; 3];
                nm_build_part_joint(homer, i as i32, p.as_mut_ptr());
                p
            };
            let sj = |i: usize| [joints[i * 3], joints[i * 3 + 1], joints[i * 3 + 2]];
            let d = |a: [f32; 3], b: [f32; 3]| ((a[0] - b[0]).powi(2) + (a[1] - b[1]).powi(2) + (a[2] - b[2]).powi(2)).sqrt();
            let mut parent = [0i32; 21];
            for i in 0..21 {
                nm_build_part_info(homer, i as i32, std::ptr::null_mut(), std::ptr::null_mut(), std::ptr::null_mut(), &mut parent[i]);
            }
            let trunk = d(sj(11), sj(0)) / d(hj(11), hj(0));
            let mut len = [1.0f32; 21];
            for q in 0..21 {
                let kids: Vec<usize> = (0..21).filter(|&c| parent[c] == q as i32).collect();
                if [0, 7, 8, 9, 10].contains(&q) {
                    len[q] = trunk;
                } else if !kids.is_empty() {
                    len[q] = kids.iter().map(|&c| d(sj(c), sj(q))).sum::<f32>() / kids.iter().map(|&c| d(hj(c), hj(q))).sum::<f32>();
                }
            }
            // both are ~1.8 m (Homer's stand is a bit crouched, so measuring it overstates Skyrim): his weight by default
            let m: f32 = std::env::var("MASS").ok().and_then(|s| s.parse().ok()).unwrap_or(1.0);
            let mut b = vec![0u8; 64];
            for i in 0..21 {
                nm_part_name(i as i32, b.as_mut_ptr() as *mut _, 64);
                eprintln!("len {:16} x{:.3}", String::from_utf8_lossy(&b).trim_end_matches('\0'), len[i]);
            }
            eprintln!("mass x{m:.3}");
            nm_build_scale(homer, len.as_ptr(), m)
        };
        assert!(!fitted.is_null(), "fit: {}", err());
        // Mass: Homer's per part, scaled by overall height only (the fit's per-bone cube overweights the spine).
        if std::env::var("BIND_FIT").is_ok() && std::env::var("RAW_MASS").is_err() {
            let stature = |b: *const NmBuild| {
                let z = |i: i32| {
                    let mut p = [0.0f32; 3];
                    nm_build_part_frame(b, i, p.as_mut_ptr(), std::ptr::null_mut());
                    p[2]
                };
                z(12) - z(3).min(z(6))
            };
            let s = stature(fitted) / stature(homer);
            println!("height scale {s:.3} -> mass x{:.3}", s.powi(3));
            for i in 0..21 {
                let mut m = 0.0;
                nm_build_part_info(homer, i, &mut m, std::ptr::null_mut(), std::ptr::null_mut(), std::ptr::null_mut());
                assert_eq!(nm_build_set_mass(fitted, i, m * s.powi(3)), 0, "set mass: {}", err());
            }
        }

        println!("part             homer kg   fitted kg  | half len m homer / fitted");
        let (mut mh, mut mf) = (0.0, 0.0);
        for i in 0..21 {
            let (mut m1, mut r1, mut h1, mut p1) = (0.0, 0.0, 0.0, 0);
            let (mut m2, mut r2, mut h2, mut p2) = (0.0, 0.0, 0.0, 0);
            nm_build_part_info(homer, i, &mut m1, &mut r1, &mut h1, &mut p1);
            nm_build_part_info(fitted, i, &mut m2, &mut r2, &mut h2, &mut p2);
            mh += m1;
            mf += m2;
            let mut b = vec![0u8; 64];
            nm_part_name(i, b.as_mut_ptr() as *mut _, 64);
            println!("{:16} {m1:8.2} {m2:10.2}  | {h1:.3} / {h2:.3}", String::from_utf8_lossy(&b).trim_end_matches('\0'));
        }
        println!("total mass       {mh:8.1} {mf:10.1}\n");

        let mut fitted = fitted;
        if let Ok(p) = std::env::var("RIG") {
            // REMAP=1: rebuild the body on Skyrim's skeleton. The first rig (with CALIB/CALIB_TILT) says how each part
            // turns onto its bone; the body's joints then go exactly onto Skyrim's and a second rig is written for it
            // (calibrated again only with REMAP_CALIB=poses, twist only).
            if std::env::var("REMAP").is_ok() {
                let rig = write_rig(&p, homer, fitted, &joints, &j, false);
                let (_, lines) = parse_rig(&rig);
                let pose = bind_pose(&rig, &j);
                // PELVIS_PITCH=deg: the pelvis part turned about Skyrim's side axis (x) against its bone
                let pitch: f32 = std::env::var("PELVIS_PITCH").ok().map_or(0.0, |s| s.parse().unwrap());
                let rots: Vec<f32> = (0..21).flat_map(|i| {
                    let r = (pose[i].0 * lines[i].off.inverse()).normalize();
                    let r = if i == 0 { (glam::Quat::from_rotation_x(pitch.to_radians()) * r).normalize() } else { r };
                    r.to_array()
                }).collect();
                // REMAP_ALIGN=Thigh,Shin: these parts also turn to point along their bone (default: none)
                let names = std::env::var("REMAP_ALIGN").unwrap_or_default();
                let mut align = 0u32;
                for (i, l) in lines.iter().enumerate() {
                    if names.split(',').filter(|w| !w.is_empty()).any(|w| l.head.split('|').next().unwrap().contains(w)) {
                        align |= 1 << i;
                    }
                }
                let re = nm_build_remap(fitted, joints.as_ptr(), rots.as_ptr(), align);
                assert!(!re.is_null(), "remap: {}", err());
                fitted = re;
                println!("remapped onto the skeleton");
                std::env::remove_var("CALIB_TILT");
                match std::env::var("REMAP_CALIB") {
                    Ok(c) => std::env::set_var("CALIB", c),
                    Err(_) => std::env::remove_var("CALIB"),
                }
            }
            write_rig(&p, homer, fitted, &joints, &j, true);
        }
        if let Ok(p) = std::env::var("SAVE") {
            assert_eq!(nm_build_save(fitted, c(&p).as_ptr()), 0, "save: {}", err());
            println!("saved fitted build to {p}");
            return;
        }
        let w = nm_world_create();
        nm_world_add_floor(w, 0.0, 0.0, 0.0, 50.0);
        let scenes = [("stand 6 s", Scene::Stand), ("shove 60 N s", Scene::Shove(60.0)), ("shove 150 N s", Scene::Shove(150.0)), ("shot (chest)", Scene::Shot)];
        println!("{:14} {:7} {:>9} {:>9} {:>8} {:>6} {:>8} {:>7} {:>6}", "scene", "body", "head min", "head end", "fell at", "bal", "max KE", "finite", "ms");
        for (name, s) in scenes {
            for (bn, b) in [("homer", homer), ("skyrim", fitted)] {
                let o = run(b, w, s, 6.0);
                println!("{name:14} {bn:7} {:9.2} {:9.2} {:>8} {:>6} {:8.0} {:>7} {:6.3}", o.head_min, o.head_end,
                    o.fell_at.map_or("-".into(), |t| format!("{t:.2}s")), if o.balancing_end { "yes" } else { "no" }, o.max_ke,
                    if o.finite { "yes" } else { "NO" }, o.ms);
            }
        }
        // smallest impulse that knocks each body down, per direction
        println!("\nfall threshold (N s, 4 s run)   backward  forward  side");
        for (bn, b) in [("homer", homer), ("skyrim", fitted)] {
            let mut row = format!("{bn:31}");
            for d in 0..3u8 {
                let t = (1..=15).map(|k| k as f32 * 20.0).find(|&n| run(b, w, Scene::Push(n, d), 4.0).fell_at.is_some());
                row += &format!(" {:>8}", t.map_or(">300".into(), |n| format!("{n:.0}")));
            }
            println!("{row}");
        }
    }
}
