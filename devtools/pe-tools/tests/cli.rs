use std::{
    fs,
    path::PathBuf,
    process::{Command, Output, Stdio},
    thread,
    time::{Duration, Instant},
};

fn cli(args: &[&str]) -> Output {
    Command::new(env!("CARGO_BIN_EXE_pe-tools"))
        .env_remove("PE_DEBUG_CONTROL")
        .args(args)
        .output()
        .unwrap()
}

#[test]
fn help_and_version_need_no_game_or_python() {
    for flag in ["--help", "--version"] {
        let out = cli(&[flag]);
        assert!(out.status.success());
        assert!(String::from_utf8(out.stdout).unwrap().contains("pe-tools"));
    }
}

#[test]
fn invalid_arguments_and_rng_return_two() {
    for args in [
        vec!["unknown"],
        vec!["run"],
        vec!["run", "--advance", "0"],
        vec!["run", "--button", "unknown"],
        vec!["command", "--", "ping"],
        vec!["rng-validate", "1 2 3 ff"],
        vec!["command", "--control-dir", ".", "--", "ping\nping"],
    ] {
        let out = cli(&args);
        assert_eq!(out.status.code(), Some(2));
        assert!(out.stdout.is_empty());
        assert!(!out.stderr.is_empty());
    }
}

#[test]
fn rng_cli_roundtrip() {
    let input = format!("4294967295 0 -1 {}", "aB".repeat(521));
    let out = cli(&["rng-validate", &input]);
    assert!(out.status.success());
    assert_eq!(
        String::from_utf8(out.stdout).unwrap().trim(),
        input.to_lowercase()
    );
}

#[test]
fn offline_battle_and_trace_commands_preserve_json_extensions() {
    let fixture_root = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../pe-protocol/fixtures");
    let state_path = fixture_root.join("battle-state.json");
    let state = cli(&["battle-validate", state_path.to_str().unwrap()]);
    assert!(state.status.success());
    let state = String::from_utf8(state.stdout).unwrap();
    assert!(state.contains("\"future\":\"preserved\""));
    let events_path = fixture_root.join("battle-events.jsonl");
    let events = cli(&["events", events_path.to_str().unwrap()]);
    assert!(events.status.success());
    let events = String::from_utf8(events.stdout).unwrap();
    assert_eq!(events.lines().count(), 9);
    assert!(events.contains("future_event"));
}

#[test]
fn offline_battle_commands_reject_corruption_with_nonzero_status() {
    let temp = Temp::new("corrupt");
    let path = temp.0.join("bad.jsonl");
    fs::write(&path, "{broken").unwrap();
    for command in ["battle-validate", "events"] {
        let out = cli(&[command, path.to_str().unwrap()]);
        assert_eq!(out.status.code(), Some(1));
        assert!(out.stdout.is_empty());
    }
}
#[test]
fn screenshot_cli_writes_png_and_refuses_to_overwrite_it() {
    let temp = Temp::new("screenshot");
    let ppm = temp.0.join("frame.ppm");
    let png = temp.0.join("frame.png");
    fs::write(
        &ppm,
        [b"P6\n2 1\n255\n".as_slice(), &[10, 13, 32, 0, 128, 255]].concat(),
    )
    .unwrap();
    let args = ["ppm-to-png", ppm.to_str().unwrap(), png.to_str().unwrap()];
    assert!(cli(&args).status.success());
    let before = fs::read(&png).unwrap();
    assert!(before.starts_with(b"\x89PNG\r\n\x1a\n"));
    assert_eq!(cli(&args).status.code(), Some(1));
    assert_eq!(fs::read(&png).unwrap(), before);
}

struct Temp(PathBuf);
impl Temp {
    fn new(label: &str) -> Self {
        let dir = std::env::temp_dir().join(format!(
            "pe-cli-{}-{label}-{}",
            std::process::id(),
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ));
        fs::create_dir(&dir).unwrap();
        Self(dir)
    }
}
impl Drop for Temp {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.0);
    }
}

fn mock_game(reply: &str, expected_code: i32) -> Output {
    let dir = Temp::new(if expected_code == 0 { "ok" } else { "err" });
    fs::write(dir.0.join("out"), "ok ping stale\n").unwrap();
    let mut child = Command::new(env!("CARGO_BIN_EXE_pe-tools"))
        .env("PE_DEBUG_CONTROL", &dir.0)
        .args(["command", "--timeout-ms", "5000", "--", "ping"])
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .unwrap();
    let start = Instant::now();
    loop {
        let ready = fs::read_to_string(dir.0.join("cmd")).is_ok_and(|s| s == "ping\n");
        if ready {
            break;
        }
        if start.elapsed() > Duration::from_secs(5) {
            let _ = child.kill();
            let _ = child.wait();
            panic!("CLI never published command");
        }
        thread::sleep(Duration::from_millis(5));
    }
    fs::remove_file(dir.0.join("cmd")).unwrap();
    fs::write(
        dir.0.join("out"),
        format!("ok ping stale\nok ping-other wrong\n{reply}\n"),
    )
    .unwrap();
    let out = child.wait_with_output().unwrap();
    assert_eq!(out.status.code(), Some(expected_code));
    assert!(!dir.0.join("client.lock").exists());
    out
}

#[test]
fn cli_exchanges_files_and_uses_environment_default() {
    let out = mock_game("ok ping 123", 0);
    assert_eq!(String::from_utf8(out.stdout).unwrap().trim(), "ok ping 123");
}

#[test]
fn cli_propagates_native_error_and_exit_status() {
    let out = mock_game("err ping unavailable", 1);
    assert!(String::from_utf8(out.stderr)
        .unwrap()
        .contains("ping: err ping unavailable"));
    assert!(out.stdout.is_empty());
}
