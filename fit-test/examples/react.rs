//! Offline check of the plugin's hit reactions on the fitted Skyrim body: does she stay up, how far she goes, how many steps.
//!
//! usage: cargo run --release --example react -- <Fred.art> <skyrim_build.txt>

use delirium_physics::*;
use std::ffi::CString;

const DT: f32 = 1.0 / 60.0;
const CHEST: i32 = 10;
const HEAD: i32 = 12;

fn c(s: &str) -> CString {
    CString::new(s).unwrap()
}

unsafe fn pos(g: *mut NmAgent, i: i32) -> [f32; 3] {
    let mut p = [0.0f32; 3];
    unsafe { nm_agent_get_part_transform(g, i, p.as_mut_ptr(), std::ptr::null_mut()) };
    p
}

#[derive(Clone, Copy, Debug)]
struct Case {
    name: &'static str,
    push: f32,       // N s at once, at the chest
    lean: f32,       // leanInDirection amount (0: none)
    spin: f32,       // shot shock spin multiplier (0: none)
    head: f32,       // N s at the head
    carry: f32,      // N s spread over carry_t (a shove that keeps going)
    carry_t: f32,
    melee_set: bool, // shot task set "MELEE"
}

unsafe fn run(build: *const NmBuild, w: *mut NmWorld, k: Case) {
    unsafe {
        let g = nm_agent_create(build, [0.0f32; 3].as_ptr(), 0.0, 0.3);
        nm_agent_set_mode(g, NM_MODE_LEAD);
        nm_msg_begin(g, c("bodyBalance").as_ptr());
        nm_msg_set_bool(g, c("start").as_ptr(), 1);
        nm_msg_send(g, w);
        let dir = [0.0f32, -1.0, 0.0]; // hit from the front: pushed back along -y
        let mut fell = None;
        let mut head_min = f32::MAX;
        let p0 = pos(g, 0);
        let hit_at = 30;
        let carry_frames = (k.carry_t / DT).max(1.0) as i32;
        for f in 0..300 {
            if f == hit_at {
                let chest = pos(g, CHEST);
                let set = c(if k.melee_set { "MELEE" } else { "*" });
                nm_agent_task_shot(g, w, set.as_ptr(), CHEST, chest.as_ptr(), dir.as_ptr(), 198.0);
                if k.spin > 0.0 {
                    nm_msg_begin(g, c("shot").as_ptr());
                    nm_msg_set_bool(g, c("alwaysAddShockSpin").as_ptr(), 1);
                    nm_msg_set_float(g, c("shockSpinMin").as_ptr(), 40.0 * k.spin);
                    nm_msg_set_float(g, c("shockSpinMax").as_ptr(), 80.0 * k.spin);
                    nm_msg_send(g, w);
                }
                if k.lean > 0.0 {
                    nm_msg_begin(g, c("leanInDirection").as_ptr());
                    nm_msg_set_float(g, c("leanAmount").as_ptr(), k.lean);
                    nm_msg_set_vec3(g, c("dir").as_ptr(), dir.as_ptr());
                    nm_msg_send(g, w);
                }
                if k.head > 0.0 {
                    let h = pos(g, HEAD);
                    nm_agent_apply_push(g, h.as_ptr(), [dir[0] * k.head, dir[1] * k.head, 0.0].as_ptr());
                }
                nm_agent_apply_push(g, chest.as_ptr(), [dir[0] * k.push, dir[1] * k.push, 0.0].as_ptr());
            }
            if k.carry > 0.0 && f > hit_at && f <= hit_at + carry_frames {
                let chest = pos(g, CHEST);
                let n = k.carry / carry_frames as f32;
                nm_agent_apply_push(g, chest.as_ptr(), [dir[0] * n, dir[1] * n, 0.0].as_ptr());
            }
            nm_agent_step(g, w, DT);
            let h = pos(g, HEAD)[2];
            if f > hit_at {
                head_min = head_min.min(h);
            }
            if fell.is_none() && f > hit_at && (nm_agent_has_fallen(g) != 0 || h < 1.0) {
                fell = Some((f - hit_at) as f32 * DT);
            }
        }
        let p1 = pos(g, 0);
        let moved = ((p1[0] - p0[0]).powi(2) + (p1[1] - p0[1]).powi(2)).sqrt();
        println!("{:<34} push {:>4.0} lean {:.2} spin {:.1} head {:>3.0} carry {:>4.0}/{:.1}s | {} head min {:.2} m, moved {:.2} m, balancing {}",
            k.name, k.push, k.lean, k.spin, k.head, k.carry, k.carry_t,
            fell.map_or("STAYS UP      ".to_string(), |t| format!("falls at {t:.2} s")), head_min, moved, nm_agent_is_balancing(g) != 0);
        nm_agent_destroy(g);
    }
}

fn main() {
    let a: Vec<String> = std::env::args().collect();
    unsafe {
        assert!(if a[1].ends_with(".json") { delirium_physics::character::nm_init_character_file(c(&a[1]).as_ptr()) } else { nm_init(c(&a[1]).as_ptr()) } >= 0);
        let build = if a[2].ends_with(".json") { delirium_physics::character::nm_build_from_character(c(&std::fs::read_to_string(&a[2]).unwrap()).as_ptr()) } else { nm_build_load(c(&a[2]).as_ptr()) };
        assert!(!build.is_null());
        let w = nm_world_create();
        nm_world_add_floor(w, 0.0, 0.0, 0.0, 50.0);
        let base = Case { name: "", push: 100.0, lean: 0.0, spin: 0.0, head: 0.0, carry: 0.0, carry_t: 0.5, melee_set: true };
        let cases = [
            Case { name: "spin3 head30", spin: 3.0, head: 30.0, ..base },
            Case { name: "spin3 head30 lean.15", spin: 3.0, head: 30.0, lean: 0.15, ..base },
            Case { name: "spin3 head30 lean.25", spin: 3.0, head: 30.0, lean: 0.25, ..base },
            Case { name: "spin3 head30 lean.35", spin: 3.0, head: 30.0, lean: 0.35, ..base },
            Case { name: "push150 spin3 head45", push: 150.0, spin: 3.0, head: 45.0, ..base },
            Case { name: "push150 spin3 head45 lean.2", push: 150.0, spin: 3.0, head: 45.0, lean: 0.2, ..base },
            Case { name: "push200 spin3 head45", push: 200.0, spin: 3.0, head: 45.0, ..base },
            Case { name: "push100 carry100/0.4 spin3 head30", carry: 100.0, carry_t: 0.4, spin: 3.0, head: 30.0, ..base },
        ];
        for k in cases {
            run(build, w, k);
        }
    }
}
