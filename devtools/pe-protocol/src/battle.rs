//! Typed battle observations; the original JSON is retained for future host fields.
use crate::ProtocolError;
use serde_json::Value;
use std::str::FromStr;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct TargetListIndex(pub i32);
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct FieldActorIndex(pub u32);

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PlayerState {
    pub hp: u32,
    pub max_hp: u32,
    pub atb: u32,
    pub pe_raw: i32,
    pub max_pe_raw: i32,
    pub status: u32,
    pub bp: u32,
    pub level: u32,
}
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct WeaponState {
    pub id: u32,
    pub kind: u32,
    pub ammo: u32,
    pub base: [u32; 3],
    pub bonus: [i32; 3],
    pub mods: Vec<u32>,
}
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct EnemyState {
    pub actor: FieldActorIndex,
    pub actor_type: u32,
    pub sub: u32,
    pub id: u32,
    pub hp: i32,
    pub max_hp: i32,
    pub status: u32,
    pub status2: u32,
    pub position: [i32; 3],
    pub steal_item: u32,
}
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct BattleState {
    pub active: bool,
    pub frame: i32,
    pub mode: i32,
    pub target_index: TargetListIndex,
    pub queue_count: u32,
    pub player: Option<PlayerState>,
    pub weapon: Option<WeaponState>,
    pub enemies: Vec<EnemyState>,
    pub raw: Value,
}

