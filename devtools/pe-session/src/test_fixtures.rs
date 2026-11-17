//! Synthetic source-only saves shared by the session's subprocess and transport tests.
pub const NAME: &str = "BASLUS-006620000000A";
pub fn save_bundle(with_companion: bool) -> (Vec<u8>, Option<Vec<u8>>) {
    use pe_save::*;
    let mut records = vec![Record::new("future.unknown", 7, vec![9, 8, 7])];
    let companion = if with_companion {
        let (bytes, link) = Companion::build(
            &CardName::new(NAME).unwrap(),
            4,
            &[Section {
                key: "pe_host/content".into(),
                version: 1,
                reader: 1,
                data: b"identity-and-state".to_vec(),
            }],
        )
        .unwrap();
        records.push(Record::new("pe_host.extlink", 1, link.payload().to_vec()));
        Some(bytes)
    } else {
        None
    };
    let tail = build_container(&records, &[0; WINDOW_SIZE], 1, 1).unwrap();
    let mut block = vec![0; BLOCK_SIZE];
    block[..2].copy_from_slice(b"SC");
    block[TAIL_AT..].copy_from_slice(&tail);
    crc::update_save_crc16(&mut block);
    (block, companion)
}
