use pe_protocol::{control::buttons, RngSnapshot};
use pe_session::{
    process::{GameSession, LaunchOptions},
    DebugControl,
};
use std::{env, path::PathBuf, process::ExitCode, time::Duration};

const HELP: &str = "pe-tools 0.1.0
Rust tools for Parasite Eve development

Usage:
  pe-tools command --control-dir DIR [--timeout-ms 30000] -- COMMAND [ARG ...]
  pe-tools rng-validate \"STATE FRAME INDEX HEX1042\"
  pe-tools run --exe EXE --disc CUE --disc2 CUE --run-dir NEW_DIR [OPTIONS]
  pe-tools --help
  pe-tools --version

The command client attaches to an already running game with PE_DEBUG_CONTROL=DIR.
PE_DEBUG_CONTROL is also the default for --control-dir. Prints the original reply.
The attached command client needs an advancing game.
Run options: --advance FRAMES --button NAME --command TEXT --frame-limit FRAMES
             --response-timeout-ms MS
--exe defaults to PE_DEBUG_EXE. run launches an isolated headless session, prints
initial/final control JSON and an optional debug reply, then quits the process.
Exit 0: success; 1: native/transport failure; 2: invalid arguments or snapshot.
";

fn run(args: Vec<String>) -> Result<(), (u8, String)> {
    let usage = |message: &str| (2, format!("{message}\nRun pe-tools --help for usage."));
    match args.first().map(String::as_str) {
        None | Some("--help" | "-h") => {
            print!("{HELP}");
            Ok(())
        }
        Some("--version" | "-V") => {
            println!("pe-tools {}", env!("CARGO_PKG_VERSION"));
            Ok(())
        }
        Some("rng-validate") if args.len() == 2 => {
            let snapshot: RngSnapshot = args[1]
                .parse()
                .map_err(|e: pe_protocol::ProtocolError| (2, e.to_string()))?;
            println!("{snapshot}");
            Ok(())
        }
        Some("run") => run_game(&args[1..]),
        Some("command") => {
            let mut directory = env::var_os("PE_DEBUG_CONTROL").map(PathBuf::from);
            let mut timeout = 30_000u64;
            let mut i = 1;
            while i < args.len() && args[i] != "--" {
                let value = args
                    .get(i + 1)
                    .ok_or_else(|| usage("missing option value"))?;
                match args[i].as_str() {
                    "--control-dir" => directory = Some(PathBuf::from(value)),
                    "--timeout-ms" => {
                        timeout = value.parse().map_err(|_| {
                            usage("timeout must be an unsigned integer in milliseconds")
                        })?
                    }
                    _ => return Err(usage("unknown command option")),
                }
                i += 2;
            }
            if i >= args.len() || i + 1 >= args.len() {
                return Err(usage("expected -- followed by a debug command"));
            }
            let directory = directory
                .filter(|p| !p.as_os_str().is_empty())
                .ok_or_else(|| usage("supply --control-dir or PE_DEBUG_CONTROL"))?;
            let command = args[i + 1..].join(" ");
            pe_protocol::Command::new(&command).map_err(|e| (2, e.to_string()))?;
            let reply = DebugControl::new(directory)
                .ask(&command, Duration::from_millis(timeout))
                .map_err(|e| (1, e.to_string()))?;
            println!("{}", reply.line);
            Ok(())
        }
        _ => Err(usage("unknown command or incorrect arguments")),
    }
}
fn run_game(args: &[String]) -> Result<(), (u8, String)> {
    let invalid = |s: &str| (2, s.to_owned());
    let mut exe = env::var_os("PE_DEBUG_EXE").map(PathBuf::from);
    let (mut disc1, mut disc2, mut run_dir) = (None, None, None);
    let mut advance = None;
    let mut button = 0;
    let mut debug_command = None;
    let mut frame_limit = 2_000_000;
    let mut response_timeout = 60_000;
    let mut i = 0;
    while i < args.len() {
        let value = args
            .get(i + 1)
            .ok_or_else(|| invalid("missing run option value"))?;
        match args[i].as_str() {
            "--exe" => exe = Some(PathBuf::from(value)),
            "--disc" => disc1 = Some(PathBuf::from(value)),
            "--disc2" => disc2 = Some(PathBuf::from(value)),
            "--run-dir" => run_dir = Some(PathBuf::from(value)),
            "--advance" => {
                advance = Some(
                    value
                        .parse::<u32>()
                        .ok()
                        .filter(|n| *n > 0)
                        .ok_or_else(|| invalid("advance must be a positive u32"))?,
                )
            }
            "--button" => {
                button = buttons::named(value).ok_or_else(|| invalid("unknown button name"))?
            }
            "--command" => {
                pe_protocol::Command::new(value).map_err(|e| (2, e.to_string()))?;
                debug_command = Some(value.clone());
            }
            "--frame-limit" => {
                frame_limit = value
                    .parse::<u64>()
                    .ok()
                    .filter(|n| *n > 0)
                    .ok_or_else(|| invalid("frame limit must be a positive u64"))?
            }
            "--response-timeout-ms" => {
                response_timeout = value
                    .parse::<u64>()
                    .ok()
                    .filter(|n| *n > 0)
                    .ok_or_else(|| invalid("response timeout must be positive milliseconds"))?
            }
            _ => return Err(invalid("unknown run option")),
        }
        i += 2;
    }
    if button != 0 && advance.is_none() {
        return Err(invalid("--button requires --advance"));
    }
    let mut options = LaunchOptions::new(
        exe.ok_or_else(|| invalid("supply --exe or PE_DEBUG_EXE"))?,
        disc1.ok_or_else(|| invalid("supply --disc"))?,
        disc2.ok_or_else(|| invalid("supply --disc2"))?,
        run_dir.ok_or_else(|| invalid("supply a new --run-dir"))?,
    );
    options.frame_limit = frame_limit;
    options.response_timeout = Duration::from_millis(response_timeout);
    let runtime = |e: pe_session::SessionError| (1, e.to_string());
    let mut session = GameSession::launch(options).map_err(runtime)?;
    println!("{}", session.state().raw);
    if let Some(frames) = advance {
        println!("{}", session.advance(frames, button).map_err(runtime)?.raw);
    }
    if let Some(command) = debug_command {
        let reply = session.command(&command, 1200).map_err(runtime)?;
        println!("{}", reply.line);
        println!("{}", session.state().raw);
    }
    session.quit().map_err(runtime)
}
fn main() -> ExitCode {
    match run(env::args().skip(1).collect()) {
        Ok(()) => ExitCode::SUCCESS,
        Err((code, message)) => {
            eprintln!("{message}");
            ExitCode::from(code)
        }
    }
}
