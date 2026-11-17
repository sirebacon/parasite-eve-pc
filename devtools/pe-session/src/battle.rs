//! Rust equivalents of pedebug/battle.py for headless and attached sessions.
pub use crate::transport::{AttachedTransport, DebugTransport};
use crate::{process::GameSession, SessionError};
use pe_protocol::{
    battle::{BattleEvent, BattleState, ModState},
    Command, ProtocolError, Reply, RngSnapshot, Status,
};
use std::{
    fs::File,
    io::{BufRead, BufReader},
    path::{Path, PathBuf},
    time::Duration,
};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Stat {
    Level,
    Hp,
    PeRaw,
    Bp,
}
impl Stat {
    fn name(self) -> &'static str {
        match self {
            Self::Level => "level",
            Self::Hp => "hp",
            Self::PeRaw => "pe-raw",
            Self::Bp => "bp",
        }
    }
}
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct IniChange {
    pub restart_required: bool,
    pub reply: String,
}

pub struct BattleClient<T> {
    transport: T,
    directory: PathBuf,
}
impl<'a> BattleClient<&'a mut GameSession> {
    pub fn headless(session: &'a mut GameSession) -> Self {
        let directory = session.directory().join("ctl");
        Self::new(session, directory)
    }
}
impl BattleClient<AttachedTransport> {
    pub fn attached(directory: PathBuf, timeout: Duration) -> Self {
        Self::new(
            AttachedTransport::new(directory.clone(), timeout),
            directory,
        )
    }
}
impl<T: DebugTransport> BattleClient<T> {
    pub fn new(transport: T, directory: PathBuf) -> Self {
        Self {
            transport,
            directory,
        }
    }
    fn request(&mut self, text: &str) -> Result<Reply, SessionError> {
        let command = Command::new(text)?;
        let reply = self.transport.execute(text)?;
        if reply.verb != command.verb() {
            return Err(ProtocolError(format!("{text}: received reply for {}", reply.verb)).into());
        }
        if reply.status == Status::Err {
            return Err(SessionError::Native {
                command: text.into(),
                reply: reply.line,
            });
        }
        Ok(reply)
    }
    pub fn state(&mut self) -> Result<BattleState, SessionError> {
        Ok(self.request("battle-state")?.details.parse()?)
    }
    pub fn rng_get(&mut self) -> Result<RngSnapshot, SessionError> {
        Ok(self.request("rng-get")?.details.parse()?)
    }
    pub fn rng_set(&mut self, snapshot: &RngSnapshot) -> Result<(), SessionError> {
        let text = snapshot.to_string();
        let _: RngSnapshot = text.parse()?;
        self.request(&format!("rng-set {text}"))?;
        Ok(())
    }
    pub fn trace(&mut self, enabled: bool) -> Result<(), SessionError> {
        self.request(if enabled { "trace on" } else { "trace off" })?;
        Ok(())
    }
    pub fn events(&self) -> Result<Vec<BattleEvent>, SessionError> {
        read_events(&self.directory.join("battle.jsonl"))
    }
    pub fn give(&mut self, item: u32) -> Result<Reply, SessionError> {
        self.request(&format!("give {item:X}"))
    }
    pub fn equip(&mut self, bag_slot: i32) -> Result<Reply, SessionError> {
        self.request(&format!("equip {bag_slot}"))
    }
    pub fn set_stat(&mut self, stat: Stat, value: i32) -> Result<Reply, SessionError> {
        self.request(&format!("set-stat {} {value}", stat.name()))
    }
    pub fn mod_state(&mut self, name: &str) -> Result<ModState, SessionError> {
        Ok(self
            .request(&format!("mod-state {name}"))?
            .details
            .parse()?)
    }
    pub fn ini_set(&mut self, key: &str, value: i32) -> Result<IniChange, SessionError> {
        let reply = self.request(&format!("ini-set {key} {value}"))?;
        Ok(IniChange {
            restart_required: reply.restart_required(),
            reply: reply.line,
        })
    }
}

