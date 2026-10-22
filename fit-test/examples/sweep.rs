//! Many randomised trials of one character, one CSV line each, for comparing characters trial by trial (the same seed
//! gives the same trials): pushes and shots while standing, throws, behaviours while lying, and channelled spells
//! (the plugin's burn / shock / chill reactions held for the whole trial while she balances).
//! Env ONLY=<family> runs one family (push, shot, thrown, lying, channel).
//!
//! usage: cargo run --release --example sweep -- <brain (.art or .json)> <body (.json or build.txt)> <out.csv> [trials per family]

use delirium_physics::*;
use std::ffi::CString;
use std::io::Write;

const DT: f32 = 1.0 / 60.0;
const HEAD: i32 = 12;

fn c(s: &str) -> CString {
    CString::new(s).unwrap()
}

#[derive(Clone, Copy)]
enum V {
    F(f32),
    B(bool),
    S(&'static str),
    D([f32; 3]),
}
use V::{B, D, F, S};
type Msg = (&'static str, Vec<(&'static str, V)>);

struct Rng(u64);
impl Rng {
    fn f(&mut self) -> f32 {
        self.0 ^= self.0 << 13;
        self.0 ^= self.0 >> 7;
        self.0 ^= self.0 << 17;
        (self.0 >> 40) as f32 / (1u64 << 24) as f32
    }
    fn range(&mut self, a: f32, b: f32) -> f32 {
        a + (b - a) * self.f()
    }
    fn pick(&mut self, n: usize) -> usize {
        ((self.f() * n as f32) as usize).min(n - 1)
    }
}

unsafe fn send(g: *mut NmAgent, w: *mut NmWorld, m: &Msg) {
    unsafe {
        nm_msg_begin(g, c(m.0).as_ptr());
        for (k, v) in &m.1 {
            let k = c(k);
            match *v {
                F(x) => nm_msg_set_float(g, k.as_ptr(), x),
                B(x) => nm_msg_set_bool(g, k.as_ptr(), x as i32),
                S(x) => nm_msg_set_string(g, k.as_ptr(), c(x).as_ptr()),
                D(x) => nm_msg_set_vec3(g, k.as_ptr(), x.as_ptr()),
            };
        }
        nm_msg_send(g, w);
    }
}

unsafe fn pos(g: *mut NmAgent, i: i32) -> [f32; 3] {
    let mut p = [0.0f32; 3];
    unsafe { nm_agent_get_part_transform(g, i, p.as_mut_ptr(), std::ptr::null_mut()) };
    p
}

/// mean spin of every part against the pelvis, deg/s
unsafe fn activity(g: *mut NmAgent) -> f32 {
    unsafe {
        let n = nm_agent_part_count(g);
        let (mut v, mut p0) = ([0.0f32; 3], [0.0f32; 3]);
        nm_agent_get_part_velocity(g, 0, v.as_mut_ptr(), p0.as_mut_ptr());
        let mut s = 0.0;
        for i in 1..n {
            let mut w = [0.0f32; 3];
            nm_agent_get_part_velocity(g, i, v.as_mut_ptr(), w.as_mut_ptr());
            s += ((w[0] - p0[0]).powi(2) + (w[1] - p0[1]).powi(2) + (w[2] - p0[2]).powi(2)).sqrt();
        }
        s / (n - 1) as f32 * 57.3
    }
}

/// The agents of one trial. Default: one agent, brain and body at once (Delirium's own muscles). GAME=1: the game's
/// loop, as the plugin runs it: a FOLLOW brain synced from a passive body (NM_MODE_BODY) every frame, whose joint
/// accelerations, behaviour torques and part forces are then pushed onto the body the way engine.cpp ApplyTorques does
/// (held for HAVOK_SUB body steps per frame). Parts Skyrim has no body for (locked in the test body) ride on their parent.
struct Sim {
    g: *mut NmAgent,
    b: *mut NmAgent,
    map: [usize; 21],
    parent: [usize; 21],
    sub: usize,
}

const MAX_SPIN: f32 = 3.0 * std::f32::consts::TAU; // engine.cpp kMaxSpin
const MAX_EXT_TURN: f32 = 4.0; // engine.cpp kMaxExtTurn

