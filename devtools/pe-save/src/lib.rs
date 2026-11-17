//! Bounded save-envelope parsing. Provider payloads and unknown records stay opaque.
use std::{collections::BTreeSet, error::Error, fmt};

// Share the existing pure-Rust CRC implementation without linking the editor GUI.
pub mod companion;
#[path = "../../../save-editor/src/crc.rs"]
pub mod crc;
pub mod portable;
pub use companion::{CardName, Companion, CompanionLink, Section};
pub use portable::PortableSave;
#[cfg(test)]
mod tests;

pub const BLOCK_SIZE: usize = 0x2000;
pub const TAIL_AT: usize = 0x1c90;
pub const TAIL_SIZE: usize = 0x370;
pub const WINDOW_AT: usize = 0x200;
pub const WINDOW_END: usize = 0x288;
pub const WINDOW_SIZE: usize = WINDOW_END - WINDOW_AT;
pub const ROOM: usize = TAIL_SIZE - WINDOW_SIZE;
const LEGACY: [(&str, usize, usize, &[u8; 4]); 5] = [
    ("pe_economy.loot", 0, 0x20, b"PELT"),
    ("pe_economy.catalog", 0x40, 0x148, b"PECT"),
    ("pe_economy.shop", 0x1c0, 0x14, b"PESH"),
    ("pe_qol.keys", 0x200, 0x54, b"PKEY"),
    ("pe_qol.sort", 0x260, 0x28, b"SORT"),
];
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct SaveError(pub String);
impl fmt::Display for SaveError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.0)
    }
}
impl Error for SaveError {}
pub(crate) fn require(ok: bool, message: &str) -> Result<(), SaveError> {
    if ok {
        Ok(())
    } else {
        Err(SaveError(message.into()))
    }
}
pub fn fnv1a(name: &str) -> u32 {
    name.bytes().fold(0x811c9dc5, |h, b| {
        (h ^ u32::from(b)).wrapping_mul(0x01000193)
    })
}
pub fn crc32(bytes: &[u8]) -> u32 {
    crc32fast::hash(bytes)
}
pub(crate) fn u16_at(bytes: &[u8], at: usize) -> u16 {
    u16::from_le_bytes(bytes[at..at + 2].try_into().expect("bounded header"))
}
pub(crate) fn u32_at(bytes: &[u8], at: usize) -> u32 {
    u32::from_le_bytes(bytes[at..at + 4].try_into().expect("bounded header"))
}
pub fn gather(tail: &[u8]) -> Result<Vec<u8>, SaveError> {
    require(tail.len() == TAIL_SIZE, "expected an 880-byte mod tail")?;
    Ok([&tail[..WINDOW_AT], &tail[WINDOW_END..]].concat())
}
pub fn scatter(linear: &[u8], window: &[u8]) -> Result<Vec<u8>, SaveError> {
    require(
        linear.len() <= ROOM && window.len() == WINDOW_SIZE,
        "invalid container or compatibility window size",
    )?;
    let mut padded = vec![0; ROOM];
    padded[..linear.len()].copy_from_slice(linear);
    Ok([&padded[..WINDOW_AT], window, &padded[WINDOW_AT..]].concat())
}
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Source {
    Container,
    Legacy,
    Window,
}
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Record {
    pub id: u32,
    pub version: u16,
    pub data: Vec<u8>,
    pub crc_ok: bool,
    pub source: Source,
}
impl Record {
    pub fn new(name: &str, version: u16, data: Vec<u8>) -> Self {
        Self {
            id: fnv1a(name),
            version,
            data,
            crc_ok: true,
            source: Source::Container,
        }
    }
}
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ModKind {
    Empty,
    Unknown,
    Legacy,
    Container,
    Damaged,
    Foreign,
}
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ModData {
    pub kind: ModKind,
    pub records: Vec<Record>,
    pub container_ok: bool,
    pub window_changed: bool,
    pub format: u8,
    pub total: u16,
    pub api: u16,
    pub writer: u16,
    pub errors: Vec<String>,
}
impl ModData {
    pub fn parse(tail: &[u8]) -> Result<Self, SaveError> {
        require(tail.len() == TAIL_SIZE, "expected an 880-byte mod tail")?;
        let mut result = Self {
            kind: ModKind::Legacy,
            records: vec![],
            container_ok: false,
            window_changed: false,
            format: 0,
            total: 0,
            api: 0,
            writer: 0,
            errors: vec![],
        };
        if &tail[..4] != b"PEMC" {
            for (name, at, size, magic) in LEGACY {
                if &tail[at..at + 4] == magic {
                    result.records.push(Record {
                        id: fnv1a(name),
                        version: 0,
                        data: tail[at..at + size].to_vec(),
                        crc_ok: true,
                        source: Source::Legacy,
                    });
                }
            }
            if result.records.is_empty() {
                result.kind = if tail.iter().all(|b| *b == 0) {
                    ModKind::Empty
                } else {
                    ModKind::Unknown
                };
            }
            return Ok(result);
        }
        result.kind = ModKind::Container;
        result.format = tail[4];
        result.total = u16_at(tail, 6);
        if result.format > 1 || (result.format == 1 && tail[5] != 0) {
            result.kind = ModKind::Foreign;
            return Ok(result);
        }
        let total = usize::from(result.total);
        if result.format != 1 || !(20..=ROOM).contains(&total) {
            result.kind = ModKind::Damaged;
            result.errors.push("invalid container header".into());
            return Ok(result);
        }
        let mut linear = gather(tail)?;
        let checksum = u32_at(&linear, 8);
        linear[8..12].fill(0);
        result.container_ok = crc32(&linear[..total]) == checksum;
        linear[8..12].copy_from_slice(&checksum.to_le_bytes());
        result.api = u16_at(&linear, 16);
        result.writer = u16_at(&linear, 18);
        let mut at = 20;
        let mut ids = BTreeSet::new();
        while at < total {
            if total - at < 12 {
                result
                    .errors
                    .push(format!("truncated record header at {at}"));
                break;
            }
            let size = usize::from(u16_at(&linear, at + 6));
            if size > total - at - 12 {
                result
                    .errors
                    .push(format!("record at {at} runs past container"));
                break;
            }
            let id = u32_at(&linear, at);
            if !ids.insert(id) {
                result.errors.push(format!("duplicate record {id:08x}"));
            }
            let data = linear[at + 12..at + 12 + size].to_vec();
            let mut checksum = crc32fast::Hasher::new();
            checksum.update(&linear[at..at + 8]);
            checksum.update(&data);
            result.records.push(Record {
                id,
                version: u16_at(&linear, at + 4),
                data,
                crc_ok: checksum.finalize() == u32_at(&linear, at + 8),
                source: Source::Container,
            });
            at += 12 + size;
        }
        result.window_changed = crc32(&tail[WINDOW_AT..WINDOW_END]) != u32_at(&linear, 12);
        if result.window_changed {
            for (name, at, size, magic) in LEGACY {
                if at >= WINDOW_AT && &tail[at..at + 4] == magic {
                    let id = fnv1a(name);
                    result.records.retain(|r| r.id != id);
                    result.records.push(Record {
                        id,
                        version: 0,
                        data: tail[at..at + size].to_vec(),
                        crc_ok: true,
                        source: Source::Window,
                    });
                }
            }
        }
        Ok(result)
    }
    pub fn validate(&self) -> Result<(), SaveError> {
        require(
            matches!(
                self.kind,
                ModKind::Empty | ModKind::Legacy | ModKind::Container
            ),
            "unsupported, unknown or damaged mod tail",
        )?;
        if self.kind == ModKind::Container {
            require(self.container_ok, "container checksum mismatch")?;
            require(
                self.errors.is_empty(),
                &format!("invalid container records: {:?}", self.errors),
            )?;
            require(
                self.records.iter().all(|r| r.crc_ok),
                "record checksum mismatch",
            )?;
        }
        Ok(())
    }
}
pub fn build_container(
    records: &[Record],
    window: &[u8],
    api: u16,
    writer: u16,
) -> Result<Vec<u8>, SaveError> {
    require(
        window.len() == WINDOW_SIZE,
        "invalid compatibility window size",
    )?;
    let mut sorted: Vec<_> = records.iter().collect();
    sorted.sort_by_key(|r| r.id);
    let mut linear = vec![0; 20];
    linear[..4].copy_from_slice(b"PEMC");
    linear[4] = 1;
    let mut previous = None;
    for record in sorted {
        require(previous != Some(record.id), "duplicate container record")?;
        previous = Some(record.id);
        require(
            record.data.len() <= ROOM && linear.len() + 12 + record.data.len() <= ROOM,
            "records do not fit the container",
        )?;
        let mut header = Vec::from(record.id.to_le_bytes());
        header.extend(record.version.to_le_bytes());
        header.extend((record.data.len() as u16).to_le_bytes());
        let mut checksum = crc32fast::Hasher::new();
        checksum.update(&header);
        checksum.update(&record.data);
        linear.extend(header);
        linear.extend(checksum.finalize().to_le_bytes());
        linear.extend(&record.data);
    }
    let total = linear.len() as u16;
    linear[6..8].copy_from_slice(&total.to_le_bytes());
    linear[12..16].copy_from_slice(&crc32(window).to_le_bytes());
    linear[16..18].copy_from_slice(&api.to_le_bytes());
    linear[18..20].copy_from_slice(&writer.to_le_bytes());
    let checksum = crc32(&linear);
    linear[8..12].copy_from_slice(&checksum.to_le_bytes());
    scatter(&linear, window)
}
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Card {
    bytes: Vec<u8>,
    mods: ModData,
}
impl Card {
    pub fn parse(bytes: Vec<u8>) -> Result<Self, SaveError> {
        require(
            bytes.len() == BLOCK_SIZE,
            "expected an 8192-byte card block",
        )?;
        let mods = ModData::parse(&bytes[TAIL_AT..])?;
        Ok(Self { bytes, mods })
    }
    pub fn bytes(&self) -> &[u8] {
        &self.bytes
    }
    pub fn mods(&self) -> &ModData {
        &self.mods
    }
    pub fn crc_ok(&self) -> bool {
        crc::read_stored_crc(&self.bytes) == u32::from(crc::calc_save_crc16(&self.bytes))
    }
    pub fn validate(&self) -> Result<(), SaveError> {
        require(self.crc_ok(), "game checksum mismatch")?;
        self.mods.validate()
    }
    pub fn companion_link(&self) -> Result<Option<CompanionLink>, SaveError> {
        self.validate()?;
        let links: Vec<_> = self
            .mods
            .records
            .iter()
            .filter(|r| r.id == fnv1a("pe_host.extlink"))
            .collect();
        require(links.len() <= 1, "duplicate companion link")?;
        links
            .first()
            .map(|r| CompanionLink::parse(r.version, &r.data))
            .transpose()
    }
}
