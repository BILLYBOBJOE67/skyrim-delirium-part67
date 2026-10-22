//! A test body with Skyrim's ragdoll joints: our character, with each joint's limit replaced by the matching Skyrim
//! constraint from the plugin's dump (SKSE log folder, SkyrimDelirium_ragdoll_<FormID>.json). The brain keeps our
//! character's joints; only the body's limit solver uses Skyrim's frames and ranges (BuildPart::body_limit).
//!
//! Skyrim has fewer bodies than Delirium has parts (no clavicles; its chest body carries the arms and neck). A Skyrim
//! joint that spans several Delirium joints goes on the lowest one, expressed through the zero pose, and the Delirium
//! joints it skips are locked.
//!
//! usage: skyrim_body <character.json> <ragdoll dump.json> <out body.json>

use delirium_physics::character::{Character, JointLimit};
use glam::{Mat3, Quat, Vec3};
use serde_json::Value;

fn quat(v: &Value) -> Quat {
    let f = |i: usize| v[i].as_f64().unwrap() as f32;
    Quat::from_xyzw(f(0), f(1), f(2), f(3)).normalize()
}

// Havok's constraint frame (columns in the body's frame) as a rotation
fn frame(v: &Value) -> Quat {
    let c = |i: usize| Vec3::new(v[i][0].as_f64().unwrap() as f32, v[i][1].as_f64().unwrap() as f32, v[i][2].as_f64().unwrap() as f32);
    Quat::from_mat3(&Mat3::from_cols(c(0), c(1), c(2)).orthonormalize()).normalize()
}

trait Ortho {
    fn orthonormalize(self) -> Mat3;
}
impl Ortho for Mat3 {
    fn orthonormalize(self) -> Mat3 {
        let x = self.x_axis.normalize();
        let y = (self.y_axis - x * x.dot(self.y_axis)).normalize();
        Mat3::from_cols(x, y, x.cross(y))
    }
}

// Delirium's joint axes from Havok's: z = twist (ragdoll) or axle (hinge), x = planes axis / first perpendicular
fn to_delirium() -> Quat {
    Quat::from_mat3(&Mat3::from_cols(Vec3::Y, Vec3::Z, Vec3::X))
}

// swing (about x, y) and twist (about z) of a joint rotation, the way Delirium splits it
fn split(q: Quat) -> (f32, f32, f32) {
    let q = if q.w < 0.0 { -q } else { q };
    let twist = 2.0 * q.z.atan2(q.w);
    let s = q * Quat::from_rotation_z(-twist);
    let (axis, angle) = s.to_axis_angle();
    (axis.x * angle, axis.y * angle, twist)
}