impl Sim {
    /// the agent pushes land on and measurements read: the body
    fn m(&self) -> *mut NmAgent {
        if self.b.is_null() { self.g } else { self.b }
    }

    unsafe fn step(&self, w: *mut NmWorld) {
        unsafe {
            if self.b.is_null() {
                nm_agent_step(self.g, w, DT);
                return;
            }
            let (g, b) = (self.g, self.b);
            let st = |a: *mut NmAgent, i: usize| {
                let (mut p, mut q, mut v, mut av) = ([0.0f32; 3], [0.0f32; 4], [0.0f32; 3], [0.0f32; 3]);
                nm_agent_get_part_transform(a, i as i32, p.as_mut_ptr(), q.as_mut_ptr());
                nm_agent_get_part_velocity(a, i as i32, v.as_mut_ptr(), av.as_mut_ptr());
                (p, q, glam::Vec3::from_array(v), glam::Vec3::from_array(av))
            };
            for i in 0..21 {
                let (p, q, v, av) = st(b, i);
                nm_agent_set_part_state(g, i as i32, p.as_ptr(), q.as_ptr(), v.to_array().as_ptr(), av.to_array().as_ptr());
            }
            assert!(nm_agent_step(g, w, DT) >= 0);
            let (mut acc, mut ext, mut drv) = ([0.0f32; 63], [0.0f32; 63], [0.0f32; 126]);
            nm_agent_get_joint_accels(g, acc.as_mut_ptr(), 21);
            nm_agent_get_part_ext_torques(g, ext.as_mut_ptr(), 21);
            nm_agent_get_part_drives(g, drv.as_mut_ptr(), 21);
            let v3 = |a: &[f32], k: usize| glam::Vec3::new(a[k], a[k + 1], a[k + 2]);
            let h = DT / self.sub as f32;
            let gain: f32 = std::env::var("GAIN").ok().and_then(|s| s.parse().ok()).unwrap_or(1.0);
            let vel = std::env::var("DRIVE").is_ok_and(|v| v == "vel");
            for sub_i in 0..self.sub {
                let first = sub_i == 0;
                let mut inv = [glam::Mat3::ZERO; 21];
                let mut im = [0.0f32; 21];
                for k in 0..21 {
                    let mut m = [0.0f32; 9];
                    nm_agent_get_part_inv_inertia(b, k as i32, m.as_mut_ptr(), &mut im[k]);
                    inv[k] = glam::Mat3::from_cols_array(&m).transpose();
                }
                let (mut dw, mut dv) = ([glam::Vec3::ZERO; 21], [glam::Vec3::ZERO; 21]);
                for p in 1..21 {
                    let (c, pb) = (self.map[p], self.map[self.parent[p]]);
                    let a = v3(&acc, p * 3);
                    if c == pb || !a.is_finite() {
                        continue;
                    }
                    let damp: f32 = std::env::var("DAMP").ok().and_then(|s| s.parse().ok()).unwrap_or(0.0);
                    // DRIVE=vel: the joint's spin goes to the brain's own after its step (its muscles and joint stiffening, which
                    // the accelerations leave out); default: the plugin's accelerations
                    let rel_b = st(b, c).3 - st(b, pb).3;
                    let want = if vel && first { (st(g, p).3 - st(g, self.parent[p]).3 - rel_b) * gain } else if vel { glam::Vec3::ZERO } else { a * h * gain - rel_b * damp };
                    let want = want.clamp_length_max(30.0);
                    let s = inv[c] + inv[pb];
                    if s.determinant().abs() <= 1e-12 {
                        continue;
                    }
                    let t = s.inverse() * want;
                    let (wc, wp) = (inv[c] * t, -(inv[pb] * t));
                    dw[c] += wc;
                    dw[pb] += wp;
                    let mut piv = [0.0f32; 3];
                    nm_agent_get_joint(b, p as i32, std::ptr::null_mut(), piv.as_mut_ptr(), std::ptr::null_mut(), std::ptr::null_mut(), std::ptr::null_mut());
                    let piv = glam::Vec3::from_array(piv);
                    let com = |k: usize| glam::Vec3::from_array(st(b, k).0);
                    dv[c] += wc.cross(com(c) - piv);
                    dv[pb] += wp.cross(com(pb) - piv);
                }
                for p in 0..21 {
                    let (k, e, f) = (self.map[p], v3(&ext, p * 3), v3(&drv, p * 6));
                    if e.is_finite() {
                        dw[k] += (inv[k] * e * h * gain).clamp_length_max(MAX_EXT_TURN);
                    }
                    if f.is_finite() {
                        dv[k] += f * im[k] * h * gain;
                    }
                }
                for k in 0..21 {
                    if self.map[k] != k {
                        continue;
                    }
                    let (p, q, v, av) = st(b, k);
                    let nl = v + dv[k].clamp_length_max(3.0);
                    let mut na = av + dw[k];
                    let (was, now) = (av.length(), na.length());
                    if now > MAX_SPIN && now > was {
                        na *= was.max(MAX_SPIN) / now;
                    }
                    if nl.is_finite() && na.is_finite() {
                        nm_agent_set_part_state(b, k as i32, p.as_ptr(), q.as_ptr(), nl.to_array().as_ptr(), na.to_array().as_ptr());
                    }
                }
                nm_agent_step(b, w, h);
            }
        }
    }

