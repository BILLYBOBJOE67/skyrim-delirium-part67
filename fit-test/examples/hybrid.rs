//! Diagnosis only (output goes to a scratch folder, never shipped): our character with one group of fields taken from
//! another .art, to find which group a behaviour difference comes from.
//!
//! usage: hybrid <our.json> <other.art> <out.json> <groups: muscle,limits,mass,pairs,axes>

use delirium_physics::character::Character;

fn main() {
    let a: Vec<String> = std::env::args().collect();
    let mut ours = Character::parse(&std::fs::read_to_string(&a[1]).unwrap()).unwrap();
    delirium_data::LE.store(true, std::sync::atomic::Ordering::Relaxed);
    let mut other = Character::from_xrt(&std::fs::read(&a[2]).unwrap(), None);
    other.joints.sort_by_key(|j| j.index);
    ours.joints.sort_by_key(|j| j.index);
    eprintln!("names {:?} / {:?}", ours.joints.iter().map(|j| &j.name).take(4).collect::<Vec<_>>(), other.joints.iter().map(|j| &j.name).take(4).collect::<Vec<_>>());
    let groups: Vec<&str> = a[4].split(',').collect();
    // R=a-b: limit limits/jpos/frames to joints a..=b (and their child parts a+1..=b+1); P0 adds the pelvis frame
    let range = groups.iter().find_map(|g| g.strip_prefix("R=")).map(|r| {
        let (x, y) = r.split_once('-').unwrap();
        (x.parse::<usize>().unwrap(), y.parse::<usize>().unwrap())
    });
    let inr = |i: usize| range.map_or(true, |(x, y)| i >= x && i <= y);
    let p0 = range.is_none() || groups.contains(&"P0");
    for (j, o) in ours.joints.iter_mut().zip(&other.joints) {
        if !inr(j.index as usize) && !groups.iter().any(|g| g.starts_with("show")) {
            continue;
        }
        if groups.contains(&"muscle") {
            (j.muscle_stiffness, j.damping, j.strength, j.max_torque) = (o.muscle_stiffness, o.damping, o.strength, o.max_torque);
        }
        if groups.contains(&"strength") {
            (j.muscle_stiffness, j.strength) = (o.muscle_stiffness, o.strength);
        }
        if groups.contains(&"damping") {
            j.damping = o.damping;
        }
        if groups.contains(&"maxtorque") {
            j.max_torque = o.max_torque;
        }
        if groups.iter().any(|g| g.strip_prefix("L=").is_some_and(|n| j.name.starts_with(n))) {
            j.limits = o.limits;
        }
        if groups.contains(&"limits") {
            j.limits = o.limits;
        }
        if groups.contains(&"axes") {
            (j.twist_axis, j.lean_axis, j.flip, j.three) = (o.twist_axis, o.lean_axis, o.flip, o.three);
        }
        if groups.contains(&"jpos") {
            j.pos = o.pos;
        }
        if groups.contains(&"showm") {
            // qualitative: how the other's value compares with ours, as a rounded factor
            let f = |a: f32, b: f32| if a.abs() < 1e-6 { "-".to_string() } else { format!("x{:.1}", b / a) };
            let z = |j: &delirium_physics::character::Joint| j.damping / (2.0 * j.strength.max(1e-6).sqrt());
            eprintln!("{:14} stiff {} strength {} damping {} ratio {} maxT {} {}", j.name, f(j.muscle_stiffness, o.muscle_stiffness), f(j.strength, o.strength),
                f(j.damping, o.damping), f(z(j), z(o)), f(j.max_torque[0], o.max_torque[0]), f(j.max_torque[1], o.max_torque[1]));
        }
        if groups.contains(&"show") {
            let d = |l: [[f32; 2]; 3]| l.map(|[a, b]| format!("{:.0}..{:.0}", a.to_degrees(), b.to_degrees())).join(" ");
            eprintln!("{:12} ours {:30} other-wider? {:?}", j.name, d(j.limits),
                (0..3).map(|k| { let (w0, w1) = (j.limits[k][1] - j.limits[k][0], o.limits[k][1] - o.limits[k][0]); if w1 > w0 * 1.2 { '+' } else if w1 < w0 / 1.2 { '-' } else { '=' } }).collect::<String>());
            eprintln!("{:12} centre sign ours/other {:?} flip ours/other {:?}/{:?} three {}/{}", j.name, (0..3).map(|k| { let m = |l: [f32; 2]| { let c = (l[0] + l[1]) * 0.5; if c.abs() < 0.05 { '0' } else if c > 0.0 { '+' } else { '-' } }; format!("{}{}", m(j.limits[k]), m(o.limits[k])) }).collect::<Vec<_>>(), j.flip, o.flip, j.three, o.three);
        }
    }
    if groups.contains(&"mass") {
        for (p, o) in ours.parts.iter_mut().zip(&other.parts) {
            p.mass = o.mass;
        }
    }
    if groups.contains(&"frames") {
        for (i, (p, o)) in ours.parts.iter_mut().zip(&other.parts).enumerate() {
            if if i == 0 { p0 } else { inr(i - 1) } {
                p.frame = o.frame;
            }
        }
    }
    if groups.contains(&"pairs") {
        ours.self_collision = other.self_collision.clone();
    }
    std::fs::write(&a[3], serde_json::to_string(&ours).unwrap()).unwrap();
}
