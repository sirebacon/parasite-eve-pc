use super::*;
use std::{
    fs,
    path::PathBuf,
    sync::atomic::{AtomicU64, Ordering},
};
const NAME: &str = "BASLUS-006620000000A";
fn fixture() -> (Vec<u8>, Vec<u8>) {
    let name = CardName::new(NAME).unwrap();
    let (companion, link) = Companion::build(
        &name,
        4,
        &[Section {
            key: "pe_host/content".into(),
            version: 1,
            reader: 1,
            data: b"identity-and-state".to_vec(),
        }],
    )
    .unwrap();
    let tail = build_container(
        &[
            Record::new("pe_host.extlink", 1, link.payload().to_vec()),
            Record::new("future.unknown", 7, vec![9, 8, 7]),
        ],
        &[0; WINDOW_SIZE],
        1,
        1,
    )
    .unwrap();
    let mut block = vec![0; BLOCK_SIZE];
    block[..2].copy_from_slice(b"SC");
    block[TAIL_AT..].copy_from_slice(&tail);
    crc::update_save_crc16(&mut block);
    (block, companion)
}
fn refresh_container(tail: &mut [u8]) {
    let mut linear = gather(tail).unwrap();
    let total = usize::from(u16_at(&linear, 6));
    linear[8..12].fill(0);
    let sum = crc32(&linear[..total]);
    linear[8..12].copy_from_slice(&sum.to_le_bytes());
    let result = scatter(&linear, &tail[WINDOW_AT..WINDOW_END]).unwrap();
    tail.copy_from_slice(&result);
}
#[test]
fn checksums_and_unknown_records_match_native_envelopes() {
    assert_eq!(crc32(b"123456789"), 0xcbf43926);
    let (block, companion) = fixture();
    let save = PortableSave::new(NAME, block.clone(), Some(companion.clone())).unwrap();
    assert!(save.card().crc_ok());
    assert_eq!(save.card().mods().records.len(), 2);
    let unknown = save
        .card()
        .mods()
        .records
        .iter()
        .find(|r| r.id == fnv1a("future.unknown"))
        .unwrap();
    assert_eq!(
        (unknown.version, unknown.data.as_slice()),
        (7, [9, 8, 7].as_slice())
    );
    assert_eq!(
        save.companion().unwrap().sections()[0].data,
        b"identity-and-state"
    );
    assert_eq!(save.files()[&PathBuf::from(NAME)], block);
    assert_eq!(save.companion().unwrap().bytes(), companion);
}
#[test]
fn every_truncated_prefix_is_rejected_without_panics() {
    let (block, bytes) = fixture();
    let card = Card::parse(block.clone()).unwrap();
    let name = CardName::new(NAME).unwrap();
    let link = card.companion_link().unwrap().unwrap();
    for length in 0..BLOCK_SIZE {
        assert!(Card::parse(block[..length].to_vec()).is_err());
    }
    for length in 0..TAIL_SIZE {
        assert!(ModData::parse(&block[TAIL_AT..TAIL_AT + length]).is_err());
    }
    for length in 0..bytes.len() {
        assert!(Companion::parse(bytes[..length].to_vec(), &name, &link).is_err());
    }
}
#[test]
fn game_container_and_record_corruption_are_independent_failures() {
    let (mut block, companion) = fixture();
    block[0x100] ^= 1;
    assert!(
        PortableSave::new(NAME, block.clone(), Some(companion.clone()))
            .unwrap_err()
            .to_string()
            .contains("game checksum")
    );
    crc::update_save_crc16(&mut block);
    block[TAIL_AT + 20 + 12] ^= 1;
    crc::update_save_crc16(&mut block);
    assert!(
        PortableSave::new(NAME, block.clone(), Some(companion.clone()))
            .unwrap_err()
            .to_string()
            .contains("container checksum")
    );
    refresh_container(&mut block[TAIL_AT..]);
    crc::update_save_crc16(&mut block);
    assert!(PortableSave::new(NAME, block, Some(companion))
        .unwrap_err()
        .to_string()
        .contains("record checksum"));
}
#[test]
fn compatibility_window_overrides_only_its_known_record() {
    let mut tail = build_container(
        &[
            Record::new("pe_qol.keys", 1, vec![42]),
            Record::new("future.unknown", 7, vec![1, 2]),
        ],
        &[0; WINDOW_SIZE],
        1,
        1,
    )
    .unwrap();
    tail[WINDOW_AT..WINDOW_AT + 4].copy_from_slice(b"PKEY");
    let mods = ModData::parse(&tail).unwrap();
    mods.validate().unwrap();
    assert!(mods.window_changed);
    let key = mods
        .records
        .iter()
        .find(|r| r.id == fnv1a("pe_qol.keys"))
        .unwrap();
    assert_eq!(key.source, Source::Window);
    assert_eq!(key.version, 0);
    assert_eq!(&key.data[..4], b"PKEY");
    assert_eq!(
        mods.records
            .iter()
            .find(|r| r.id == fnv1a("future.unknown"))
            .unwrap()
            .data,
        [1, 2]
    );
}
#[test]
fn bad_container_headers_records_and_unsupported_formats_are_not_portable() {
    let (block, _) = fixture();
    for (at, value) in [(4, 2), (5, 1), (6, 0), (7, 255)] {
        let mut changed = block.clone();
        changed[TAIL_AT + at] = value;
        crc::update_save_crc16(&mut changed);
        assert!(Card::parse(changed).unwrap().validate().is_err());
    }
    let mut tail = build_container(&[], &[0; WINDOW_SIZE], 1, 1).unwrap();
    tail[6..8].copy_from_slice(&21u16.to_le_bytes());
    refresh_container(&mut tail);
    let mods = ModData::parse(&tail).unwrap();
    assert!(mods.errors[0].contains("truncated"));
    assert!(mods.validate().is_err());
    assert!(build_container(
        &[
            Record::new("same", 1, vec![]),
            Record::new("same", 2, vec![])
        ],
        &[0; WINDOW_SIZE],
        1,
        1
    )
    .is_err());
    assert!(build_container(
        &[Record::new("large", 1, vec![0; ROOM])],
        &[0; WINDOW_SIZE],
        1,
        1
    )
    .is_err());
}
#[test]
fn empty_legacy_and_unknown_tails_are_distinguished() {
    let mut tail = vec![0; TAIL_SIZE];
    assert_eq!(ModData::parse(&tail).unwrap().kind, ModKind::Empty);
    tail[0] = 1;
    assert_eq!(ModData::parse(&tail).unwrap().kind, ModKind::Unknown);
    tail[..4].copy_from_slice(b"PELT");
    let mods = ModData::parse(&tail).unwrap();
    assert_eq!(mods.kind, ModKind::Legacy);
    assert_eq!(mods.records[0].source, Source::Legacy);
}
#[test]
fn companion_outer_internal_and_section_checksums_fail_separately() {
    let (block, bytes) = fixture();
    let link = Card::parse(block)
        .unwrap()
        .companion_link()
        .unwrap()
        .unwrap();
    let name = CardName::new(NAME).unwrap();
    let mut bad = bytes.clone();
    *bad.last_mut().unwrap() ^= 1;
    assert!(Companion::parse(bad.clone(), &name, &link)
        .unwrap_err()
        .to_string()
        .contains("outer checksum"));
    let mut changed_link = link.clone();
    changed_link.checksum = crc32(&bad);
    assert!(Companion::parse(bad.clone(), &name, &changed_link)
        .unwrap_err()
        .to_string()
        .contains("internal checksum"));
    bad[88..92].fill(0);
    let checksum = crc32(&bad);
    bad[88..92].copy_from_slice(&checksum.to_le_bytes());
    changed_link.checksum = crc32(&bad);
    assert!(Companion::parse(bad, &name, &changed_link)
        .unwrap_err()
        .to_string()
        .contains("section checksum"));
    assert!(
        Companion::parse(bytes, &CardName::new("OTHER").unwrap(), &link)
            .unwrap_err()
            .to_string()
            .contains("another card")
    );
}
#[test]
fn names_links_companion_versions_and_missing_companions_are_rejected() {
    for name in ["", "../escape", "a/b", "a\\b", "x:y", "a.pex"] {
        assert!(CardName::new(name).is_err());
    }
    let (block, bytes) = fixture();
    assert!(PortableSave::new(NAME, block.clone(), None)
        .unwrap_err()
        .to_string()
        .contains("missing"));
    let link = Card::parse(block)
        .unwrap()
        .companion_link()
        .unwrap()
        .unwrap();
    let mut payload = link.payload();
    payload[17] = 1;
    assert!(CompanionLink::parse(1, &payload).is_err());
    assert!(CompanionLink::parse(2, &link.payload()).is_err());
    let mut bad = bytes;
    bad[4] = 2;
    let mut link = link;
    link.checksum = crc32(&bad);
    assert!(Companion::parse(bad, &CardName::new(NAME).unwrap(), &link)
        .unwrap_err()
        .to_string()
        .contains("header"));
}
#[test]
fn companion_sections_are_sorted_bounded_and_kept_opaque() {
    let name = CardName::new(NAME).unwrap();
    let section = |key: &str| Section {
        key: key.into(),
        version: 9,
        reader: 7,
        data: vec![1, 2, 3],
    };
    let (bytes, link) =
        Companion::build(&name, 1, &[section("z/unknown"), section("a/unknown")]).unwrap();
    let parsed = Companion::parse(bytes, &name, &link).unwrap();
    assert_eq!(parsed.sections()[0].key, "a/unknown");
    assert_eq!(parsed.sections()[0].version, 9);
    assert!(Companion::build(&name, 0, &[]).is_err());
    assert!(Companion::build(&name, 1, &[section("same"), section("same")]).is_err());
    assert!(Companion::build(&name, 1, &[section("UPPER")]).is_err());
    assert!(Companion::build(&name, 1, &[section("valid\0invalid")]).is_err());
    let mut huge = section("huge");
    huge.data = vec![0; 8192];
    assert!(Companion::build(&name, 1, &[huge]).is_err());
}
static NEXT: AtomicU64 = AtomicU64::new(0);
struct Temp(PathBuf);
impl Temp {
    fn new() -> Self {
        let path = std::env::temp_dir().join(format!(
            "pe-save-{}-{}-{}",
            std::process::id(),
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap()
                .as_nanos(),
            NEXT.fetch_add(1, Ordering::Relaxed)
        ));
        fs::create_dir(&path).unwrap();
        Self(path)
    }
}
impl Drop for Temp {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.0);
    }
}
#[test]
fn portable_export_copies_both_files_and_refuses_existing_destination() {
    let temp = Temp::new();
    let (block, companion) = fixture();
    let save = PortableSave::new(NAME, block.clone(), Some(companion.clone())).unwrap();
    let dir = temp.0.join("export");
    let path = save.export_new(&dir).unwrap();
    assert_eq!(fs::read(&path).unwrap(), block);
    assert_eq!(
        fs::read(dir.join(format!(".pe-ext/{NAME}.00000004.pex"))).unwrap(),
        companion
    );
    assert_eq!(PortableSave::from_directory(&path).unwrap(), save);
    assert!(save.export_new(&dir).is_err());
    assert_eq!(fs::read(path).unwrap(), block);
    let target = temp.0.join("copy");
    let copied = PortableSave::from_directory(&dir.join(NAME))
        .unwrap()
        .export_new(&target)
        .unwrap();
    assert_eq!(fs::read(copied).unwrap(), block);
}