    unsafe fn destroy(&self) {
        unsafe {
            nm_agent_destroy(self.g);
            if !self.b.is_null() {
                nm_agent_destroy(self.b);
            }
        }
    }
}

/// STANCE=<SkyrimDelirium_pose.txt> RIG=<rig.txt>: an NPC's pose from the game (the plugin saves it when Delirium
/// takes her), placed the way fit-test's REPLAY does at scale 1. Returns the agent's yaw and every part's world
/// transform (7 floats each), for its start and its zero pose, as the plugin sets both.
fn stance(pose: &str, rig: &str, parent: &[Option<usize>]) -> (f32, Vec<f32>) {
    use glam::{Mat3, Quat, Vec3};
    let nums = |s: &str| s.split(' ').filter(|x| !x.is_empty()).map(|x| x.parse::<f32>().unwrap()).collect::<Vec<f32>>();
    let txt = std::fs::read_to_string(pose).unwrap();
    let rig = std::fs::read_to_string(rig).unwrap();
    let mut lines = txt.lines();
    let heading: f32 = lines.next().unwrap().split(' ').nth(1).unwrap().parse().unwrap();
    let rig_yaw: f32 = rig.lines().next().unwrap()[4..].trim().parse().unwrap();
    let rig_lines: Vec<&str> = rig.lines().skip(1).collect();
    let mut start: Vec<(Vec3, Quat)> = vec![];
    for (i, l) in lines.enumerate().take(21) {
        let f: Vec<&str> = l.split('|').collect();
        let bone = Quat::from_mat3(&Mat3::from_cols_array(&nums(f[1]).try_into().unwrap()).transpose()).normalize();
        let rf: Vec<&str> = rig_lines[i].split('|').collect();
        let r = (bone * Quat::from_slice(&nums(rf[5])).inverse()).normalize();
        let (a, inp) = (Vec3::from_slice(&nums(rf[6])), Vec3::from_slice(&nums(rf[7])));
        let p = match parent[i] {
            None => Vec3::from_slice(&nums(f[3])) - r * a,
            Some(j) => start[j].0 + start[j].1 * inp - r * a,
        };
        start.push((p, r));
    }
    let tf = start.iter().flat_map(|(p, r)| p.to_array().into_iter().chain(r.to_array())).collect();
    (heading.cos().atan2(heading.sin()) - rig_yaw, tf)
}

