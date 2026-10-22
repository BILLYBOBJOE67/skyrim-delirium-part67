//! How a body stands under bodyBalance: part heights and a few body angles at the start and after a few seconds.
//!
//! usage: cargo run --release --example posture -- <character (.art or .json)> [build.txt] [seconds]

use delirium_physics::*;
use delirium_physics::character::{nm_build_from_character, nm_init_character_file};
use glam::Vec3;
use std::ffi::CString;

fn c(s: &str) -> CString {
    CString::new(s).unwrap()
}

fn err() -> String {
    let mut b = vec![0u8; 512];
    unsafe { nm_last_error(b.as_mut_ptr() as *mut _, b.len() as i32) };
    String::from_utf8_lossy(&b).trim_end_matches('\0').to_string()
}

fn main() {
    let a: Vec<String> = std::env::args().collect();
    let secs: f32 = a.iter().skip(2).find_map(|s| s.parse().ok()).unwrap_or(4.0);
    let build = a.get(2).filter(|s| s.parse::<f32>().is_err());
    unsafe {
        let ok = if a[1].ends_with(".json") { nm_init_character_file(c(&a[1]).as_ptr()) } else { nm_init(c(&a[1]).as_ptr()) };
        assert!(ok >= 0, "init: {}", err());
        let b = match build {
            Some(p) => nm_build_load(c(p).as_ptr()),
            None => nm_build_from_character(c(&std::fs::read_to_string(&a[1]).unwrap()).as_ptr()),
        };
        assert!(!b.is_null(), "build: {}", err());
        let w = nm_world_create();
        nm_world_add_floor(w, 0.0, 0.0, 0.0, 50.0);
        let g = nm_agent_create(b, [0.0f32; 3].as_ptr(), 0.0, 0.3);
        nm_agent_set_mode(g, NM_MODE_LEAD);
        nm_msg_begin(g, c("bodyBalance").as_ptr());
        nm_msg_set_bool(g, c("start").as_ptr(), 1);
        nm_msg_send(g, w);
        let joint = |i: i32| {
            let (mut parent, mut p) = (0, [0.0f32; 3]);
            nm_agent_get_joint(g, i, &mut parent, p.as_mut_ptr(), std::ptr::null_mut(), std::ptr::null_mut(), std::ptr::null_mut());
            Vec3::from(p)
        };
        let part = |i: i32| {
            let mut p = [0.0f32; 3];
            nm_agent_get_part_transform(g, i, p.as_mut_ptr(), std::ptr::null_mut());
            Vec3::from(p)
        };
        let show = |t: f32| {
            let left = (joint(1) - joint(4)).normalize();
            let fwd = left.cross(Vec3::Z).normalize();
            let deg = |a: Vec3, b: Vec3| a.normalize().dot(b.normalize()).clamp(-1.0, 1.0).acos().to_degrees();
            // knee: thigh line against shin line; lean: + forward, from vertical
            let knee = |h: i32, k: i32, an: i32| deg(joint(k) - joint(h), joint(an) - joint(k));
            let lean = |lo: Vec3, hi: Vec3| { let d = hi - lo; d.dot(fwd).atan2(d.z).to_degrees() };
            let arm = |s: i32, e: i32| deg(joint(e) - joint(s), -Vec3::Z);
            print!("feet {:.3} {:.3} ankles {:.3} hips {:.3} ", part(3).z, part(6).z, joint(3).z, joint(1).z);
            println!("t {t:4.1}  pelvis {:.2}  chest {:.2}  head {:.2} m | knees {:3.0} {:3.0}  thigh lean {:3.0} {:3.0}  trunk lean {:3.0}  head lean {:3.0} | arms out {:3.0} {:3.0}  elbows {:3.0} {:3.0}",
                part(0).z, part(10).z, part(12).z, knee(1, 2, 3), knee(4, 5, 6),
                -lean(joint(2), joint(1)), -lean(joint(5), joint(4)), lean(joint(7), joint(11)), lean(joint(12), part(12)),
                arm(14, 15), arm(18, 19), knee(14, 15, 16), knee(18, 19, 20));
        };
        show(0.0);
        let dt = 1.0 / 60.0;
        let n = (secs / dt) as usize;
        for k in 1..=n {
            nm_agent_step(g, w, dt);
            if k % 60 == 0 || k == n {
                show(k as f32 * dt);
            }
        }
    }
}
