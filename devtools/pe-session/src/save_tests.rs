use crate::{
    save::SaveClient,
    test_fixtures::{save_bundle, NAME},
    transport::DebugTransport,
    SessionError,
};
use pe_protocol::{Command, Reply};
use pe_save::{BLOCK_SIZE, TAIL_SIZE};
use std::{
    fs,
    path::{Path, PathBuf},
    sync::atomic::{AtomicU64, Ordering},
};
static NEXT: AtomicU64 = AtomicU64::new(0);
struct Temp(PathBuf);
impl Temp {
    fn new() -> Self {
        let path = std::env::temp_dir().join(format!(
            "pe-save-client-{}-{}-{}",
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
struct Mock {
    directory: PathBuf,
    block: Vec<u8>,
    companion: Option<Vec<u8>>,
    reply: String,
    tail: Vec<u8>,
}
impl Mock {
    fn valid(directory: &Path) -> Self {
        let (block, companion) = save_bundle(true);
        Self {
            directory: directory.into(),
            tail: block[pe_save::TAIL_AT..].to_vec(),
            block,
            companion,
            reply: format!("ok save-block {NAME} companion={NAME}.00000004.pex"),
        }
    }
}
impl DebugTransport for Mock {
    fn execute(&mut self, command: &str) -> Result<Reply, SessionError> {
        let reply = if command == "mods-tail" {
            fs::write(self.directory.join("tail.bin"), &self.tail).unwrap();
            "ok mods-tail tail.bin".to_owned()
        } else {
            if self.reply.starts_with("ok ") {
                fs::write(self.directory.join("block.bin"), &self.block).unwrap();
                if let Some(bytes) = &self.companion {
                    fs::write(self.directory.join("companion.pex"), bytes).unwrap();
                }
            }
            self.reply.clone()
        };
        Ok(Reply::matching(&Command::new(command).unwrap(), &reply).unwrap())
    }
}
#[test]
fn validates_reply_paths_duplicates_and_link_metadata() {
    for detail in [
        "../escape".to_owned(),
        format!("{NAME} companion=../escape"),
        format!("{NAME} companion=a.pex companion=b.pex"),
        NAME.into(),
        format!("{NAME} companion={NAME}.00000005.pex"),
    ] {
        let temp = Temp::new();
        let mut transport = Mock::valid(&temp.0);
        transport.reply = format!("ok save-block {detail}");
        assert!(
            SaveClient::new(transport, temp.0.clone())
                .save_files(1)
                .is_err(),
            "{detail}"
        );
    }
}
#[test]
fn missing_corrupt_and_unreferenced_companions_are_distinguished() {
    for corrupt in [false, true] {
        let temp = Temp::new();
        let mut transport = Mock::valid(&temp.0);
        if corrupt {
            *transport.companion.as_mut().unwrap().last_mut().unwrap() ^= 1;
        } else {
            transport.companion = None;
        }
        assert!(SaveClient::new(transport, temp.0.clone())
            .save_files(1)
            .is_err());
    }
    let temp = Temp::new();
    let mut transport = Mock::valid(&temp.0);
    transport.block = save_bundle(false).0;
    transport.reply = format!("ok save-block {NAME}");
    // Stale companion bytes exist, but the new card does not reference them.
    assert!(SaveClient::new(transport, temp.0.clone())
        .save_files(1)
        .unwrap()
        .companion()
        .is_none());
}
#[test]
fn binary_sizes_are_required_and_native_errors_win_over_stale_files() {
    for size in [TAIL_SIZE - 1, TAIL_SIZE + 1] {
        let temp = Temp::new();
        let mut transport = Mock::valid(&temp.0);
        transport.tail = vec![0; size];
        assert!(SaveClient::new(transport, temp.0.clone())
            .mods_tail()
            .is_err());
    }
    let temp = Temp::new();
    let mut transport = Mock::valid(&temp.0);
    transport.block.truncate(BLOCK_SIZE - 1);
    assert!(SaveClient::new(transport, temp.0.clone())
        .save_block(1)
        .is_err());
    let temp = Temp::new();
    fs::write(temp.0.join("block.bin"), save_bundle(true).0).unwrap();
    let mut transport = Mock::valid(&temp.0);
    transport.reply = "err save-block not in free field".into();
    assert!(SaveClient::new(transport, temp.0.clone())
        .save_files(1)
        .unwrap_err()
        .to_string()
        .contains("save-block 1: err save-block not in free field"));
}
