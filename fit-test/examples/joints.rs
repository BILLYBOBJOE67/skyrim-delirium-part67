fn main() {
    for p in std::env::args().skip(1) {
        let d = std::fs::read(&p).unwrap();
        let r = std::panic::catch_unwind(|| delirium_data::nm_asset::read(&d));
        match r {
            Ok(j) => println!("{p}: {} bytes, {} joints, parts {}, first 3: {:?}", d.len(), j.len(), delirium_data::nm_asset::parts(&d).len(), j.iter().take(3).map(|j| j.name.clone()).collect::<Vec<_>>()),
            Err(_) => println!("{p}: panic"),
        }
    }
}