/// STANCE_TRACK=<SkyrimDelirium_track_<FormID>.txt> RIG=<rig.txt>: the held poses the plugin recorded each frame while
/// she stood (bRecordPoses: her live animation, staggers and hit reactions included), from Delirium taking her. Returns
/// the agent's yaw and each frame's (time from the first, every part's transform), moved so the first frame's pelvis
/// stands on the origin and scaled to a scale-1 skeleton.
fn track(file: &str, rig: &str, body: &str) -> (f32, Vec<(f32, Vec<f32>)>) {
    let txt = std::fs::read_to_string(file).unwrap();
    let mut lines = txt.lines();
    let head: Vec<f32> = lines.next().unwrap().split(' ').filter_map(|x| x.parse().ok()).collect();
    let (heading, scale) = (head[0], head[1]);
    let rig_yaw: f32 = std::fs::read_to_string(rig).unwrap().lines().next().unwrap()[4..].trim().parse().unwrap();
    let mut frames: Vec<(f32, Vec<f32>)> = lines
        .map(|l| l.split(' ').filter(|x| !x.is_empty()).map(|x| x.parse::<f32>().unwrap()).collect::<Vec<f32>>())
        .filter(|v| v.len() == 1 + 21 * 7)
        .map(|v| (v[0], v[1..].to_vec()))
        .collect();
    assert!(!frames.is_empty(), "no frames in {file}");
    let (t0, x0, y0) = (frames[0].0, frames[0].1[0], frames[0].1[1]);
    for (t, tf) in &mut frames {
        *t -= t0;
        for i in 0..21 {
            tf[i * 7] = (tf[i * 7] - x0) / scale;
            tf[i * 7 + 1] = (tf[i * 7 + 1] - y0) / scale;
            tf[i * 7 + 2] /= scale;
        }
    }
    // TRACK_PARTS=arms|upper|spine|all: only those parts follow the track (their joint's own angle, in their parent's
    // frame); the rest hold the first frame's
    let follow: Vec<usize> = match std::env::var("TRACK_PARTS").as_deref() {
        Ok("arms") => (13..21).collect(),
        Ok("upper") => (11..21).collect(),
        Ok("spine") => (7..21).collect(),
        Ok("none") => vec![],
        _ => (0..21).collect(),
    };
    // LEGS=rest: the hips, knees and ankles hold the body's own standing build (its rest frames), whatever the track does
    let legs_rest = std::env::var("LEGS").is_ok_and(|v| v == "rest");
    let rest: Vec<f32> = {
        let j: serde_json::Value = serde_json::from_str(&std::fs::read_to_string(body).unwrap()).unwrap();
        (0..21).flat_map(|k| {
            let f = &j["build"][k]["frame"];
            let g = |n: &str, i: usize| f[n][i].as_f64().unwrap() as f32;
            [g("pos", 0), g("pos", 1), g("pos", 2), g("rot", 0), g("rot", 1), g("rot", 2), g("rot", 3)]
        }).collect()
    };
    if follow.len() < 21 || legs_rest {
        use glam::{Quat, Vec3};
        const PARENT: [usize; 21] = [0, 0, 1, 2, 0, 4, 5, 0, 7, 8, 9, 10, 11, 10, 13, 14, 15, 10, 17, 18, 19];
        let get = |tf: &[f32], i: usize| (Vec3::from_slice(&tf[i * 7..]), Quat::from_slice(&tf[i * 7 + 3..]).normalize());
        let first = frames[0].1.clone();
        for (_, tf) in frames.iter_mut() {
            let src = tf.clone();
            for i in 0..21 {
                let from = if legs_rest && (1..=6).contains(&i) { &rest } else if follow.contains(&i) { &src } else { &first };
                let (p, r) = get(from, i);
                let (np, nr) = if i == 0 {
                    (p, r)
                } else {
                    let (pp, pr) = get(from, PARENT[i]);
                    let (qp, qr) = get(tf, PARENT[i]);  // the parent as already rebuilt
                    (qp + qr * (pr.inverse() * (p - pp)), qr * (pr.inverse() * r))
                };
                tf[i * 7..i * 7 + 3].copy_from_slice(&np.to_array());
                tf[i * 7 + 3..i * 7 + 7].copy_from_slice(&nr.to_array());
            }
        }
    }
    if legs_rest {
        // straight rest legs under the recorded pelvis: lift or drop the whole pose so the lower foot stands where it rests
        for (_, tf) in &mut frames {
            let dz = rest[3 * 7 + 2].min(rest[6 * 7 + 2]) - tf[3 * 7 + 2].min(tf[6 * 7 + 2]);
            for i in 0..21 {
                tf[i * 7 + 2] += dz;
            }
        }
    }
    // FLAT_FEET=1: each foot's sole levelled (a foot's local y is up when flat), its heading kept
    if std::env::var("FLAT_FEET").is_ok() {
        use glam::{Quat, Vec3};
        for (_, tf) in &mut frames {
            for i in [3usize, 6] {
                let r = Quat::from_slice(&tf[i * 7 + 3..]).normalize();
                let flat = (Quat::from_rotation_arc((r * Vec3::Y).normalize(), Vec3::Z) * r).normalize();
                tf[i * 7 + 3..i * 7 + 7].copy_from_slice(&flat.to_array());
            }
        }
    }
    eprintln!("STANCE_TRACK: {} frames over {:.2} s, scale {scale}, {} parts follow", frames.len(), frames.last().unwrap().0, follow.len());
    (heading.cos().atan2(heading.sin()) - rig_yaw, frames)
}

