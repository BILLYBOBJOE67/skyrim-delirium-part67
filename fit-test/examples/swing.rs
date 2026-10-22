//! Offline: how far each part swings against the pelvis per second (deg, mean over parts) for behaviours on the ground,
//! to compare with the in-game "limbs swung" trace.
//!
//! usage: cargo run --release --example swing -- <Fred.art> <skyrim_build.txt>

use delirium_physics::*;
use std::ffi::CString;

const DT: f32 = 1.0 / 60.0;
const CHEST: i32 = 10;

fn c(s: &str) -> CString {
    CString::new(s).unwrap()
}

#[derive(Clone, Copy)]
enum V {
    F(f32),
    B(bool),
    S(&'static str),
}

type Msg = (&'static str, &'static [(&'static str, V)]);

unsafe fn send(g: *mut NmAgent, w: *mut NmWorld, m: &Msg) {
    unsafe {
        nm_msg_begin(g, c(m.0).as_ptr());
        for (k, v) in m.1 {
            let k = c(k);
            match *v {
                V::F(x) => nm_msg_set_float(g, k.as_ptr(), x),
                V::B(x) => nm_msg_set_bool(g, k.as_ptr(), x as i32),
                V::S(x) => nm_msg_set_string(g, k.as_ptr(), c(x).as_ptr()),
            };
        }
        nm_msg_send(g, w);
    }
}

type Qt = [f32; 4]; // xyzw

fn conj(q: Qt) -> Qt {
    [-q[0], -q[1], -q[2], q[3]]
}

fn mul(a: Qt, b: Qt) -> Qt {
    [
        a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
        a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
        a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
        a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2],
    ]
}

unsafe fn rel(g: *mut NmAgent) -> Vec<Qt> {
    unsafe {
        let n = nm_agent_part_count(g);
        let mut p = [0.0f32; 3];
        let mut q0 = [0.0f32; 4];
        nm_agent_get_part_transform(g, 0, p.as_mut_ptr(), q0.as_mut_ptr());
        (1..n)
            .map(|i| {
                let mut q = [0.0f32; 4];
                nm_agent_get_part_transform(g, i, p.as_mut_ptr(), q.as_mut_ptr());
                mul(conj(q0), q)
            })
            .collect()
    }
}

unsafe fn run(build: *const NmBuild, w: *mut NmWorld, name: &str, stack: &[Msg]) {
    unsafe {
        let g = nm_agent_create(build, [0.0f32; 3].as_ptr(), 0.0, 0.3);
        nm_agent_set_mode(g, NM_MODE_LEAD);
        send(g, w, &("bodyBalance", &[("start", V::B(true))]));
        for _ in 0..30 {
            nm_agent_step(g, w, DT);
        }
        send(g, w, &("stopAllBehaviours", &[]));
        send(g, w, &("bodyRelax", &[("start", V::B(true))]));
        let mut p = [0.0f32; 3];
        nm_agent_get_part_transform(g, CHEST, p.as_mut_ptr(), std::ptr::null_mut());
        nm_agent_apply_push(g, p.as_ptr(), [0.0f32, -120.0, 0.0].as_ptr());
        for _ in 0..150 {
            nm_agent_step(g, w, DT);
        }
        send(g, w, &("stopAllBehaviours", &[]));
        for m in stack {
            send(g, w, m);
        }
        let mut line = String::new();
        for _sec in 0..3 {
            let r0 = rel(g);
            let mut top = vec![0.0f32; r0.len()];
            for _ in 0..60 {
                nm_agent_step(g, w, DT);
                for (j, r) in rel(g).iter().enumerate() {
                    let f = r0[j];
                    let d = (f[0] * r[0] + f[1] * r[1] + f[2] * r[2] + f[3] * r[3]).abs().min(1.0);
                    top[j] = top[j].max(2.0 * d.acos() * 57.3);
                }
            }
            line += &format!(" {:>4.0}", top.iter().sum::<f32>() / top.len() as f32);
        }
        println!("{name:<40} swing per second (deg):{line}");
        nm_agent_destroy(g);
    }
}

fn main() {
    let a: Vec<String> = std::env::args().collect();
    use V::*;
    unsafe {
        assert!(nm_init(c(&a[1]).as_ptr()) >= 0);
        let build = nm_build_load(c(&a[2]).as_ptr());
        assert!(!build.is_null());
        let w = nm_world_create();
        nm_world_add_floor(w, 0.0, 0.0, 0.0, 50.0);
        run(build, w, "relax", &[("bodyRelax", &[("start", B(true))])]);
        run(build, w, "writhe fire (plugin: 2.2, 0.4, roll)", &[("bodyWrithe", &[("start", B(true)), ("mask", S("fb")), ("armAmplitude", F(2.2)),
            ("legAmplitude", F(2.2)), ("backAmplitude", F(2.2)), ("armPeriod", F(0.4)), ("legPeriod", F(0.4)), ("backPeriod", F(0.4)),
            ("rollOverFlag", B(true))])]);
        run(build, w, "writhe default", &[("bodyWrithe", &[("start", B(true))])]);
        run(build, w, "writhe shock + electrocute", &[("bodyWrithe", &[("start", B(true)), ("mask", S("fb")), ("armAmplitude", F(1.3)),
            ("legAmplitude", F(1.3)), ("backAmplitude", F(1.3)), ("armPeriod", F(0.3)), ("legPeriod", F(0.3)), ("backPeriod", F(0.3))]),
            ("electrocute", &[("start", B(true)), ("stunMag", F(0.5)), ("stunInterval", F(0.07)), ("largeMult", F(3.0)), ("directionRandomness", F(0.5))])]);
    }
}
