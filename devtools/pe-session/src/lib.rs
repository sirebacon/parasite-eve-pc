//! File channel for an existing game. Headless callers supply frame advancement.
use pe_protocol::{Command, ProtocolError, Reply, Status};
pub mod process;
use std::{
    error::Error,
    fmt,
    fs::{self, OpenOptions},
    io,
    path::{Path, PathBuf},
    thread,
    time::{Duration, Instant},
};

#[derive(Debug)]
pub enum SessionError {
    Protocol(ProtocolError),
    Io {
        path: PathBuf,
        source: io::Error,
    },
    Busy(PathBuf),
    Native {
        command: String,
        reply: String,
    },
    Timeout {
        command: String,
        directory: PathBuf,
        budget: String,
    },
    Advance(String),
    Process {
        directory: PathBuf,
        message: String,
    },
}
impl fmt::Display for SessionError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Protocol(e) => e.fmt(f),
            Self::Io { path, source } => write!(f, "{}: {source}", path.display()),
            Self::Busy(path) => write!(
                f,
                "debug channel is busy: {} (another command/client may be pending)",
                path.display()
            ),
            Self::Native { command, reply } => write!(f, "{command}: {reply}"),
            Self::Timeout {
                command,
                directory,
                budget,
            } => write!(
                f,
                "no answer to {command:?} after {budget}; channel {} (command may still execute)",
                directory.display()
            ),
            Self::Advance(e) => write!(f, "advancing game: {e}"),
            Self::Process { directory, message } => write!(
                f,
                "{message}; session {} (see stdout.log and stderr.txt)",
                directory.display()
            ),
        }
    }
}
impl Error for SessionError {
    fn source(&self) -> Option<&(dyn Error + 'static)> {
        match self {
            Self::Protocol(e) => Some(e),
            Self::Io { source, .. } => Some(source),
            _ => None,
        }
    }
}
impl From<ProtocolError> for SessionError {
    fn from(e: ProtocolError) -> Self {
        Self::Protocol(e)
    }
}
fn io_at<T>(path: &Path, result: io::Result<T>) -> Result<T, SessionError> {
    result.map_err(|source| SessionError::Io {
        path: path.into(),
        source,
    })
}

pub struct DebugControl {
    directory: PathBuf,
}
impl DebugControl {
    pub fn new(directory: impl Into<PathBuf>) -> Self {
        Self {
            directory: directory.into(),
        }
    }

    /// For an already running, unpaused game. This does not advance a headless game.
    pub fn ask(&self, text: &str, timeout: Duration) -> Result<Reply, SessionError> {
        let start = Instant::now();
        self.exchange(
            text,
            || {
                if start.elapsed() >= timeout {
                    return Ok(false);
                }
                thread::sleep(
                    Duration::from_millis(50).min(timeout.saturating_sub(start.elapsed())),
                );
                Ok(true)
            },
            format!("{} ms", timeout.as_millis()),
        )
    }

    /// Mirrors Session.command: poll, then advance 10 frames until the frame budget.
    /// The callback must enforce its own wall-clock/process timeout (Python uses 60s).
    pub fn ask_with_frames<F>(
        &self,
        text: &str,
        timeout_frames: u32,
        mut advance: F,
    ) -> Result<Reply, SessionError>
    where
        F: FnMut(u32) -> Result<(), SessionError>,
    {
        let mut waited = 0u64;
        self.exchange(
            text,
            || {
                if waited >= u64::from(timeout_frames) {
                    return Ok(false);
                }
                advance(10)?;
                waited += 10;
                Ok(true)
            },
            format!("{timeout_frames} frames"),
        )
    }

