//! Isolated native game lifecycle and the stdin/VBlank control loop.
use crate::{io_at, DebugControl, SessionError};
use pe_protocol::{
    control::{ControlEvent, ControlState},
    Reply,
};
use std::{
    collections::{BTreeMap, VecDeque},
    ffi::OsStr,
    fs::{self, File, OpenOptions},
    io::{BufRead, BufReader, Read, Write},
    path::{Component, Path, PathBuf},
    process::{Child, ChildStdin, Command, Stdio},
    sync::mpsc::{self, Receiver},
    thread::{self, JoinHandle},
    time::{Duration, Instant},
};

pub struct LaunchOptions {
    pub executable: PathBuf,
    pub disc1: PathBuf,
    pub disc2: PathBuf,
    /// Must not already exist; existing user directories are never deleted.
    pub run_dir: PathBuf,
    pub cards: [Vec<PathBuf>; 2],
    pub mods_ini: Option<String>,
    pub extra_files: BTreeMap<PathBuf, Vec<u8>>,
    pub frame_limit: u64,
    pub response_timeout: Duration,
    pub shutdown_timeout: Duration,
}
impl LaunchOptions {
    pub fn new(executable: PathBuf, disc1: PathBuf, disc2: PathBuf, run_dir: PathBuf) -> Self {
        Self {
            executable,
            disc1,
            disc2,
            run_dir,
            cards: [vec![], vec![]],
            mods_ini: Some("Manage mods/All mods=1\n".into()),
            extra_files: BTreeMap::new(),
            frame_limit: 2_000_000,
            response_timeout: Duration::from_secs(60),
            shutdown_timeout: Duration::from_secs(10),
        }
    }
    fn command(&self) -> Result<Command, SessionError> {
        let exe = native_path(io_at(&self.executable, fs::canonicalize(&self.executable))?);
        let disc1 = native_path(io_at(&self.disc1, fs::canonicalize(&self.disc1))?);
        let disc2 = native_path(io_at(&self.disc2, fs::canonicalize(&self.disc2))?);
        let mut command = Command::new(exe);
        command
            .arg("--disc")
            .arg(disc1)
            .arg("--disc2")
            .arg(disc2)
            .arg("--headless")
            .arg("--frame-limit")
            .arg(self.frame_limit.to_string())
            .args([
                "--control-at",
                "1",
                "--dump-frame",
                "frame.ppm",
                "--pad-at",
                "1:0",
            ]);
        Ok(command)
    }
}

