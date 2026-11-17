//! Native debug command wire format, independent of process/file transport.
use std::{error::Error, fmt, str::FromStr};

pub mod control;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ProtocolError(pub String);
impl fmt::Display for ProtocolError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.0)
    }
}
impl Error for ProtocolError {}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Command(String);
impl Command {
    pub fn new(text: impl Into<String>) -> Result<Self, ProtocolError> {
        let text = text.into();
        if text.trim().is_empty() || text.contains(['\r', '\n', '\0']) {
            return Err(ProtocolError(
                "send exactly one nonempty debug command (no CR, LF or NUL)".into(),
            ));
        }
        Ok(Self(text))
    }
    pub fn text(&self) -> &str {
        &self.0
    }
    pub fn verb(&self) -> &str {
        self.0.split_whitespace().next().expect("validated command")
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Status {
    Ok,
    Err,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Reply {
    pub status: Status,
    pub verb: String,
    /// Kept verbatim, including JSON and spaces in settings keys.
    pub details: String,
    pub line: String,
}
impl Reply {
    pub fn matching(command: &Command, line: &str) -> Option<Self> {
        let line = line.trim();
        let mut words = line.split_whitespace();
        let status = match words.next()? {
            "ok" => Status::Ok,
            "err" => Status::Err,
            _ => return None,
        };
        let verb = words.next()?;
        if verb != command.verb() {
            return None;
        }
        let rest = line
            .strip_prefix(match status {
                Status::Ok => "ok",
                Status::Err => "err",
            })?
            .trim_start();
        let details = rest.strip_prefix(verb)?.trim_start().to_owned();
        Some(Self {
            status,
            verb: verb.into(),
            details,
            line: line.into(),
        })
    }
    pub fn restart_required(&self) -> bool {
        self.status == Status::Ok
            && self
                .details
                .split_whitespace()
                .any(|s| s == "restart_required=1")
    }
}

/// Both native RNG streams: no random values are generated while parsing/restoring.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct RngSnapshot {
    pub state: u32,
    pub frame: u32,
    pub index: i32,
    pub table: [u8; 521],
}
impl FromStr for RngSnapshot {
    type Err = ProtocolError;
    fn from_str(text: &str) -> Result<Self, Self::Err> {
        let invalid =
            || ProtocolError("expected STATE FRAME INDEX HEX1042; index must be -1..520".into());
        let words: Vec<_> = text.split_whitespace().collect();
        if words.len() != 4 {
            return Err(invalid());
        }
        let state = words[0].parse().map_err(|_| invalid())?;
        let frame = words[1].parse().map_err(|_| invalid())?;
        let index = words[2].parse().map_err(|_| invalid())?;
        if !(-1..521).contains(&index)
            || words[3].len() != 1042
            || !words[3].bytes().all(|c| c.is_ascii_hexdigit())
        {
            return Err(invalid());
        }
        let mut table = [0; 521];
        for (i, byte) in table.iter_mut().enumerate() {
            *byte = u8::from_str_radix(&words[3][i * 2..i * 2 + 2], 16).map_err(|_| invalid())?;
        }
        Ok(Self {
            state,
            frame,
            index,
            table,
        })
    }
}
impl fmt::Display for RngSnapshot {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{} {} {} ", self.state, self.frame, self.index)?;
        for byte in self.table {
            write!(f, "{byte:02x}")?;
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn command_rejects_empty_and_injection() {
        for text in ["", " \t", "ping\nping", "ping\rping", "ping\0junk"] {
            assert!(Command::new(text).is_err());
        }
        assert_eq!(
            Command::new("ini-set Manage mods/All mods 0")
                .unwrap()
                .verb(),
            "ini-set"
        );
    }
    #[test]
    fn exact_verb_and_status() {
        let cmd = Command::new("battle 1").unwrap();
        for line in [
            "ok battle-state wrong",
            "err battlefield wrong",
            "noise battle 1",
            "ok",
        ] {
            assert!(Reply::matching(&cmd, line).is_none());
        }
        assert_eq!(
            Reply::matching(&cmd, "ok battle chosen").unwrap().details,
            "chosen"
        );
        assert_eq!(
            Reply::matching(&cmd, "err battle not in free field")
                .unwrap()
                .status,
            Status::Err
        );
    }
    #[test]
    fn json_payload_and_restart_token() {
        let cmd = Command::new("mod-state example").unwrap();
        assert_eq!(
            Reply::matching(&cmd, "ok mod-state {\"label\": \"two words\"}")
                .unwrap()
                .details,
            "{\"label\": \"two words\"}"
        );
        let cmd = Command::new("ini-set Manage mods/All mods 0").unwrap();
        assert!(Reply::matching(&cmd, "ok ini-set restart_required=1")
            .unwrap()
            .restart_required());
        assert!(!Reply::matching(&cmd, "ok ini-set not_restart_required=1")
            .unwrap()
            .restart_required());
    }
    #[test]
    fn rng_roundtrip_preserves_every_byte_and_integer_boundary() {
        let mut snapshot = RngSnapshot {
            state: u32::MAX,
            frame: 0,
            index: -1,
            table: [0; 521],
        };
        for (i, byte) in snapshot.table.iter_mut().enumerate() {
            *byte = (i % 256) as u8;
        }
        assert_eq!(
            snapshot.to_string().parse::<RngSnapshot>().unwrap(),
            snapshot
        );
        snapshot.index = 520;
        snapshot.frame = u32::MAX;
        assert_eq!(
            snapshot
                .to_string()
                .to_uppercase()
                .parse::<RngSnapshot>()
                .unwrap(),
            snapshot
        );
    }
    #[test]
    fn rng_refuses_malformed_and_extra_data() {
        let hex = "00".repeat(521);
        for text in [
            format!("0 0 -2 {hex}"),
            format!("0 0 521 {hex}"),
            format!("4294967296 0 0 {hex}"),
            format!("0 -1 0 {hex}"),
            format!("0 0 0 {hex} extra"),
            "0 0 0 ff".into(),
            format!("0 0 0 {}gg", "00".repeat(520)),
            format!("0 0 0 {}é", "00".repeat(520)),
        ] {
            assert!(text.parse::<RngSnapshot>().is_err(), "{text}");
        }
    }
}