    fn exchange<F>(&self, text: &str, mut wait: F, budget: String) -> Result<Reply, SessionError>
    where
        F: FnMut() -> Result<bool, SessionError>,
    {
        let command = Command::new(text)?;
        // A cooperating-client lock avoids replacing someone else's pending command.
        // Drop removes only our lock; pending cmd is retained after a timeout.
        let lock_path = self.directory.join("client.lock");
        let lock_file = match OpenOptions::new()
            .write(true)
            .create_new(true)
            .open(&lock_path)
        {
            Ok(file) => file,
            Err(e) if e.kind() == io::ErrorKind::AlreadyExists => {
                return Err(SessionError::Busy(lock_path))
            }
            Err(source) => {
                return Err(SessionError::Io {
                    path: lock_path,
                    source,
                })
            }
        };
        let _lock = ChannelLock {
            path: lock_path,
            file: Some(lock_file),
        };
        let pending = self.directory.join("cmd");
        if io_at(&pending, pending.try_exists())? {
            return Err(SessionError::Busy(pending));
        }
        let out = self.directory.join("out");
        let baseline = read_optional(&out)?;
        let mut seen = baseline.lines().count();
        let tmp = self.directory.join("cmd.tmp");
        io_at(&tmp, fs::write(&tmp, format!("{}\n", command.text())))?;
        io_at(&pending, fs::rename(&tmp, &pending))?;
        loop {
            let output = read_optional(&out)?;
            // Do not consume a partially appended line: the next read must revisit it.
            let complete = output.rfind('\n').map_or("", |i| &output[..=i]);
            let lines: Vec<_> = complete.lines().collect();
            if lines.len() < seen {
                seen = 0;
            } // log truncation/restart
            for line in &lines[seen..] {
                if let Some(reply) = Reply::matching(&command, line) {
                    return if reply.status == Status::Ok {
                        Ok(reply)
                    } else {
                        Err(SessionError::Native {
                            command: command.text().into(),
                            reply: reply.line,
                        })
                    };
                }
            }
            seen = lines.len();
            if !wait()? {
                return Err(SessionError::Timeout {
                    command: command.text().into(),
                    directory: self.directory.clone(),
                    budget,
                });
            }
        }
    }
}
fn read_optional(path: &Path) -> Result<String, SessionError> {
    match fs::read_to_string(path) {
        Ok(text) => Ok(text),
        Err(e) if e.kind() == io::ErrorKind::NotFound => Ok(String::new()),
        Err(source) => Err(SessionError::Io {
            path: path.into(),
            source,
        }),
    }
}
struct ChannelLock {
    path: PathBuf,
    file: Option<fs::File>,
}
impl Drop for ChannelLock {
    fn drop(&mut self) {
        drop(self.file.take());
        let _ = fs::remove_file(&self.path);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::atomic::{AtomicU64, Ordering};
    static NEXT: AtomicU64 = AtomicU64::new(0);
    struct Temp(PathBuf);
    impl Temp {
        fn new() -> Self {
            let path = std::env::temp_dir().join(format!(
                "pe-rust-test-{}-{}-{}",
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
        fn ctl(&self) -> DebugControl {
            DebugControl::new(&self.0)
        }
    }
    impl Drop for Temp {
        fn drop(&mut self) {
            let _ = fs::remove_dir_all(&self.0);
        }
    }
    #[test]
    fn ignores_old_and_prefix_replies_and_sends_exact_command() {
        let dir = Temp::new();
        fs::write(dir.0.join("out"), "ok battle stale\n").unwrap();
        let mut calls = vec![];
        let reply = dir
            .ctl()
            .ask_with_frames("battle 1", 20, |frames| {
                calls.push(frames);
                assert_eq!(fs::read_to_string(dir.0.join("cmd")).unwrap(), "battle 1\n");
                assert!(!dir.0.join("cmd.tmp").exists());
                fs::remove_file(dir.0.join("cmd")).unwrap();
                fs::write(
                    dir.0.join("out"),
                    "ok battle stale\nok battle-state wrong command\nok battle chosen\n",
                )
                .unwrap();
                Ok(())
            })
            .unwrap();
        assert_eq!(reply.line, "ok battle chosen");
        assert_eq!(calls, [10]);
        assert!(!dir.0.join("client.lock").exists());
    }
    #[test]
    fn error_includes_original_command_and_native_reason() {
        let dir = Temp::new();
        let err = dir
            .ctl()
            .ask_with_frames("equip 0", 10, |_| {
                fs::write(dir.0.join("out"), "err equip not in free field\n").unwrap();
                Ok(())
            })
            .unwrap_err()
            .to_string();
        assert!(err.contains("equip 0: err equip not in free field"));
        assert!(!dir.0.join("client.lock").exists());
    }
    #[test]
    fn incomplete_reply_is_revisited() {
        let dir = Temp::new();
        let mut count = 0;
        let reply = dir
            .ctl()
            .ask_with_frames("ping", 20, |_| {
                count += 1;
                fs::write(
                    dir.0.join("out"),
                    if count == 1 {
                        "ok ping 1"
                    } else {
                        "ok ping 123\n"
                    },
                )
                .unwrap();
                Ok(())
            })
            .unwrap();
        assert_eq!(count, 2);
        assert_eq!(reply.details, "123");
    }
    #[test]
    fn frame_timeout_is_bounded_and_keeps_pending_command() {
        let dir = Temp::new();
        let mut frames = 0;
        let err = dir
            .ctl()
            .ask_with_frames("ping", 20, |n| {
                frames += n;
                Ok(())
            })
            .unwrap_err();
        assert_eq!(frames, 20);
        assert!(matches!(err, SessionError::Timeout { .. }));
        assert!(dir.0.join("cmd").exists());
        assert!(!dir.0.join("client.lock").exists());
        assert!(matches!(
            dir.ctl().ask("ping", Duration::ZERO),
            Err(SessionError::Busy(_))
        ));
    }
    #[test]
    fn invalid_commands_never_touch_channel() {
        let dir = Temp::new();
        for text in ["", "ping\nping"] {
            assert!(matches!(
                dir.ctl().ask(text, Duration::ZERO),
                Err(SessionError::Protocol(_))
            ));
        }
        assert_eq!(fs::read_dir(&dir.0).unwrap().count(), 0);
    }
    #[test]
    fn concurrent_client_lock_is_respected() {
        let dir = Temp::new();
        fs::write(dir.0.join("client.lock"), "other client").unwrap();
        assert!(matches!(
            dir.ctl().ask("ping", Duration::ZERO),
            Err(SessionError::Busy(_))
        ));
        assert_eq!(
            fs::read_to_string(dir.0.join("client.lock")).unwrap(),
            "other client"
        );
    }
    #[test]
    fn advance_failure_releases_lock() {
        let dir = Temp::new();
        assert!(matches!(
            dir.ctl()
                .ask_with_frames("ping", 10, |_| Err(SessionError::Advance(
                    "game exited".into()
                ))),
            Err(SessionError::Advance(_))
        ));
        assert!(!dir.0.join("client.lock").exists());
    }
    #[test]
    fn zero_wall_budget_times_out() {
        let dir = Temp::new();
        assert!(matches!(
            dir.ctl().ask("ping", Duration::ZERO),
            Err(SessionError::Timeout { .. })
        ));
    }
}
