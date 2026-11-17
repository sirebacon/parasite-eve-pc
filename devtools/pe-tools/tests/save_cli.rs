use std::{
    fs,
    path::PathBuf,
    process::{Command, Output, Stdio},
    thread,
    time::{Duration, Instant},
};
const NAME: &str = "BASLUS-006620000000A";
fn cli(args: &[&str]) -> Output {
    Command::new(env!("CARGO_BIN_EXE_pe-tools"))
        .env_remove("PE_DEBUG_CONTROL")
        .args(args)
        .output()
        .unwrap()
}
struct Temp(PathBuf);
impl Temp {
    fn new(label: &str) -> Self {
        let path = std::env::temp_dir().join(format!(
            "pe-save-cli-{}-{label}-{}",
            std::process::id(),
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap()
                .as_nanos()
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
fn synthetic_save() -> (Vec<u8>, Vec<u8>) {
    use pe_save::*;
    let (companion, link) = Companion::build(
        &CardName::new(NAME).unwrap(),
        4,
        &[Section {
            key: "example/state".into(),
            version: 2,
            reader: 1,
            data: vec![1, 2, 3],
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
fn write_card(directory: &std::path::Path) -> PathBuf {
    let (block, companion) = synthetic_save();
    fs::create_dir(directory.join(".pe-ext")).unwrap();
    let card = directory.join(NAME);
    fs::write(&card, block).unwrap();
    fs::write(
        directory.join(format!(".pe-ext/{NAME}.00000004.pex")),
        companion,
    )
    .unwrap();
    card
}
#[test]
fn card_check_and_copy_preserve_both_files_and_unknown_records() {
    let temp = Temp::new("copy");
    let card = write_card(&temp.0);
    let target = temp.0.join("copied");
    let out = cli(&["card-check", card.to_str().unwrap()]);
    assert!(out.status.success());
    let summary: serde_json::Value = serde_json::from_slice(&out.stdout).unwrap();
    assert_eq!(summary["records"].as_array().unwrap().len(), 2);
    assert_eq!(summary["game_crc_ok"], true);
    assert!(cli(&[
        "card-copy",
        card.to_str().unwrap(),
        target.to_str().unwrap()
    ])
    .status
    .success());
    let (block, companion) = synthetic_save();
    assert_eq!(fs::read(target.join(NAME)).unwrap(), block);
    assert_eq!(
        fs::read(target.join(format!(".pe-ext/{NAME}.00000004.pex"))).unwrap(),
        companion
    );
    assert_eq!(
        cli(&[
            "card-copy",
            card.to_str().unwrap(),
            target.to_str().unwrap()
        ])
        .status
        .code(),
        Some(1)
    );
}
#[test]
fn missing_companions_and_corrupt_cards_cannot_be_reported_as_portable() {
    let temp = Temp::new("bad");
    let card = write_card(&temp.0);
    fs::remove_file(temp.0.join(format!(".pe-ext/{NAME}.00000004.pex"))).unwrap();
    let target = temp.0.join("copied");
    assert_eq!(
        cli(&[
            "card-copy",
            card.to_str().unwrap(),
            target.to_str().unwrap()
        ])
        .status
        .code(),
        Some(1)
    );
    assert!(!target.exists());
    let mut block = fs::read(&card).unwrap();
    block[0x100] ^= 1;
    fs::write(&card, block).unwrap();
    assert_eq!(
        cli(&["card-check", card.to_str().unwrap()]).status.code(),
        Some(1)
    );
}
#[test]
fn attached_save_export_uses_the_native_file_channel() {
    let temp = Temp::new("attached");
    let target = temp.0.join("exported");
    let mut child = Command::new(env!("CARGO_BIN_EXE_pe-tools"))
        .env("PE_DEBUG_CONTROL", &temp.0)
        .args([
            "save-export",
            "--out",
            target.to_str().unwrap(),
            "--timeout-ms",
            "5000",
        ])
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .unwrap();
    let start = Instant::now();
    loop {
        if fs::read_to_string(temp.0.join("cmd")).is_ok_and(|s| s == "save-block 1\n") {
            break;
        }
        if start.elapsed() > Duration::from_secs(5) {
            let _ = child.kill();
            let _ = child.wait();
            panic!("save-export did not send command");
        }
        thread::sleep(Duration::from_millis(5));
    }
    let (block, companion) = synthetic_save();
    fs::write(temp.0.join("block.bin"), block.clone()).unwrap();
    fs::write(temp.0.join("companion.pex"), companion).unwrap();
    fs::remove_file(temp.0.join("cmd")).unwrap();
    fs::write(
        temp.0.join("out"),
        format!("ok save-block {NAME} companion={NAME}.00000004.pex\n"),
    )
    .unwrap();
    let out = child.wait_with_output().unwrap();
    assert!(
        out.status.success(),
        "{}",
        String::from_utf8_lossy(&out.stderr)
    );
    assert_eq!(fs::read(target.join(NAME)).unwrap(), block);
    assert!(pe_save::PortableSave::from_directory(&target.join(NAME)).is_ok());
}
#[test]
fn invalid_save_export_arguments_return_two() {
    assert_eq!(cli(&["save-export"]).status.code(), Some(2));
    assert_eq!(
        cli(&["save-export", "--slot", "NaN"]).status.code(),
        Some(2)
    );
}
