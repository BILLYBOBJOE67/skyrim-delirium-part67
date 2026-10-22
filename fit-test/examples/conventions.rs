//! Which way each joint's axes point, in body terms (along the bone, left, forward, up), and which joints are hinges:
//! the convention a character file has to follow so Delirium's behaviours bend joints the right way.
//! Prints labels only, no numbers from the character.
//!
//! usage: cargo run --release --example conventions -- <character (.art or .json)> <build.txt>

use delirium_physics::*;
use delirium_physics::character::nm_init_character_file;
use glam::{Quat, Vec3};
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
    assert!(a.len() == 3, "usage: conventions <character> <build.txt>");
    unsafe {
        let ok = if a[1].ends_with(".json") { nm_init_character_file(c(&a[1]).as_ptr()) } else { nm_init(c(&a[1]).as_ptr()) };
        assert!(ok >= 0, "init: {}", err());
        let b = nm_build_load(c(&a[2]).as_ptr());
        assert!(!b.is_null(), "build: {}", err());
        let g = nm_agent_create(b, [0.0f32, 0.0, 0.0].as_ptr(), 0.0, 1.0);
        assert!(!g.is_null(), "agent: {}", err());
        let name = |i: i32| {
            let mut s = vec![0u8; 64];
            nm_part_name(i, s.as_mut_ptr() as *mut _, 64);
            String::from_utf8_lossy(&s).trim_end_matches('\0').to_string()
        };
        let pos = |i: i32| {
            let mut p = [0.0f32; 3];
            nm_agent_get_part_transform(g, i, p.as_mut_ptr(), std::ptr::null_mut());
            Vec3::from(p)
        };
        let mut parent = [-1i32; 21];
        let mut pivot = [Vec3::ZERO; 21];
        let mut frame = [Quat::IDENTITY; 21];
        let mut kind = [0i32; 21];
        for i in 0..21 {
            let (mut pv, mut fr, mut lim) = ([0.0f32; 3], [0.0f32; 4], [0.0f32; 3]);
            kind[i] = nm_agent_get_joint(g, i as i32, &mut parent[i], pv.as_mut_ptr(), fr.as_mut_ptr(), lim.as_mut_ptr(), std::ptr::null_mut());
            pivot[i] = Vec3::from(pv);
            frame[i] = Quat::from_array(fr);
        }
        // body directions: left from the hips (left thigh joint minus right), up, forward = left x up... facing
        let left = (pivot[1] - pivot[4]).normalize();
        let up = Vec3::Z;
        let fwd = up.cross(left).normalize();
        let label = |v: Vec3, along: Vec3| -> String {
            let dirs = [("along", along), ("left", left), ("fwd", fwd), ("up", up)];
            let (n, d) = dirs.iter().map(|(n, d)| (*n, v.dot(*d))).max_by(|x, y| x.1.abs().total_cmp(&y.1.abs())).unwrap();
            let sign = if d >= 0.0 { "+" } else { "-" };
            let clean = if d.abs() > 0.9 { "" } else if d.abs() > 0.7 { " ~" } else { " ?" };
            format!("{sign}{n}{clean}")
        };
        println!("body: forward = up x left (checked: head is up {})", (pos(12) - pos(0)).dot(up) > 0.0);
        println!("{:16} {:6} {:10} {:10} {:10}", "joint (child)", "type", "x (lean)", "y", "z (twist)");
        // flip flags (booleans) from the character's joint records, when it is an .art
        let flips: Vec<[bool; 3]> = if a[1].ends_with(".art") {
            delirium_data::nm_asset::read(&std::fs::read(&a[1]).unwrap()).iter().map(|j| { println!("asset joint {} index {} three {}", j.name, j.index, j.three); j.flip }).collect()
        } else { vec![] };
        for i in 1..21 {
            if let Some(f) = flips.get(i - 1) {
                print!("flip{:?} ", f);
            }
            if kind[i] <= 0 {
                println!("{:16} none", name(i as i32));
                continue;
            }
            let kids: Vec<usize> = (0..21).filter(|&k| parent[k] == i as i32).collect();
            let along = if let Some(&k) = kids.first() { (pivot[k] - pivot[i]).normalize() } else { (pos(i as i32) - pivot[i]).normalize() };
            let f = frame[i];
            let t = if kind[i] == 2 { "hinge" } else { "3-dof" };
            println!("{:16} {:6} {:10} {:10} {:10}", name(i as i32), t, label(f * Vec3::X, along), label(f * Vec3::Y, along), label(f * Vec3::Z, along));
        }
        println!("
part axes (zero pose), capsule runs along y");
        println!("{:16} {:10} {:10} {:10}", "part", "x", "y", "z");
        for i in 0..21 {
            let mut r = [0.0f32; 4];
            let mut p = [0.0f32; 3];
            nm_agent_get_part_transform(g, i as i32, p.as_mut_ptr(), r.as_mut_ptr());
            let q = Quat::from_array(r);
            let kids: Vec<usize> = (0..21).filter(|&k| parent[k] == i as i32).collect();
            let along = if let Some(&k) = kids.first() { (pivot[k] - pivot[i]).normalize() } else if i > 0 { (pos(i as i32) - pivot[i]).normalize() } else { Vec3::Z };
            println!("{:16} {:10} {:10} {:10}", name(i as i32), label(q * Vec3::X, along), label(q * Vec3::Y, along), label(q * Vec3::Z, along));
        }
        if a[1].ends_with(".art") {
            // the character file's own zero pose (asset coordinates flip x and y), labelled against its own hips
            let d = std::fs::read(&a[1]).unwrap();
            let aj = delirium_data::nm_asset::read(&d);
            let ap = delirium_data::nm_asset::parts(&d);
            let ours = |v: [f32; 3]| Vec3::new(-v[0], -v[1], v[2]);
            let zl = (ours(aj[0].pos) - ours(aj[3].pos)).normalize();
            let zb = Vec3::Z.cross(zl);
            let ax = |v: Vec3| { let a = v.abs(); if a.x >= a.y && a.x >= a.z { format!("{}x", if v.x > 0.0 { "+" } else { "-" }) } else if a.y >= a.z { format!("{}y", if v.y > 0.0 { "+" } else { "-" }) } else { format!("{}z", if v.z > 0.0 { "+" } else { "-" }) } };
            println!("
zero pose: left is {} , probe-fwd (up x left) is {} (world axes, our coords)", ax(zl), ax(zb));
            let zlabel = |v: Vec3| {
                let dirs = [("left", zl), ("fwd", zb), ("up", Vec3::Z)];
                let (n, dd) = dirs.iter().map(|(n, d)| (*n, v.dot(*d))).max_by(|x, y| x.1.abs().total_cmp(&y.1.abs())).unwrap();
                format!("{}{}{}", if dd >= 0.0 { "+" } else { "-" }, n, if dd.abs() > 0.9 { "" } else if dd.abs() > 0.7 { " ~" } else { " ?" })
            };
            println!("{:16} {:10} {:10} {:10}", "zero part", "x", "y", "z");
            for (k, m) in ap.iter().enumerate() {
                let r = glam::Mat3::from_cols(Vec3::from(m[0]), Vec3::from(m[1]), Vec3::from(m[2])).transpose();
                let rw = glam::Mat3::from_cols(ours(r.x_axis.into()), ours(r.y_axis.into()), ours(r.z_axis.into()));
                println!("{:16} {:10} {:10} {:10}", name(k as i32), zlabel(rw.x_axis), zlabel(rw.y_axis), zlabel(rw.z_axis));
            }
        }
        println!("facing: fwd is {}", label(fwd, Vec3::Z));
        println!("left = ({:.0}, {:.0}, {:.0}) rounded", left.x, left.y, left.z);
    }
}