/// Same missing-file/strict-line semantics as Python events(), with path/line errors.
pub fn read_events(path: &Path) -> Result<Vec<BattleEvent>, SessionError> {
    let file = match File::open(path) {
        Ok(file) => file,
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => return Ok(vec![]),
        Err(source) => {
            return Err(SessionError::Io {
                path: path.into(),
                source,
            })
        }
    };
    BufReader::new(file)
        .lines()
        .enumerate()
        .map(|(i, line)| {
            let line = line.map_err(|source| SessionError::Io {
                path: path.into(),
                source,
            })?;
            line.parse::<BattleEvent>()
                .map_err(|e| ProtocolError(format!("{}:{}: {e}", path.display(), i + 1)).into())
        })
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::{
        collections::VecDeque,
        fs,
        sync::{
            atomic::{AtomicU64, Ordering},
            Arc, Mutex,
        },
    };
    type Exchange = (String, String);
    struct Mock {
        exchanges: VecDeque<Exchange>,
        sent: Arc<Mutex<Vec<String>>>,
    }
    impl DebugTransport for Mock {
        fn execute(&mut self, text: &str) -> Result<Reply, SessionError> {
            self.sent.lock().unwrap().push(text.into());
            let (expected, reply) = self.exchanges.pop_front().expect("unexpected command");
            assert_eq!(text, expected);
            Ok(Reply::matching(&Command::new(text).unwrap(), &reply).unwrap())
        }
    }
    #[test]
    fn every_python_helper_preserves_command_spelling_and_payload() {
        let hex = "a5".repeat(521);
        let snapshot = format!("4294967295 7 -1 {hex}");
        let state = include_str!("../../pe-protocol/fixtures/battle-state.json").trim();
        let mod_state = include_str!("../../pe-protocol/fixtures/mod-state.json").trim();
        let exchanges = VecDeque::from([
            ("battle-state".into(), format!("ok battle-state {state}")),
            ("rng-get".into(), format!("ok rng-get {snapshot}")),
            (
                format!("rng-set {snapshot}"),
                format!("ok rng-set {snapshot}"),
            ),
            ("trace on".into(), "ok trace on".into()),
            ("trace off".into(), "ok trace off".into()),
            ("give FF".into(), "ok give item=255".into()),
            ("equip -1".into(), "err equip bad slot".into()),
            ("equip 0".into(), "ok equip slot=0".into()),
            ("set-stat level 7".into(), "ok set-stat level=7".into()),
            ("set-stat hp 35".into(), "ok set-stat hp=35".into()),
            (
                "set-stat pe-raw 65536".into(),
                "ok set-stat pe-raw=65536".into(),
            ),
            ("set-stat bp 99999".into(), "ok set-stat bp=99999".into()),
            (
                "mod-state example".into(),
                format!("ok mod-state {mod_state}"),
            ),
            (
                "ini-set Manage mods/All mods 0".into(),
                "ok ini-set Manage mods/All mods=0 restart_required=1".into(),
            ),
            (
                "ini-set Example/Option 1".into(),
                "ok ini-set Example/Option=1 restart_required=0".into(),
            ),
        ]);
        let total = exchanges.len();
        let sent = Arc::new(Mutex::new(vec![]));
        let mut client = BattleClient::new(
            Mock {
                exchanges,
                sent: sent.clone(),
            },
            PathBuf::from("unused"),
        );
        assert_eq!(client.state().unwrap().target_index.0, 1);
        let rng = client.rng_get().unwrap();
        assert_eq!(rng.table, [0xa5; 521]);
        client.rng_set(&rng).unwrap();
        client.trace(true).unwrap();
        client.trace(false).unwrap();
        assert_eq!(client.give(255).unwrap().details, "item=255");
        assert!(client
            .equip(-1)
            .unwrap_err()
            .to_string()
            .contains("equip -1: err equip bad slot"));
        client.equip(0).unwrap();
        for (stat, value) in [
            (Stat::Level, 7),
            (Stat::Hp, 35),
            (Stat::PeRaw, 65536),
            (Stat::Bp, 99999),
        ] {
            client.set_stat(stat, value).unwrap();
        }
        assert!(!client.mod_state("example").unwrap().running);
        assert!(
            client
                .ini_set("Manage mods/All mods", 0)
                .unwrap()
                .restart_required
        );
        assert!(
            !client
                .ini_set("Example/Option", 1)
                .unwrap()
                .restart_required
        );
        assert_eq!(sent.lock().unwrap().len(), total);
        assert!(client.transport.exchanges.is_empty());
    }
    #[test]
    fn invalid_snapshot_and_multiline_argument_never_reach_transport() {
        let sent = Arc::new(Mutex::new(vec![]));
        let mut client = BattleClient::new(
            Mock {
                exchanges: VecDeque::new(),
                sent: sent.clone(),
            },
            PathBuf::from("unused"),
        );
        assert!(client
            .rng_set(&RngSnapshot {
                state: 1,
                frame: 0,
                index: 521,
                table: [0; 521]
            })
            .is_err());
        assert!(client.mod_state("example\nquit").is_err());
        assert!(client.ini_set("Page/Key\rquit", 1).is_err());
        assert!(sent.lock().unwrap().is_empty());
    }
    static NEXT: AtomicU64 = AtomicU64::new(0);
    struct Temp(PathBuf);
    impl Temp {
        fn new() -> Self {
            let path = std::env::temp_dir().join(format!(
                "pe-events-{}-{}-{}",
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
    fn trace_file_missing_valid_corrupt_and_partial_behave_explicitly() {
        let temp = Temp::new();
        let path = temp.0.join("battle.jsonl");
        assert!(read_events(&path).unwrap().is_empty());
        fs::write(
            &path,
            include_str!("../../pe-protocol/fixtures/battle-events.jsonl"),
        )
        .unwrap();
        assert_eq!(read_events(&path).unwrap().len(), 9);
        fs::write(&path, "{\"event\":\"spell\",\"frame\":1}\n{broken").unwrap();
        let error = read_events(&path).unwrap_err().to_string();
        assert!(error.contains("battle.jsonl:2:"));
        fs::write(&path, "\n").unwrap();
        assert!(read_events(&path).is_err());
        fs::write(&path, "{\"event\":\"spell\",\"frame\":1}").unwrap();
        assert_eq!(read_events(&path).unwrap().len(), 1);
    }
}