// the plugin's channel reactions (engine.cpp StartBurn / StartShock / StartChill), standing
fn channel(r: &mut Rng, away: [f32; 3]) -> (&'static str, Vec<Msg>) {
    match r.pick(3) {
        0 => ("burn", vec![
            ("bodyWrithe", vec![("start", B(true)), ("mask", S("ub")), ("armStiffness", F(12.0)), ("armDamping", F(0.5)),
                ("armPeriod", F(r.range(0.4, 0.6))), ("armAmplitude", F(r.range(1.5, 2.0))), ("elbowAmplitude", F(1.5)),
                ("backStiffness", F(11.0)), ("backDamping", F(0.6)), ("backPeriod", F(r.range(0.8, 1.2))), ("backAmplitude", F(r.range(0.8, 1.2)))]),
            ("leanInDirection", vec![("start", B(true)), ("applyAsForce", B(false)), ("dir", D(away)), ("leanAmount", F(r.range(0.2, 0.35)))]),
        ]),
        1 => ("shock", vec![("electrocute", vec![("start", B(true)), ("stunMag", F(r.range(0.45, 0.55))), ("stunInterval", F(0.07)),
            ("largeMult", F(3.0)), ("largeMinTime", F(0.2)), ("largeMaxTime", F(0.45)), ("directionRandomness", F(0.6))])]),
        _ => ("chill", vec![
            ("bodyFoetal", vec![("start", B(true)), ("mask", S("ub")), ("stiffness", F(7.0)), ("dampingFactor", F(1.6)), ("asymmetry", F(0.3))]),
            ("electrocute", vec![("start", B(true)), ("leftLeg", B(false)), ("rightLeg", B(false)), ("stunMag", F(0.06)), ("stunInterval", F(0.05)),
                ("largeMult", F(1.0)), ("directionRandomness", F(0.3))]),
            ("configureBalance", vec![("stepClampScale", F(0.5)), ("stepHeight", F(0.05))]),
        ]),
    }
}

fn stacks(r: &mut Rng, family: usize) -> (&'static str, Vec<Msg>) {
    let el = |m: f32| -> Msg {
        ("electrocute", vec![("start", B(true)), ("stunMag", F(m)), ("stunInterval", F(0.07)), ("largeMult", F(2.5)),
            ("largeMinTime", F(0.25)), ("largeMaxTime", F(0.5)), ("directionRandomness", F(0.6)), ("useTorques", B(false))])
    };
    let writhe = |mask: &'static str, a: f32, p: f32, roll: bool| -> Msg {
        ("bodyWrithe", vec![("start", B(true)), ("mask", S(mask)), ("armAmplitude", F(a)), ("legAmplitude", F(a)),
            ("backAmplitude", F(a)), ("armPeriod", F(p)), ("legPeriod", F(p)), ("backPeriod", F(p)), ("rollOverFlag", B(roll))])
    };
    let on = |n: &'static str| -> Msg { (n, vec![("start", B(true))]) };
    match family {
        // standing shots
        1 => match r.pick(5) {
            0 => ("none", vec![]),
            1 => ("writhe_ub", vec![writhe("ub", 1.8, 0.4, false)]),
            2 => ("electrocute", vec![el(r.range(0.1, 0.3))]),
            3 => ("foetal_ub", vec![("bodyFoetal", vec![("start", B(true)), ("mask", S("ub"))])]),
            _ => ("writhe_ub+el", vec![writhe("ub", 1.8, 0.4, false), el(0.15)]),
        },
        // thrown
        2 => match r.pick(7) {
            0 => ("relax", vec![on("bodyRelax")]),
            1 => ("highFall", vec![on("highFall")]),
            2 => ("windmill", vec![("armsWindmillAdaptive", vec![("start", B(true)), ("disableOnImpact", B(true))])]),
            3 => ("foetal", vec![on("bodyFoetal")]),
            4 => ("rollUp", vec![("bodyRollUp", vec![("start", B(true)), ("mask", S("fb"))])]),
            5 => ("pedal", vec![on("pedalLegs")]),
            _ => ("catchFall", vec![on("catchFall")]),
        },
        // lying
        _ => match r.pick(7) {
            0 => ("relax", vec![on("bodyRelax")]),
            1 => ("writhe_fire", vec![writhe("fb", 1.6, 0.45, true)]),
            2 => ("writhe_shock+el", vec![writhe("fb", 1.3, 0.3, false), el(0.2)]),
            3 => ("electrocute", vec![el(r.range(0.1, 0.3))]),
            4 => ("foetal", vec![on("bodyFoetal")]),
            5 => ("rollUp", vec![("bodyRollUp", vec![("start", B(true)), ("mask", S("fb"))])]),
            _ => ("writhe_hard", vec![writhe("fb", 3.0, 0.35, true)]),
        },
    }
}