enum Output {
    Line(String),
    Error(String),
    End,
}
pub struct GameSession {
    directory: PathBuf,
    child: Child,
    stdin: Option<ChildStdin>,
    lines: Option<Receiver<Output>>,
    reader: Option<JoinHandle<()>>,
    state: Option<ControlState>,
    history: VecDeque<ControlState>,
    response_timeout: Duration,
    shutdown_timeout: Duration,
    closed: bool,
}
impl GameSession {
    pub fn launch(options: LaunchOptions) -> Result<Self, SessionError> {
        let command = options.command()?;
        Self::start(options, command)
    }
    fn start(options: LaunchOptions, mut command: Command) -> Result<Self, SessionError> {
        // Use a fresh directory; never recursively delete or reuse user data.
        if options.frame_limit == 0 || options.response_timeout.is_zero() {
            return Err(SessionError::Process {
                directory: options.run_dir,
                message: "frame limit and response timeout must be positive".into(),
            });
        }
        io_at(&options.run_dir, fs::create_dir(&options.run_dir))?;
        let directory = native_path(io_at(&options.run_dir, fs::canonicalize(&options.run_dir))?);
        for (port, cards) in ["bu00", "bu10"].into_iter().zip(&options.cards) {
            let target = directory.join(port);
            io_at(&target, fs::create_dir(&target))?;
            for card in cards {
                copy_card(card, &target)?;
            }
        }
        if let Some(ini) = &options.mods_ini {
            write_new(&directory.join("pe_mods.ini"), ini.as_bytes())?;
        }
        for (name, bytes) in &options.extra_files {
            if name.as_os_str().is_empty()
                || !name.components().all(|c| matches!(c, Component::Normal(_)))
            {
                return Err(SessionError::Process {
                    directory,
                    message: format!(
                        "extra file must be a relative path without traversal: {}",
                        name.display()
                    ),
                });
            }
            let path = directory.join(name);
            if let Some(parent) = path.parent() {
                io_at(parent, fs::create_dir_all(parent))?;
            }
            write_new(&path, bytes)?;
        }
        let ctl = directory.join("ctl");
        io_at(&ctl, fs::create_dir(&ctl))?;
        let stderr_path = directory.join("stderr.txt");
        let stderr = io_at(&stderr_path, File::create(&stderr_path))?;
        let stdout_path = directory.join("stdout.log");
        let mut stdout_log = io_at(&stdout_path, File::create(&stdout_path))?;
        command
            .current_dir(&directory)
            .env("PE_CONFIG_HOME", &directory)
            .env("PE_DEBUG_CONTROL", &ctl)
            .env("PSYZ_AUDIO_CLOCK", "video")
            .env("SDL_AUDIODRIVER", "dummy")
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .stderr(stderr);
        let mut child = command.spawn().map_err(|e| SessionError::Process {
            directory: directory.clone(),
            message: format!("launch failed: {e}"),
        })?;
        let stdin = child.stdin.take();
        let stdout = child.stdout.take().expect("piped stdout");
        let (sender, lines) = mpsc::sync_channel(64);
        let reader = thread::spawn(move || {
            let mut reader = BufReader::new(stdout);
            loop {
                let mut bytes = Vec::new();
                // Bound both channel length and individual lines, even for a broken host.
                match (&mut reader).take(1_048_577).read_until(b'\n', &mut bytes) {
                    Ok(0) => {
                        let _ = sender.send(Output::End);
                        break;
                    }
                    Ok(_) => {}
                    Err(e) => {
                        let _ = sender.send(Output::Error(e.to_string()));
                        break;
                    }
                }
                if let Err(e) = stdout_log.write_all(&bytes) {
                    let _ = sender.send(Output::Error(format!("stdout log: {e}")));
                    break;
                }
                if bytes.len() > 1_048_576 {
                    let _ = sender.send(Output::Error("stdout line exceeds 1 MiB".into()));
                    break;
                }
                let message = match String::from_utf8(bytes) {
                    Ok(line) => Output::Line(line),
                    Err(e) => Output::Error(format!("stdout is not UTF-8: {e}")),
                };
                if sender.send(message).is_err() {
                    break;
                }
            }
        });
        let mut session = Self {
            directory,
            child,
            stdin,
            lines: Some(lines),
            reader: Some(reader),
            state: None,
            history: VecDeque::new(),
            response_timeout: options.response_timeout,
            shutdown_timeout: options.shutdown_timeout,
            closed: false,
        };
        session.read_state()?; // Drop cleans up the process even when startup fails.
        Ok(session)
    }
    pub fn directory(&self) -> &Path {
        &self.directory
    }
    pub fn state(&self) -> &ControlState {
        self.state.as_ref().expect("startup reads initial state")
    }
    pub fn history(&self) -> &VecDeque<ControlState> {
        &self.history
    }
    pub fn process_id(&self) -> u32 {
        self.child.id()
    }
    pub fn frame_ppm(&self) -> PathBuf {
        self.directory.join("frame.ppm")
    }
    fn failure(&self, message: impl Into<String>) -> SessionError {
        SessionError::Process {
            directory: self.directory.clone(),
            message: message.into(),
        }
    }
    fn response_failure(&self) -> SessionError {
        self.failure(format!(
            "no control response for {} ms; last state: {}",
            self.response_timeout.as_millis(),
            self.state
                .as_ref()
                .map_or("startup".into(), ControlState::short)
        ))
    }
    fn read_state(&mut self) -> Result<(), SessionError> {
        let start = Instant::now();
        loop {
            let left = self.response_timeout.saturating_sub(start.elapsed());
            if left.is_zero() {
                return Err(self.response_failure());
            }
            let output = self
                .lines
                .as_ref()
                .expect("active reader")
                .recv_timeout(left)
                .map_err(|e| match e {
                    mpsc::RecvTimeoutError::Timeout => self.response_failure(),
                    _ => self.failure(format!("control reader: {e}")),
                })?;
            match output {
                Output::Line(line) => {
                    if let Some(state) =
                        ControlState::from_line(&line).map_err(|e| self.failure(e.to_string()))?
                    {
                        self.history.push_back(state.clone());
                        if self.history.len() > 2000 {
                            self.history.pop_front();
                        }
                        self.state = Some(state);
                        return Ok(());
                    }
                }
                Output::Error(e) => return Err(self.failure(format!("stdout reader: {e}"))),
                Output::End => {
                    let exit_deadline = Instant::now();
                    let status = loop {
                        let status = self
                            .child
                            .try_wait()
                            .map_err(|e| self.failure(e.to_string()))?;
                        if status.is_some() || exit_deadline.elapsed() >= Duration::from_millis(50)
                        {
                            break status;
                        }
                        thread::sleep(Duration::from_millis(1));
                    };
                    return Err(self.failure(format!("game stdout closed; exit status {status:?}")));
                }
            }
        }
    }
    fn send(&mut self, text: &str) -> Result<(), SessionError> {
        let Some(stdin) = &mut self.stdin else {
            return Err(self.failure("game stdin is closed"));
        };
        stdin
            .write_all(text.as_bytes())
            .and_then(|_| stdin.flush())
            .map_err(|e| self.failure(format!("game stdin: {e}")))
    }
    pub fn advance(&mut self, frames: u32, buttons: u16) -> Result<&ControlState, SessionError> {
        if frames == 0 {
            return Err(self.failure("advance requires positive frames"));
        }
        if self.closed || self.state().event == ControlEvent::FrameLimit {
            return Err(self.failure("cannot advance a closed or frame-limited game"));
        }
        let result = self
            .send(&format!("advance {frames} {buttons}\n"))
            .and_then(|_| self.read_state());
        if let Err(e) = result {
            self.terminate();
            return Err(e);
        }
        Ok(self.state())
    }
    pub fn press(
        &mut self,
        buttons: u16,
        hold: u32,
        after: u32,
    ) -> Result<&ControlState, SessionError> {
        self.advance(hold, buttons)?;
        self.advance(after, 0)
    }
    pub fn wait_until<F>(
        &mut self,
        mut predicate: F,
        timeout: u32,
        step: u32,
        buttons: u16,
    ) -> Result<&ControlState, SessionError>
    where
        F: FnMut(&ControlState) -> bool,
    {
        if step == 0 {
            return Err(self.failure("wait step must be positive"));
        }
        let mut waited = 0;
        while !predicate(self.state()) {
            if waited >= timeout {
                return Err(self.failure(format!(
                    "wait timed out after {timeout} frames: {}",
                    self.state().short()
                )));
            }
            let count = step.min(timeout - waited);
            self.advance(count, buttons)?;
            waited += count;
        }
        Ok(self.state())
    }
    pub fn wait_stable(
        &mut self,
        frames: u32,
        timeout: u32,
    ) -> Result<&ControlState, SessionError> {
        let mut last = self.state().stable_key();
        let mut same = 0;
        let mut waited = 0;
        while same < frames {
            if waited >= timeout {
                return Err(self.failure(format!(
                    "never settled after {timeout} frames: {}",
                    self.state().short()
                )));
            }
            let count = 10.min(timeout - waited);
            self.advance(count, 0)?;
            waited += count;
            let key = self.state().stable_key();
            same = if key == last { same + count } else { 0 };
            last = key;
        }
        Ok(self.state())
    }
    pub fn command(&mut self, text: &str, timeout_frames: u32) -> Result<Reply, SessionError> {
        DebugControl::new(self.directory.join("ctl")).ask_with_frames(
            text,
            timeout_frames,
            |frames| self.advance(frames, 0).map(|_| ()),
        )
    }
    pub fn quit(&mut self) -> Result<(), SessionError> {
        if self.closed {
            return Ok(());
        }
        let _ = self.send("quit\n");
        self.stdin.take();
        let start = Instant::now();
        loop {
            match self.child.try_wait() {
                Ok(Some(_)) => {
                    self.finish_reader();
                    return Ok(());
                }
                Ok(None) if start.elapsed() < self.shutdown_timeout => {
                    thread::sleep(Duration::from_millis(10))
                }
                Ok(None) => {
                    self.terminate();
                    return Err(
                        self.failure("game did not quit before shutdown deadline; terminated")
                    );
                }
                Err(e) => {
                    self.terminate();
                    return Err(self.failure(format!("waiting for game exit: {e}")));
                }
            }
        }
    }
    fn finish_reader(&mut self) {
        self.lines.take(); // Unblock a reader waiting to send to the bounded channel.
        if let Some(reader) = self.reader.take() {
            let _ = reader.join();
        }
        self.closed = true;
    }
    fn terminate(&mut self) {
        if !self.closed {
            self.stdin.take();
            let _ = self.child.kill();
            let _ = self.child.wait();
            self.finish_reader();
        }
    }
}
impl Drop for GameSession {
    fn drop(&mut self) {
        if !self.closed {
            let _ = self.quit();
        }
    }
}

