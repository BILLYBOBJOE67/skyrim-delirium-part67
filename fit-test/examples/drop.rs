use delirium_physics::*;
use std::ffi::CString;
fn c(s: &str) -> CString { CString::new(s).unwrap() }
fn main() {
    let a: Vec<String> = std::env::args().collect();
    unsafe {
        println!("init {}", nm_init(c(&a[1]).as_ptr()));
        let b = nm_build_load(c(&a[2]).as_ptr());
        let w = nm_world_create();
        nm_world_add_floor(w, 0.0, 0.0, 0.0, 50.0);
        for (name, z, bal) in [("drop 0.5 m, no msg", 0.5f32, false), ("drop 0.5 m, bodyBalance", 0.5, true), ("floor, add_velocity", 0.0, true)] {
            let g = nm_agent_create(b, [0.0, 0.0, z].as_ptr(), 0.0, 0.3);
            if bal { nm_msg_begin(g, c("bodyBalance").as_ptr()); nm_msg_set_bool(g, c("start").as_ptr(), 1); println!("  send {}", nm_msg_send(g, w)); }
            for k in 0..120 {
                if z == 0.0 && k == 10 { nm_agent_add_velocity(g, [2.0, 0.0, 0.0].as_ptr()); }
                nm_agent_step(g, w, 1.0 / 60.0);
                if k % 20 == 0 { let mut p = [0.0f32; 3]; nm_agent_get_part_transform(g, 0, p.as_mut_ptr(), std::ptr::null_mut());
                    println!("{name} k={k} pelvis={p:.3?} awake={} ke={:.2}", nm_agent_is_awake(g), nm_agent_get_kinetic_energy(g)); }
            }
            nm_agent_destroy(g);
        }
    }
}
