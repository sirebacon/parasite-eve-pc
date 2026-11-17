//! Control-mode JSON and PS1 input, preserving fields added by the native host.
use crate::ProtocolError;
use serde_json::Value;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ControlEvent {
    Control,
    FrameLimit,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ControlState {
    pub event: ControlEvent,
    pub vblanks: u64,
    pub scene: u64,
    pub flags: u64,
    pub game_flags: u64,
    pub raw: Value,
}
impl ControlState {
    /// Non-JSON diagnostics and unrelated JSON events do not form control boundaries.
    pub fn from_line(line: &str) -> Result<Option<Self>, ProtocolError> {
        if !line.trim_start().starts_with('{') {
            return Ok(None);
        }
        let raw: Value = serde_json::from_str(line)
            .map_err(|e| ProtocolError(format!("invalid control JSON: {e}")))?;
        let event = match raw.get("event").and_then(Value::as_str) {
            Some("control") => ControlEvent::Control,
            Some("frame_limit") => ControlEvent::FrameLimit,
            _ => return Ok(None),
        };
        let number = |key| {
            raw.get(key)
                .and_then(Value::as_u64)
                .ok_or_else(|| ProtocolError(format!("control JSON requires unsigned {key}")))
        };
        Ok(Some(Self {
            event,
            vblanks: number("vblanks")?,
            scene: number("scene")?,
            flags: number("flags")?,
            game_flags: number("game_flags")?,
            raw,
        }))
    }
    pub fn menu(&self) -> Option<&Value> {
        self.raw.get("menu").filter(|v| !v.is_null())
    }
    pub fn player(&self) -> Option<&Value> {
        self.raw
            .get("actors")?
            .as_array()?
            .iter()
            .find(|v| v.get("player").and_then(Value::as_bool) == Some(true))
    }
    pub fn hp(&self) -> Option<u64> {
        self.raw.get("combat")?.get("player_hp")?.as_u64()
    }
    pub fn max_hp(&self) -> Option<u64> {
        self.raw.get("combat")?.get("player_max_hp")?.as_u64()
    }
    pub fn dialogue(&self) -> Vec<&Value> {
        self.raw
            .get("dialogue")
            .and_then(Value::as_array)
            .into_iter()
            .flatten()
            .filter(|v| {
                v.get("state")
                    .and_then(Value::as_u64)
                    .is_some_and(|s| s != 0)
            })
            .collect()
    }
    pub fn short(&self) -> String {
        format!(
            "vb {} scene {} flags {:#x} gs {:#x} menu {} hp {:?}/{:?} dialogue {}",
            self.vblanks,
            self.scene,
            self.flags,
            self.game_flags,
            self.menu().unwrap_or(&Value::Null),
            self.hp(),
            self.max_hp(),
            self.dialogue().len()
        )
    }
    pub fn stable_key(&self) -> (u64, u64, u64, Option<Value>) {
        (
            self.scene,
            self.flags,
            self.game_flags,
            self.menu().cloned(),
        )
    }
}

pub mod buttons {
    pub const SELECT: u16 = 0x0001;
    pub const L3: u16 = 0x0002;
    pub const R3: u16 = 0x0004;
    pub const START: u16 = 0x0008;
    pub const UP: u16 = 0x0010;
    pub const RIGHT: u16 = 0x0020;
    pub const DOWN: u16 = 0x0040;
    pub const LEFT: u16 = 0x0080;
    pub const L2: u16 = 0x0100;
    pub const R2: u16 = 0x0200;
    pub const L1: u16 = 0x0400;
    pub const R1: u16 = 0x0800;
    pub const TRIANGLE: u16 = 0x1000;
    pub const CIRCLE: u16 = 0x2000;
    pub const CROSS: u16 = 0x4000;
    pub const SQUARE: u16 = 0x8000;
    pub fn named(name: &str) -> Option<u16> {
        Some(match name {
            "select" => SELECT,
            "l3" => L3,
            "r3" => R3,
            "start" => START,
            "up" => UP,
            "right" => RIGHT,
            "down" => DOWN,
            "left" => LEFT,
            "l2" => L2,
            "r2" => R2,
            "l1" => L1,
            "r1" => R1,
            "triangle" => TRIANGLE,
            "circle" => CIRCLE,
            "cross" => CROSS,
            "square" => SQUARE,
            _ => return None,
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn parses_reordered_json_preserves_extensions_and_accessors() {
        let state = ControlState::from_line(r#"{ "scene":3,"event":"control","flags":2,"game_flags":9,"vblanks":10,"future":123,"combat":{"player_hp":8,"player_max_hp":10},"actors":[{"player":false},{"player":true,"position":[1,2,3]}],"dialogue":[{"state":0},{"state":1}] }"#).unwrap().unwrap();
        assert_eq!(state.raw["future"], 123);
        assert_eq!(state.hp(), Some(8));
        assert_eq!(state.max_hp(), Some(10));
        assert_eq!(state.player().unwrap()["position"][2], 3);
        assert_eq!(state.dialogue().len(), 1);
        assert!(state.menu().is_none());
    }
    #[test]
    fn ignores_logs_validates_required_fields_and_terminal_event() {
        assert!(ControlState::from_line("boot log").unwrap().is_none());
        assert!(ControlState::from_line(r#"{"event":"other"}"#)
            .unwrap()
            .is_none());
        assert!(ControlState::from_line(r#"{"event":"control"}"#).is_err());
        assert!(ControlState::from_line(r#"{"event":"control","vblanks":-1}"#).is_err());
        assert!(ControlState::from_line("{broken").is_err());
        assert_eq!(
            ControlState::from_line(
                r#"{"event":"frame_limit","vblanks":20,"scene":0,"flags":0,"game_flags":0}"#
            )
            .unwrap()
            .unwrap()
            .event,
            ControlEvent::FrameLimit
        );
    }
    #[test]
    fn native_button_masks_are_exact() {
        let names = [
            "select", "l3", "r3", "start", "up", "right", "down", "left", "l2", "r2", "l1", "r1",
            "triangle", "circle", "cross", "square",
        ];
        for (index, name) in names.iter().enumerate() {
            assert_eq!(buttons::named(name), Some(1u16 << index));
        }
        assert_eq!(buttons::named("unknown"), None);
    }
}