// The C host appends '/filename'. Windows verbatim paths from canonicalize forbid
// those separators, so pass conventional absolute paths across the native boundary.
fn native_path(path: PathBuf) -> PathBuf {
    #[cfg(windows)]
    {
        use std::{
            ffi::OsString,
            os::windows::ffi::{OsStrExt, OsStringExt},
            path::Prefix,
        };
        if let Some(Component::Prefix(prefix)) = path.components().next() {
            match prefix.kind() {
                Prefix::VerbatimDisk(_) => {
                    return OsString::from_wide(
                        &path.as_os_str().encode_wide().skip(4).collect::<Vec<_>>(),
                    )
                    .into()
                }
                Prefix::VerbatimUNC(_, _) => {
                    let wide: Vec<_> = "\\\\"
                        .encode_utf16()
                        .chain(path.as_os_str().encode_wide().skip(8))
                        .collect();
                    return OsString::from_wide(&wide).into();
                }
                _ => {}
            }
        }
    }
    path
}

fn write_new(path: &Path, bytes: &[u8]) -> Result<(), SessionError> {
    let mut file = io_at(
        path,
        OpenOptions::new().write(true).create_new(true).open(path),
    )?;
    io_at(path, file.write_all(bytes))
}
fn copy_card(card: &Path, target: &Path) -> Result<(), SessionError> {
    let name = card
        .file_name()
        .and_then(OsStr::to_str)
        .ok_or_else(|| SessionError::Process {
            directory: target.into(),
            message: format!("invalid card filename: {}", card.display()),
        })?;
    write_new(&target.join(name), &io_at(card, fs::read(card))?)?;
    let companions = card.parent().unwrap_or(Path::new(".")).join(".pe-ext");
    match fs::read_dir(&companions) {
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => {}
        Err(source) => {
            return Err(SessionError::Io {
                path: companions,
                source,
            })
        }
        Ok(entries) => {
            for entry in entries {
                let path = io_at(&companions, entry)?.path();
                let matches = path
                    .file_name()
                    .and_then(OsStr::to_str)
                    .is_some_and(|n| n.starts_with(&format!("{name}.")) && n.ends_with(".pex"));
                if matches {
                    let dir = target.join(".pe-ext");
                    io_at(&dir, fs::create_dir_all(&dir))?;
                    write_new(
                        &dir.join(path.file_name().expect("entry filename")),
                        &io_at(&path, fs::read(&path))?,
                    )?;
                }
            }
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use pe_protocol::control::buttons;
    use std::sync::atomic::{AtomicU64, Ordering};
    static NEXT_DIRECTORY: AtomicU64 = AtomicU64::new(0);
    struct Temp(PathBuf);
    impl Temp {
        fn new() -> Self {
            let root = std::env::temp_dir().join(format!(
                "pe-process-{}-{}-{}",
                std::process::id(),
                std::time::SystemTime::now()
                    .duration_since(std::time::UNIX_EPOCH)
                    .unwrap()
                    .as_nanos(),
                NEXT_DIRECTORY.fetch_add(1, Ordering::Relaxed)
            ));
            fs::create_dir(&root).unwrap();
            Self(root)
        }
        fn options(&self) -> LaunchOptions {
            let mut options = LaunchOptions::new(
                std::env::current_exe().unwrap(),
                self.0.join("disc1.cue"),
                self.0.join("disc2.cue"),
                self.0.join("run"),
            );
            options.response_timeout = Duration::from_secs(3);
            options.shutdown_timeout = Duration::from_millis(100);
            options
        }
        fn start(&self, options: LaunchOptions, mode: &str) -> Result<GameSession, SessionError> {
            let mut command = Command::new(std::env::current_exe().unwrap());
            command
                .args([
                    "--ignored",
                    "--exact",
                    "process::tests::fixture_child",
                    "--nocapture",
                    "--quiet",
                ])
                .env("PE_SESSION_FIXTURE", mode);
            GameSession::start(options, command)
        }
    }
    impl Drop for Temp {
        fn drop(&mut self) {
            let _ = fs::remove_dir_all(&self.0);
        }
    }

    // Invoked only as a subprocess; not an asset-dependent test skipped by CI.
    #[test]
    #[ignore = "subprocess fixture entry point"]
    fn fixture_child() {
        let mode = std::env::var("PE_SESSION_FIXTURE").expect("fixture requires mode");
        let cwd = std::env::current_dir().unwrap();
        assert_eq!(
            fs::canonicalize(PathBuf::from(std::env::var_os("PE_CONFIG_HOME").unwrap())).unwrap(),
            fs::canonicalize(&cwd).unwrap()
        );
        assert_eq!(
            fs::canonicalize(PathBuf::from(std::env::var_os("PE_DEBUG_CONTROL").unwrap())).unwrap(),
            fs::canonicalize(cwd.join("ctl")).unwrap()
        );
        assert_eq!(std::env::var("SDL_AUDIODRIVER").unwrap(), "dummy");
        assert_eq!(std::env::var("PSYZ_AUDIO_CLOCK").unwrap(), "video");
        match mode.as_str() {
            "exit" => std::process::exit(7),
            "hang-start" => loop {
                thread::sleep(Duration::from_secs(1));
            },
            "chatty" => loop {
                println!("boot diagnostic");
                thread::sleep(Duration::from_millis(1));
            },
            "malformed" => {
                println!("\n{{\"event\":\"control\"}}");
                return;
            }
            "oversized" => {
                println!("\n{}", "x".repeat(1_048_577));
                return;
            }
            _ => {}
        }
        let state = |frame: u32, pad: u16, event: &str| {
            println!("\n{{\"event\":\"{event}\",\"vblanks\":{frame},\"scene\":3,\"flags\":0,\"game_flags\":0,\"pad\":{pad},\"menu\":null}}");
            std::io::stdout().flush().unwrap();
        };
        let mut frame = 1;
        state(frame, 0, "control");
        let mut received = File::create("received.txt").unwrap();
        for line in std::io::stdin().lock().lines() {
            let line = line.unwrap();
            writeln!(received, "{line}").unwrap();
            received.flush().unwrap();
            if line == "quit" {
                if mode == "no-quit" {
                    loop {
                        thread::sleep(Duration::from_secs(1));
                    }
                }
                return;
            }
            if mode == "hang-advance" {
                loop {
                    thread::sleep(Duration::from_secs(1));
                }
            }
            let words: Vec<_> = line.split_whitespace().collect();
            assert_eq!(words[0], "advance");
            frame += words[1].parse::<u32>().unwrap();
            let pad = words[2].parse::<u16>().unwrap();
            if let Ok(command) = fs::read_to_string("ctl/cmd") {
                fs::remove_file("ctl/cmd").unwrap();
                let mut out = OpenOptions::new()
                    .append(true)
                    .create(true)
                    .open("ctl/out")
                    .unwrap();
                writeln!(out, "ok {} 123", command.split_whitespace().next().unwrap()).unwrap();
            }
            state(
                frame,
                pad,
                if mode == "terminal" {
                    "frame_limit"
                } else {
                    "control"
                },
            );
        }
    }
    #[test]
    #[cfg(windows)]
    fn windows_native_paths_allow_host_separator_appends() {
        assert_eq!(
            native_path(PathBuf::from(r"\\?\D:\pe 测试\run")),
            PathBuf::from(r"D:\pe 测试\run")
        );
        assert_eq!(
            native_path(PathBuf::from(r"\\?\UNC\server\share\run")),
            PathBuf::from(r"\\server\share\run")
        );
        assert_eq!(
            native_path(PathBuf::from(r"D:\run")),
            PathBuf::from(r"D:\run")
        );
    }
    #[test]
    fn launch_input_wait_commands_and_graceful_quit() {
        let temp = Temp::new();
        let options = temp.options();
        let mut session = temp.start(options, "normal").unwrap();
        assert_eq!(session.state().vblanks, 1);
        assert!(session.process_id() > 0);
        session.press(buttons::CROSS, 10, 20).unwrap();
        assert_eq!(session.history()[1].raw["pad"], buttons::CROSS);
        assert_eq!(session.state().raw["pad"], 0);
        assert_eq!(session.command("ping", 20).unwrap().line, "ok ping 123");
        session.wait_until(|s| s.vblanks >= 51, 10, 10, 0).unwrap();
        session.wait_stable(20, 20).unwrap();
        assert_eq!(session.state().vblanks, 71);
        session.quit().unwrap();
        session.quit().unwrap();
        assert!(session.child.try_wait().unwrap().is_some());
        assert_eq!(fs::read_to_string(session.directory().join("received.txt")).unwrap(), "advance 10 16384\nadvance 20 0\nadvance 10 0\nadvance 10 0\nadvance 10 0\nadvance 10 0\nquit\n");
    }
    #[test]
    fn copies_cards_and_companions_without_changing_originals() {
        let temp = Temp::new();
        let source = temp.0.join("source");
        fs::create_dir(&source).unwrap();
        fs::create_dir(source.join(".pe-ext")).unwrap();
        let card = source.join("BASLUS-test");
        fs::write(&card, "original card").unwrap();
        fs::write(source.join(".pe-ext/BASLUS-test.00000004.pex"), "companion").unwrap();
        fs::write(source.join(".pe-ext/unrelated.pex"), "unrelated").unwrap();
        let mut options = temp.options();
        options.cards[0].push(card.clone());
        options
            .extra_files
            .insert("lang/example.pelang".into(), b"synthetic".to_vec());
        let mut session = temp.start(options, "normal").unwrap();
        assert_eq!(
            fs::read_to_string(
                session
                    .directory()
                    .join("bu00/.pe-ext/BASLUS-test.00000004.pex")
            )
            .unwrap(),
            "companion"
        );
        assert!(!session
            .directory()
            .join("bu00/.pe-ext/unrelated.pex")
            .exists());
        fs::write(session.directory().join("bu00/BASLUS-test"), "changed copy").unwrap();
        assert_eq!(fs::read_to_string(card).unwrap(), "original card");
        assert_eq!(
            fs::read(session.directory().join("lang/example.pelang")).unwrap(),
            b"synthetic"
        );
        session.quit().unwrap();
    }
    #[test]
    fn refuses_existing_directory_and_traversal() {
        let temp = Temp::new();
        let options = temp.options();
        fs::create_dir(&options.run_dir).unwrap();
        fs::write(options.run_dir.join("sentinel"), "keep").unwrap();
        assert!(temp.start(options, "normal").is_err());
        assert_eq!(
            fs::read_to_string(temp.0.join("run/sentinel")).unwrap(),
            "keep"
        );
        let mut options = temp.options();
        options.run_dir = temp.0.join("other");
        options.extra_files.insert("../escape".into(), vec![1]);
        assert!(temp.start(options, "normal").is_err());
        assert!(!temp.0.join("escape").exists());
    }
    #[test]
    fn native_launch_arguments_preserve_control_and_pad_schedule() {
        let temp = Temp::new();
        let options = temp.options();
        fs::write(&options.disc1, "synthetic").unwrap();
        fs::write(&options.disc2, "synthetic").unwrap();
        let command = options.command().unwrap();
        let args: Vec<_> = command.get_args().collect();
        assert_eq!(
            &args[4..],
            [
                "--headless",
                "--frame-limit",
                "2000000",
                "--control-at",
                "1",
                "--dump-frame",
                "frame.ppm",
                "--pad-at",
                "1:0"
            ]
            .map(OsStr::new)
        );
        assert!(!options.run_dir.exists());
    }
    #[test]
    fn startup_failures_and_no_response_are_bounded() {
        for mode in ["exit", "malformed", "oversized", "hang-start", "chatty"] {
            let temp = Temp::new();
            let mut options = temp.options();
            if mode == "hang-start" || mode == "chatty" {
                options.response_timeout = Duration::from_millis(100);
            }
            let start = Instant::now();
            let error = match temp.start(options, mode) {
                Ok(_) => panic!("{mode} unexpectedly started"),
                Err(error) => error.to_string(),
            };
            let expected = match mode {
                "exit" => "7",
                "malformed" => "control JSON requires unsigned",
                "oversized" => "stdout line exceeds 1 MiB",
                _ => "no control response",
            };
            assert!(error.contains(expected), "{mode}: {error}");
            assert!(start.elapsed() < Duration::from_secs(5), "{mode}");
        }
    }
    #[test]
    fn advance_timeout_kills_and_reaps_game() {
        let temp = Temp::new();
        let mut session = temp.start(temp.options(), "hang-advance").unwrap();
        session.response_timeout = Duration::from_millis(100);
        assert!(session.advance(10, 0).is_err());
        assert!(session.closed);
        assert!(session.child.try_wait().unwrap().is_some());
        assert!(session.reader.is_none());
    }
    #[test]
    fn shutdown_deadline_kills_and_reaps_game() {
        let temp = Temp::new();
        let mut session = temp.start(temp.options(), "no-quit").unwrap();
        assert!(session
            .quit()
            .unwrap_err()
            .to_string()
            .contains("terminated"));
        assert!(session.child.try_wait().unwrap().is_some());
        assert!(session.closed);
    }
    #[test]
    fn rejects_advancing_terminal_state_and_zero_steps() {
        let temp = Temp::new();
        let mut session = temp.start(temp.options(), "terminal").unwrap();
        assert!(session.advance(0, 0).is_err());
        assert!(session.wait_until(|_| false, 10, 0, 0).is_err());
        assert_eq!(
            session.advance(10, 0).unwrap().event,
            ControlEvent::FrameLimit
        );
        assert!(session.advance(1, 0).is_err());
        session.quit().unwrap();
    }
    #[test]
    fn wait_budget_caps_final_step_and_history_is_bounded() {
        let temp = Temp::new();
        let mut session = temp.start(temp.options(), "normal").unwrap();
        assert!(session.wait_until(|_| false, 11, 10, 0).is_err());
        assert_eq!(session.state().vblanks, 12);
        for _ in 0..2001 {
            session.advance(1, 0).unwrap();
        }
        assert_eq!(session.history().len(), 2000);
        session.quit().unwrap();
    }
}