fn main() {
    let a: Vec<String> = std::env::args().collect();
    let per: usize = a.get(4).and_then(|s| s.parse().ok()).unwrap_or(100);
    let mut out = std::io::BufWriter::new(std::fs::File::create(&a[3]).unwrap());
    writeln!(out, "family,trial,stack,part,mag,angle,fell,t_fall,head_min,moved,act,act_late,ke_max,settle,finite").unwrap();
    unsafe {
        let ok = if a[1].ends_with(".json") { delirium_physics::character::nm_init_character_file(c(&a[1]).as_ptr()) } else { nm_init(c(&a[1]).as_ptr()) };
        assert!(ok >= 0);
        let build = if a[2].ends_with(".json") {
            delirium_physics::character::nm_build_from_character(c(&std::fs::read_to_string(&a[2]).unwrap()).as_ptr())
        } else {
            nm_build_load(c(&a[2]).as_ptr())
        };
        assert!(!build.is_null());
        let game = std::env::var("GAME").is_ok_and(|v| v == "1");
        let sub: usize = std::env::var("HAVOK_SUB").ok().and_then(|s| s.parse().ok()).unwrap_or(1);
        let brain_build = if game {
            assert!(a[1].ends_with(".json") && a[2].ends_with(".json"), "GAME=1 needs a .json brain and body");
            let bb = delirium_physics::character::nm_build_from_character(c(&std::fs::read_to_string(&a[1]).unwrap()).as_ptr());
            assert!(!bb.is_null());
            bb
        } else {
            std::ptr::null_mut()
        };
        // parts Skyrim has no body for: locked in the test body (zero range, child frame identity); ride on the parent's body
        let (mut map, mut parent) = ([0usize; 21], [0usize; 21]);
        if game {
            let body: serde_json::Value = serde_json::from_str(&std::fs::read_to_string(&a[2]).unwrap()).unwrap();
            for k in 0..21 {
                let p = &body["build"][k];
                parent[k] = p["parent"].as_u64().unwrap_or(0) as usize;
                let l = &p["body_limit"]["bone"];
                let locked = l.is_object() && l["lean"] == serde_json::json!([0.0, 0.0]) && l["twist"].as_f64() == Some(0.0);
                map[k] = if locked { map[parent[k]] } else { k };
            }
            eprintln!("GAME=1, {sub} body steps per frame; parts on their parent's body: {:?}", (0..21).filter(|&k| map[k] != k).collect::<Vec<_>>());
        }
        let pose = std::env::var("STANCE").ok().map(|p| {
            let body: serde_json::Value = serde_json::from_str(&std::fs::read_to_string(&a[2]).expect("STANCE needs a .json body")).unwrap();
            let parent: Vec<Option<usize>> = (0..21).map(|k| body["build"][k]["parent"].as_u64().map(|p| p as usize)).collect();
            stance(&p, &std::env::var("RIG").expect("STANCE needs RIG=<rig.txt>"), &parent)
        });
        // STANCE_TRACK: starts from the first recorded frame, then the zero pose follows the track from the hit on
        // (standing families only: in game she stops following her animation once knocked down or thrown)
        let frames = std::env::var("STANCE_TRACK").ok().map(|p| track(&p, &std::env::var("RIG").expect("STANCE_TRACK needs RIG=<rig.txt>"), &a[2]));
        let pose = match &frames {
            Some((yaw, f)) => Some((*yaw, f[0].1.clone())),
            None => pose,
        };
        assert!(frames.is_none() || !game, "STANCE_TRACK is for the standing (LEAD) sweep, not GAME=1");
        if let (Ok(out), Some((yaw, tf))) = (std::env::var("PRINT_POSE"), &pose) {
            std::fs::write(out, format!("{yaw}
{}
", tf.iter().map(|x| x.to_string()).collect::<Vec<_>>().join(" "))).unwrap();
        }
        let w = nm_world_create();
        nm_world_add_floor(w, 0.0, 0.0, 0.0, 50.0);
        let names = ["push", "shot", "thrown", "lying", "channel"];
        let only = std::env::var("ONLY").ok();
        for family in 0..5 {
            if only.as_deref().is_some_and(|o| o != names[family]) {
                continue;
            }
            for t in 0..per {
                // the same trial for every character: seeded by family and trial only
                let mut r = Rng(0x9E37_79B9_7F4A_7C15 ^ ((family as u64) << 32 | t as u64).wrapping_mul(0x2545_F491_4F6C_DD1D));
                for _ in 0..4 {
                    r.f();
                }
                let seed = r.f();
                let sim = if game {
                    let g = nm_agent_create(brain_build, [0.0f32; 3].as_ptr(), 0.0, seed);
                    let b = nm_agent_create(build, [0.0f32; 3].as_ptr(), 0.0, seed);
                    nm_agent_set_mode(g, NM_MODE_FOLLOW);
                    nm_agent_set_mode(b, NM_MODE_BODY);
                    Sim { g, b, map, parent, sub }
                } else {
                    let g = nm_agent_create(build, [0.0f32; 3].as_ptr(), pose.as_ref().map_or(0.0, |p| p.0), seed);
                    nm_agent_set_mode(g, NM_MODE_LEAD);
                    if let Some((_, tf)) = &pose {
                        for i in 0..21 {
                            let s = &tf[i * 7..];
                            nm_agent_set_part_state(g, i as i32, s.as_ptr(), s[3..].as_ptr(), [0.0f32; 3].as_ptr(), [0.0f32; 3].as_ptr());
                        }
                        assert_eq!(nm_agent_set_zero_pose(g, tf.as_ptr(), 21), 0);
                    }
                    Sim { g, b: std::ptr::null_mut(), map, parent, sub }
                };
                let (g, m) = (sim.g, sim.m());
                send(g, w, &("bodyBalance", vec![("start", B(true))]));
                for _ in 0..30 {
                    sim.step(w);
                }
                let angle = r.range(0.0, std::f32::consts::TAU);
                let dir = [angle.cos(), angle.sin(), 0.0];
                let (mut part, mut mag) = (10, 0.0);
                if family == 3 {
                    send(g, w, &("stopAllBehaviours", vec![]));
                    send(g, w, &("bodyRelax", vec![("start", B(true))]));
                    let p = pos(m, 10);
                    nm_agent_apply_push(m, p.as_ptr(), [dir[0] * 150.0, dir[1] * 150.0, 0.0].as_ptr());
                    for _ in 0..120 {
                        sim.step(w);
                    }
                    send(g, w, &("stopAllBehaviours", vec![]));
                }
                let (stack, msgs) = match family {
                    0 => ("balance", vec![]),
                    4 => channel(&mut r, [-dir[0], -dir[1], 0.0]),
                    _ => stacks(&mut r, family),
                };
                match family {
                    0 => {
                        part = [0, 7, 10, 12][r.pick(4)];
                        mag = r.range(30.0, 280.0);
                    if std::env::var("NOHIT").is_ok() {
                        mag = 0.0;  // control: the stance (or STANCE_TRACK) alone
                    }
                        let p = pos(m, part);
                        nm_agent_apply_push(m, p.as_ptr(), [dir[0] * mag, dir[1] * mag, 0.0].as_ptr());
                    }
                    1 => {
                        part = [7, 8, 10, 12, 14, 1][r.pick(6)];
                        mag = r.range(40.0, 220.0);
                        let p = pos(m, part);
                        let d = [-dir[0], -dir[1], 0.0];
                        nm_agent_task_shot(g, w, std::ptr::null(), part, p.as_ptr(), d.as_ptr(), 198.0);
                        nm_agent_apply_push(m, p.as_ptr(), [dir[0] * mag, dir[1] * mag, 0.0].as_ptr());
                    }
                    2 => {
                        send(g, w, &("stopAllBehaviours", vec![]));
                        mag = r.range(2.0, 9.0);
                        let up = r.range(1.0, 6.0);
                        nm_agent_add_velocity(m, [dir[0] * mag, dir[1] * mag, up].as_ptr());
                    }
                    _ => {}
                }
                for m in &msgs {
                    send(g, w, m);
                }
                let start = pos(m, 0);
                let steps = 240;
                let (mut act, mut late, mut ke_max, mut head_min) = (0.0f32, 0.0f32, 0.0f32, f32::MAX);
                let (mut fell, mut settle, mut finite) = (None, None, true);
                let mut fi = 0;
                for k in 0..steps {
                    if let Some((_, f)) = frames.as_ref().filter(|_| family <= 1 || family == 4) {
                        // the latest recorded frame at this time; past the end (she was knocked down) the last one
                        while fi + 1 < f.len() && f[fi + 1].0 <= k as f32 * DT {
                            fi += 1;
                        }
                        if std::env::var("TRACK_ONCE").is_err() {
                            assert_eq!(nm_agent_set_zero_pose(g, f[fi].1.as_ptr(), 21), 0);
                        }
                    } else if let Some((_, tf)) = pose.as_ref().filter(|_| std::env::var("REZERO").is_ok()) {
                        // control: the same stance sent every step, as the plugin does
                        assert_eq!(nm_agent_set_zero_pose(g, tf.as_ptr(), 21), 0);
                    }
                    sim.step(w);
                    let s = activity(m);
                    act += s;
                    if k >= steps - 60 {
                        late += s;
                    }
                    let ke = nm_agent_get_kinetic_energy(m);
                    let h = pos(m, HEAD)[2];
                    if !ke.is_finite() || !h.is_finite() {
                        finite = false;
                        break;
                    }
                    ke_max = ke_max.max(ke);
                    head_min = head_min.min(h);
                    if (family <= 1 || family == 4) && fell.is_none() && h < 1.0 {
                        fell = Some(k as f32 * DT);
                    }
                    if settle.is_none() && k > 30 && ke < 0.5 {
                        settle = Some(k as f32 * DT);
                    }
                }
                if std::env::var("PARTS").is_ok() && family == 3 && stack == "relax" {
                    let mut line = String::new();
                    for i in 0..21 {
                        let (mut v, mut wv) = ([0.0f32; 3], [0.0f32; 3]);
                        nm_agent_get_part_velocity(m, i, v.as_mut_ptr(), wv.as_mut_ptr());
                        line += &format!(" {:.0}", (wv[0] * wv[0] + wv[1] * wv[1] + wv[2] * wv[2]).sqrt() * 57.3);
                    }
                    eprintln!("relax spin per part:{line}");
                }
                let end = pos(m, 0);
                let moved = ((end[0] - start[0]).powi(2) + (end[1] - start[1]).powi(2)).sqrt();
                writeln!(out, "{},{t},{stack},{part},{mag:.1},{:.0},{},{},{head_min:.3},{moved:.3},{:.1},{:.1},{ke_max:.1},{},{}",
                    names[family], angle.to_degrees(), fell.is_some() as i32, fell.map_or(String::new(), |x| format!("{x:.2}")),
                    act / steps as f32, late / 60.0, settle.map_or(String::new(), |x| format!("{x:.2}")), finite as i32).unwrap();
                sim.destroy();
            }
        }
    }
}
