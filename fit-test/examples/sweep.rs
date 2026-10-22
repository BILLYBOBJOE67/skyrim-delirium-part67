//! Many randomised trials of one character, one CSV line each, for comparing characters trial by trial (the same seed
//! gives the same trials): pushes and shots while standing, throws, and behaviours while lying.
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
}
use V::{B, F, S};
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
        let w = nm_world_create();
        nm_world_add_floor(w, 0.0, 0.0, 0.0, 50.0);
        let names = ["push", "shot", "thrown", "lying"];
        for family in 0..4 {
            for t in 0..per {
                // the same trial for every character: seeded by family and trial only
                let mut r = Rng(0x9E37_79B9_7F4A_7C15 ^ ((family as u64) << 32 | t as u64).wrapping_mul(0x2545_F491_4F6C_DD1D));
                for _ in 0..4 {
                    r.f();
                }
                let g = nm_agent_create(build, [0.0f32; 3].as_ptr(), 0.0, r.f());
                nm_agent_set_mode(g, NM_MODE_LEAD);
                send(g, w, &("bodyBalance", vec![("start", B(true))]));
                for _ in 0..30 {
                    nm_agent_step(g, w, DT);
                }
                let angle = r.range(0.0, std::f32::consts::TAU);
                let dir = [angle.cos(), angle.sin(), 0.0];
                let (mut part, mut mag) = (10, 0.0);
                if family == 3 {
                    send(g, w, &("stopAllBehaviours", vec![]));
                    send(g, w, &("bodyRelax", vec![("start", B(true))]));
                    let p = pos(g, 10);
                    nm_agent_apply_push(g, p.as_ptr(), [dir[0] * 150.0, dir[1] * 150.0, 0.0].as_ptr());
                    for _ in 0..120 {
                        nm_agent_step(g, w, DT);
                    }
                    send(g, w, &("stopAllBehaviours", vec![]));
                }
                let (stack, msgs) = if family == 0 { ("balance", vec![]) } else { stacks(&mut r, family) };
                match family {
                    0 => {
                        part = [0, 7, 10, 12][r.pick(4)];
                        mag = r.range(30.0, 280.0);
                        let p = pos(g, part);
                        nm_agent_apply_push(g, p.as_ptr(), [dir[0] * mag, dir[1] * mag, 0.0].as_ptr());
                    }
                    1 => {
                        part = [7, 8, 10, 12, 14, 1][r.pick(6)];
                        mag = r.range(40.0, 220.0);
                        let p = pos(g, part);
                        let d = [-dir[0], -dir[1], 0.0];
                        nm_agent_task_shot(g, w, std::ptr::null(), part, p.as_ptr(), d.as_ptr(), 198.0);
                        nm_agent_apply_push(g, p.as_ptr(), [dir[0] * mag, dir[1] * mag, 0.0].as_ptr());
                    }
                    2 => {
                        send(g, w, &("stopAllBehaviours", vec![]));
                        mag = r.range(2.0, 9.0);
                        let up = r.range(1.0, 6.0);
                        nm_agent_add_velocity(g, [dir[0] * mag, dir[1] * mag, up].as_ptr());
                    }
                    _ => {}
                }
                for m in &msgs {
                    send(g, w, m);
                }
                let start = pos(g, 0);
                let steps = 240;
                let (mut act, mut late, mut ke_max, mut head_min) = (0.0f32, 0.0f32, 0.0f32, f32::MAX);
                let (mut fell, mut settle, mut finite) = (None, None, true);
                for k in 0..steps {
                    nm_agent_step(g, w, DT);
                    let s = activity(g);
                    act += s;
                    if k >= steps - 60 {
                        late += s;
                    }
                    let ke = nm_agent_get_kinetic_energy(g);
                    let h = pos(g, HEAD)[2];
                    if !ke.is_finite() || !h.is_finite() {
                        finite = false;
                        break;
                    }
                    ke_max = ke_max.max(ke);
                    head_min = head_min.min(h);
                    if family <= 1 && fell.is_none() && h < 1.0 {
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
                        nm_agent_get_part_velocity(g, i, v.as_mut_ptr(), wv.as_mut_ptr());
                        line += &format!(" {:.0}", (wv[0] * wv[0] + wv[1] * wv[1] + wv[2] * wv[2]).sqrt() * 57.3);
                    }
                    eprintln!("relax spin per part:{line}");
                }
                let end = pos(g, 0);
                let moved = ((end[0] - start[0]).powi(2) + (end[1] - start[1]).powi(2)).sqrt();
                writeln!(out, "{},{t},{stack},{part},{mag:.1},{:.0},{},{},{head_min:.3},{moved:.3},{:.1},{:.1},{ke_max:.1},{},{}",
                    names[family], angle.to_degrees(), fell.is_some() as i32, fell.map_or(String::new(), |x| format!("{x:.2}")),
                    act / steps as f32, late / 60.0, settle.map_or(String::new(), |x| format!("{x:.2}")), finite as i32).unwrap();
                nm_agent_destroy(g);
            }
        }
    }
}