// Every conversion names the field, rejects wrong JSON types and checks native widths.
struct Json<'a> {
    value: &'a Value,
    path: String,
}
impl<'a> Json<'a> {
    fn root(value: &'a Value, path: &str) -> Self {
        Self {
            value,
            path: path.into(),
        }
    }
    fn error(&self, expected: &str) -> ProtocolError {
        ProtocolError(format!("{}: expected {expected}", self.path))
    }
    fn field(&self, key: &str) -> Result<Self, ProtocolError> {
        Ok(Self {
            value: self
                .value
                .get(key)
                .ok_or_else(|| ProtocolError(format!("{}.{}: missing field", self.path, key)))?,
            path: format!("{}.{}", self.path, key),
        })
    }
    fn u32(&self) -> Result<u32, ProtocolError> {
        self.value
            .as_u64()
            .and_then(|n| n.try_into().ok())
            .ok_or_else(|| self.error("u32"))
    }
    fn i32(&self) -> Result<i32, ProtocolError> {
        self.value
            .as_i64()
            .and_then(|n| n.try_into().ok())
            .ok_or_else(|| self.error("i32"))
    }
    fn bool(&self) -> Result<bool, ProtocolError> {
        self.value.as_bool().ok_or_else(|| self.error("boolean"))
    }
    fn text(&self) -> Result<String, ProtocolError> {
        self.value
            .as_str()
            .map(str::to_owned)
            .ok_or_else(|| self.error("string"))
    }
    fn array(&self) -> Result<Vec<Self>, ProtocolError> {
        self.value
            .as_array()
            .ok_or_else(|| self.error("array"))
            .map(|a| {
                a.iter()
                    .enumerate()
                    .map(|(i, value)| Self {
                        value,
                        path: format!("{}[{i}]", self.path),
                    })
                    .collect()
            })
    }
    fn u32_array(&self) -> Result<Vec<u32>, ProtocolError> {
        self.array()?.iter().map(Self::u32).collect()
    }
    fn u32_triple(&self) -> Result<[u32; 3], ProtocolError> {
        self.u32_array()?
            .try_into()
            .map_err(|_| self.error("three u32 values"))
    }
    fn i32_triple(&self) -> Result<[i32; 3], ProtocolError> {
        self.array()?
            .iter()
            .map(Self::i32)
            .collect::<Result<Vec<_>, _>>()?
            .try_into()
            .map_err(|_| self.error("three i32 values"))
    }
}
fn parse(text: &str, kind: &str) -> Result<Value, ProtocolError> {
    serde_json::from_str(text).map_err(|e| ProtocolError(format!("{kind}: invalid JSON: {e}")))
}
impl FromStr for BattleState {
    type Err = ProtocolError;
    fn from_str(text: &str) -> Result<Self, Self::Err> {
        let raw = parse(text, "battle-state")?;
        let json = Json::root(&raw, "battle-state");
        let player = json.field("player")?;
        let player = if player.value.is_null() {
            None
        } else {
            Some(PlayerState {
                hp: player.field("hp")?.u32()?,
                max_hp: player.field("max_hp")?.u32()?,
                atb: player.field("atb")?.u32()?,
                pe_raw: player.field("pe_raw")?.i32()?,
                max_pe_raw: player.field("max_pe_raw")?.i32()?,
                status: player.field("status")?.u32()?,
                bp: player.field("bp")?.u32()?,
                level: player.field("level")?.u32()?,
            })
        };
        let weapon = json.field("weapon")?;
        let weapon = if weapon.value.is_null() {
            None
        } else {
            Some(WeaponState {
                id: weapon.field("id")?.u32()?,
                kind: weapon.field("kind")?.u32()?,
                ammo: weapon.field("ammo")?.u32()?,
                base: weapon.field("base")?.u32_triple()?,
                bonus: weapon.field("bonus")?.i32_triple()?,
                mods: weapon.field("mods")?.u32_array()?,
            })
        };
        let enemies = json
            .field("enemies")?
            .array()?
            .iter()
            .map(|enemy| {
                Ok(EnemyState {
                    actor: FieldActorIndex(enemy.field("actor")?.u32()?),
                    actor_type: enemy.field("type")?.u32()?,
                    sub: enemy.field("sub")?.u32()?,
                    id: enemy.field("id")?.u32()?,
                    hp: enemy.field("hp")?.i32()?,
                    max_hp: enemy.field("max_hp")?.i32()?,
                    status: enemy.field("status")?.u32()?,
                    status2: enemy.field("status2")?.u32()?,
                    position: enemy.field("position")?.i32_triple()?,
                    steal_item: enemy.field("steal_item")?.u32()?,
                })
            })
            .collect::<Result<Vec<_>, ProtocolError>>()?;
        Ok(Self {
            active: json.field("active")?.bool()?,
            frame: json.field("frame")?.i32()?,
            mode: json.field("mode")?.i32()?,
            target_index: TargetListIndex(json.field("target_index")?.i32()?),
            queue_count: json.field("queue_count")?.u32()?,
            player,
            weapon,
            enemies,
            raw,
        })
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ModRow {
    pub page: String,
    pub label: String,
    pub value: String,
}
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ModState {
    pub id: String,
    pub running: bool,
    pub rows: Vec<ModRow>,
    pub raw: Value,
}
impl FromStr for ModState {
    type Err = ProtocolError;
    fn from_str(text: &str) -> Result<Self, Self::Err> {
        let raw = parse(text, "mod-state")?;
        let json = Json::root(&raw, "mod-state");
        let rows = json
            .field("rows")?
            .array()?
            .iter()
            .map(|row| {
                Ok(ModRow {
                    page: row.field("page")?.text()?,
                    label: row.field("label")?.text()?,
                    value: row.field("value")?.text()?,
                })
            })
            .collect::<Result<Vec<_>, ProtocolError>>()?;
        Ok(Self {
            id: json.field("id")?.text()?,
            running: json.field("running")?.bool()?,
            rows,
            raw,
        })
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct DropOffer {
    pub id: i32,
    pub extra: i32,
}
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum BattleEventKind {
    EnemyHpCommit {
        actor_type: u32,
        sub: u32,
        before: i32,
        after: i32,
        loss: i32,
        reaction: u32,
        critical: bool,
    },
    HpLossAdmitted {
        before: i32,
        loss: i32,
        source: u32,
    },
    PlayerHpChanged {
        before: i32,
        after: u32,
    },
    RewardsOffered {
        exp: i32,
        drops: Vec<DropOffer>,
    },
    Steal {
        item: i32,
        success: bool,
    },
    Resources {
        battle: bool,
        bp: u32,
        exp: u32,
        level: u32,
    },
    PlayerAction,
    Spell,
    Unknown(String),
}
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct BattleEvent {
    pub frame: u32,
    pub battle_frame: Option<i32>,
    pub kind: BattleEventKind,
    pub raw: Value,
}
impl FromStr for BattleEvent {
    type Err = ProtocolError;
    fn from_str(text: &str) -> Result<Self, Self::Err> {
        let raw = parse(text, "battle event")?;
        let json = Json::root(&raw, "battle event");
        let name = json.field("event")?.text()?;
        let kind = match name.as_str() {
            "enemy_hp_commit" => BattleEventKind::EnemyHpCommit {
                actor_type: json.field("type")?.u32()?,
                sub: json.field("sub")?.u32()?,
                before: json.field("before")?.i32()?,
                after: json.field("after")?.i32()?,
                loss: json.field("loss")?.i32()?,
                reaction: json.field("reaction")?.u32()?,
                critical: json.field("critical")?.bool()?,
            },
            "hp_loss_admitted" => BattleEventKind::HpLossAdmitted {
                before: json.field("before")?.i32()?,
                loss: json.field("loss")?.i32()?,
                source: json.field("source")?.u32()?,
            },
            "player_hp_changed" => BattleEventKind::PlayerHpChanged {
                before: json.field("before")?.i32()?,
                after: json.field("after")?.u32()?,
            },
            "rewards_offered" => BattleEventKind::RewardsOffered {
                exp: json.field("exp")?.i32()?,
                drops: json
                    .field("drops")?
                    .array()?
                    .iter()
                    .map(|drop| {
                        Ok(DropOffer {
                            id: drop.field("id")?.i32()?,
                            extra: drop.field("extra")?.i32()?,
                        })
                    })
                    .collect::<Result<Vec<_>, ProtocolError>>()?,
            },
            "steal" => BattleEventKind::Steal {
                item: json.field("item")?.i32()?,
                success: json.field("success")?.bool()?,
            },
            "resources" => BattleEventKind::Resources {
                battle: json.field("battle")?.bool()?,
                bp: json.field("bp")?.u32()?,
                exp: json.field("exp")?.u32()?,
                level: json.field("level")?.u32()?,
            },
            "player_action" => BattleEventKind::PlayerAction,
            "spell" => BattleEventKind::Spell,
            _ => BattleEventKind::Unknown(name),
        };
        let battle_frame = if raw.get("battle_frame").is_some() {
            Some(json.field("battle_frame")?.i32()?)
        } else {
            None
        };
        Ok(Self {
            frame: json.field("frame")?.u32()?,
            battle_frame,
            kind,
            raw,
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    const STATE: &str = include_str!("../fixtures/battle-state.json");
    #[test]
    fn parses_native_fields_signed_stats_and_distinct_indexes() {
        let state: BattleState = STATE.parse().unwrap();
        assert_eq!(state.target_index, TargetListIndex(1));
        assert_eq!(state.enemies[0].actor, FieldActorIndex(7));
        assert_eq!(state.enemies[0].hp, -5);
        assert_eq!(state.enemies[0].position, [65536, -1, i32::MIN]);
        assert_eq!(state.weapon.as_ref().unwrap().bonus, [-1, 2, 3]);
        assert_eq!(state.player.as_ref().unwrap().pe_raw, 65536);
        assert_eq!(state.raw["future"], "preserved");
        assert_eq!(state.raw["player"]["extension"], true);
    }
    #[test]
    fn absent_entities_are_null_not_missing_or_wrong_type() {
        let mut raw: Value = serde_json::from_str(STATE).unwrap();
        raw["player"] = Value::Null;
        raw["weapon"] = Value::Null;
        raw["enemies"] = serde_json::json!([]);
        let state: BattleState = raw.to_string().parse().unwrap();
        assert!(state.player.is_none());
        assert!(state.weapon.is_none());
        assert!(state.enemies.is_empty());
        raw.as_object_mut().unwrap().remove("player");
        assert!(raw
            .to_string()
            .parse::<BattleState>()
            .unwrap_err()
            .to_string()
            .contains("battle-state.player"));
        raw["player"] = serde_json::json!(false);
        assert!(raw.to_string().parse::<BattleState>().is_err());
    }
    #[test]
    fn rejects_numeric_overflows_wrong_types_and_vectors_with_field_context() {
        for (field, value) in [
            ("frame", serde_json::json!(2147483648u64)),
            ("queue_count", serde_json::json!(-1)),
            ("queue_count", serde_json::json!(4294967296u64)),
            ("active", serde_json::json!(1)),
        ] {
            let mut raw: Value = serde_json::from_str(STATE).unwrap();
            raw[field] = value;
            assert!(raw
                .to_string()
                .parse::<BattleState>()
                .unwrap_err()
                .to_string()
                .contains(field));
        }
        let mut raw: Value = serde_json::from_str(STATE).unwrap();
        raw["enemies"][0]["position"] = serde_json::json!([1, 2]);
        assert!(raw
            .to_string()
            .parse::<BattleState>()
            .unwrap_err()
            .to_string()
            .contains("enemies[0].position"));
    }
    #[test]
    fn distinguishes_admission_commit_observation_offer_and_transfer() {
        let events: Vec<BattleEvent> = include_str!("../fixtures/battle-events.jsonl")
            .lines()
            .map(|l| l.parse().unwrap())
            .collect();
        assert!(matches!(
            events[0].kind,
            BattleEventKind::EnemyHpCommit {
                before: 10,
                after: -5,
                loss: 15,
                ..
            }
        ));
        assert!(matches!(
            events[1].kind,
            BattleEventKind::HpLossAdmitted { loss: 20, .. }
        ));
        assert!(matches!(
            events[2].kind,
            BattleEventKind::PlayerHpChanged {
                before: 50,
                after: 35
            }
        ));
        assert!(matches!(
            events[3].kind,
            BattleEventKind::RewardsOffered { exp: 99, .. }
        ));
        assert!(matches!(
            events[4].kind,
            BattleEventKind::Steal { success: false, .. }
        ));
        assert!(matches!(events[5].kind, BattleEventKind::Resources { .. }));
        assert_eq!(events[6].kind, BattleEventKind::PlayerAction);
        assert_eq!(events[7].kind, BattleEventKind::Spell);
        assert_eq!(
            events[8].kind,
            BattleEventKind::Unknown("future_event".into())
        );
        assert_eq!(events[8].raw["future"], 123);
    }
    #[test]
    fn trace_requires_valid_known_fields_and_never_guesses_missing_values() {
        for text in [
            r#"{"event":"steal","frame":1,"item":3}"#,
            r#"{"event":"resources","frame":-1}"#,
            r#"{"event":"spell","frame":0,"battle_frame":null}"#,
            r#"{"event":"enemy_hp_commit","frame":1}"#,
        ] {
            assert!(text.parse::<BattleEvent>().is_err());
        }
    }
    #[test]
    fn mod_rows_preserve_spaces_and_inactive_values() {
        let state: ModState = include_str!("../fixtures/mod-state.json").parse().unwrap();
        assert!(!state.running);
        assert_eq!(state.rows[0].value, "inactive");
        assert_eq!(state.rows[1].label, "All mods");
        assert_eq!(state.raw["future"], 123);
    }
}