fn main() {
    let a: Vec<String> = std::env::args().collect();
    let mut ch = Character::parse(&std::fs::read_to_string(&a[1]).unwrap()).unwrap();
    let dump: Value = serde_json::from_str(&std::fs::read_to_string(&a[2]).unwrap()).unwrap();
    let build = ch.build.as_mut().expect("the character has no build");
    let world: Vec<Quat> = build.iter().map(|p| Quat::from_array(p.frame.rot).normalize()).collect();
    let parent: Vec<Option<usize>> = build.iter().map(|p| p.parent).collect();
    let name: Vec<String> = build.iter().map(|p| p.name.clone()).collect();
    let mut set: Vec<Option<(JointLimit, String)>> = (0..build.len()).map(|_| None).collect();
    let mut locked = vec![false; build.len()];
    let deg = |r: f32| r.to_degrees();

    // bodies on no Delirium part (NPC COM: the plugin carries it on the pelvis): put them on the root part, turned so
    // their joints' two frames meet at the zero pose, as all the mapped joints' do (checked by the zero-pose print)
    let mut joints: Vec<Value> = dump["joints"].as_array().unwrap().clone();
    let unmapped: Vec<String> = joints.iter().filter(|j| j["parent"]["part"].as_i64() == Some(-1)).map(|j| j["parent"]["bone"].as_str().unwrap().to_string()).collect();
    for bone in unmapped.iter().collect::<std::collections::BTreeSet<_>>() {
        let est: Vec<Quat> = joints.iter().filter(|j| j["parent"]["bone"] == bone.as_str() && j["child"]["part"].as_i64().unwrap_or(-1) >= 0).map(|j| {
            let k = j["child"]["part"].as_u64().unwrap() as usize;
            world[0].inverse() * world[k] * quat(&j["child"]["body_in_part"]) * frame(&j["frame_child"]) * frame(&j["frame_parent"]).inverse()
        }).collect();
        let Some(&e) = est.first() else { continue };
        let spread = est.iter().map(|q| q.angle_between(e).to_degrees()).fold(0.0f32, f32::max);
        eprintln!("{bone} on {} (estimated from {} joints, spread {spread:.1} deg)", name[0], est.len());
        for j in joints.iter_mut().filter(|j| j["parent"]["bone"] == bone.as_str()) {
            j["parent"]["part"] = 0.into();
            j["parent"]["body_in_part"] = serde_json::json!(e.to_array());
        }
    }

    for j in &joints {
        let (c, p) = (j["child"]["part"].as_i64().unwrap_or(-1), j["parent"]["part"].as_i64().unwrap_or(-1));
        let (cb, pb) = (j["child"]["bone"].as_str().unwrap_or("?"), j["parent"]["bone"].as_str().unwrap_or("?"));
        if c < 0 || p < 0 || c == p || j["child"]["body_in_part"].is_null() || j["parent"]["body_in_part"].is_null() {
            eprintln!("skip  {cb} -> {pb} (no Delirium part, or both bodies on one part)");
            continue;
        }
        let (c, p) = (c as usize, p as usize);
        // the Delirium joint it goes on: c's own; the joints between c's parent and p are locked
        let q = parent[c].expect("root part as a child");
        let mut chain = vec![];
        let mut at = q;
        while at != p {
            chain.push(at);
            match parent[at] {
                Some(u) => at = u,
                None => panic!("{} is not above {} in Delirium's tree", name[p], name[c]),
            }
        }
        let f = to_delirium();
        let cf = quat(&j["child"]["body_in_part"]) * frame(&j["frame_child"]) * f;
        // the parent's joint frame, moved from part p to part q through the zero pose
        let in_p = quat(&j["parent"]["body_in_part"]) * frame(&j["frame_parent"]) * f;
        let into_q = world[q].inverse() * world[p];
        let (limit, text, rel_centre) = if j["type"] == "hinge" {
            let (lo, hi) = (j["angle"][0].as_f64().unwrap() as f32, j["angle"][1].as_f64().unwrap() as f32);
            let pf = into_q * in_p;
            (JointLimit::BoneHinge { parent_frame: pf.to_array(), child_frame: cf.to_array(), lo, hi }, format!("hinge {:.0}..{:.0}", deg(lo), deg(hi)), Quat::IDENTITY)
        } else {
            let tw = [j["twist"][0].as_f64().unwrap() as f32, j["twist"][1].as_f64().unwrap() as f32];
            let cone = j["cone"].as_f64().unwrap() as f32;
            let pl = [j["planes"][0].as_f64().unwrap() as f32, j["planes"][1].as_f64().unwrap() as f32];
            // planes tilt the twist axis toward the planes axis (about y here); the cone bounds it all round
            let (lo, hi) = (pl[0].max(-cone), pl[1].min(cone));
            let (pc, ph) = ((lo + hi) * 0.5, ((hi - lo) * 0.5).max(0.0));
            let (tc, th) = ((tw[0] + tw[1]) * 0.5, ((tw[1] - tw[0]) * 0.5).max(0.0));
            let centre = Quat::from_rotation_y(pc) * Quat::from_rotation_z(tc);
            let pf = into_q * in_p * centre;
            (JointLimit::Bone { parent_frame: pf.to_array(), child_frame: cf.to_array(), lean: [ph, cone], twist: th },
             format!("planes {:.0}..{:.0} cone {:.0} twist {:.0}..{:.0}", deg(pl[0]), deg(pl[1]), deg(cone), deg(tw[0]), deg(tw[1])), centre)
        };
        // the zero pose in this joint: where our standing pose sits in Skyrim's range
        let rel = (into_q * in_p * rel_centre).inverse() * (world[q].inverse() * world[c]) * cf;
        let (sx, sy, st) = split(rel);
        let note = if chain.is_empty() { String::new() } else { format!(", locks {}", chain.iter().map(|&k| name[k].as_str()).collect::<Vec<_>>().join(" ")) };
        eprintln!("{:16} <- {:28} {:48} zero pose: swing x {:4.0} y {:4.0} twist {:4.0}{note}", name[c], format!("{cb} / {pb}"), text, deg(sx), deg(sy), deg(st));
        for &k in &chain {
            locked[k] = true;
        }
        set[c] = Some((limit, text));
    }

    // sign check: bend each knee and elbow the anatomical way and see where the angle goes
    let (hl, hr) = (build.iter().position(|p| p.name == "Thigh_Left").unwrap(), build.iter().position(|p| p.name == "Thigh_Right").unwrap());
    let fwd = (Vec3::from_array(build[hl].frame.pos) - Vec3::from_array(build[hr].frame.pos)).cross(Vec3::Z).normalize();
    let left = Vec3::Z.cross(fwd);
    for (n, axis) in [("Shin_Left", left), ("Shin_Right", left), ("Lower_Arm_Left", Vec3::Z), ("Lower_Arm_Right", -Vec3::Z)] {
        let k = build.iter().position(|p| p.name == n).unwrap();
        if let Some((JointLimit::BoneHinge { parent_frame, child_frame, .. }, _)) = &set[k] {
            let q = parent[k].unwrap();
            let ang = |w: Quat| split((Quat::from_array(*parent_frame)).inverse() * world[q].inverse() * w * Quat::from_array(*child_frame)).2;
            let bent = Quat::from_axis_angle(axis, 0.5) * world[k];
            eprintln!("bend {n:16} straight {:5.0}  bent 29 deg {:5.0}", deg(ang(world[k])), deg(ang(bent)));
        }
    }

    let mut n = 0;
    for (k, p) in build.iter_mut().enumerate() {
        if let Some((l, _)) = set[k].take() {
            p.body_limit = Some(l);
            n += 1;
        } else if locked[k] {
            // held at its zero pose (Delirium's Locked means the part's rest, which our builds leave at identity)
            let pf = world[p.parent.unwrap()].inverse() * world[k];
            p.body_limit = Some(JointLimit::Bone { parent_frame: pf.to_array(), child_frame: Quat::IDENTITY.to_array(), lean: [0.0; 2], twist: 0.0 });
        } else if p.parent.is_some() {
            eprintln!("keeps our limits: {}", p.name);
        }
    }
    eprintln!("{n} joints from Skyrim, {} locked", locked.iter().filter(|&&l| l).count());
    std::fs::write(&a[3], serde_json::to_string(&ch).unwrap()).unwrap();
}
