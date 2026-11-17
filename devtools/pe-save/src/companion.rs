use crate::{crc32, require, u16_at, u32_at, SaveError};
pub const FILE_LIMIT: usize = 8192;
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CardName(String);
impl CardName {
    pub fn new(name: &str) -> Result<Self, SaveError> {
        require(
            !name.is_empty()
                && name.len() <= 63
                && name.bytes().all(|b| b.is_ascii_alphanumeric() || b == b'-'),
            "invalid card basename",
        )?;
        Ok(Self(name.into()))
    }
    pub fn as_str(&self) -> &str {
        &self.0
    }
}
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CompanionLink {
    pub generation: u32,
    pub size: u32,
    pub checksum: u32,
    pub sections: u8,
}
impl CompanionLink {
    pub fn parse(version: u16, bytes: &[u8]) -> Result<Self, SaveError> {
        require(
            version == 1 && bytes.len() == 20,
            "invalid companion link version/size",
        )?;
        require(
            bytes[..4] == [1, 1, 0, 0] && bytes[17..] == [0, 0, 0],
            "invalid companion link flags",
        )?;
        let link = Self {
            generation: u32_at(bytes, 4),
            size: u32_at(bytes, 8),
            checksum: u32_at(bytes, 12),
            sections: bytes[16],
        };
        require(
            link.generation != 0
                && (96..=FILE_LIMIT as u32).contains(&link.size)
                && link.sections <= 16,
            "invalid companion link bounds",
        )?;
        Ok(link)
    }
    pub fn filename(&self, name: &CardName) -> String {
        format!("{}.{:08x}.pex", name.as_str(), self.generation)
    }
    pub fn payload(&self) -> [u8; 20] {
        let mut bytes = [0; 20];
        bytes[0] = 1;
        bytes[1] = 1;
        bytes[4..8].copy_from_slice(&self.generation.to_le_bytes());
        bytes[8..12].copy_from_slice(&self.size.to_le_bytes());
        bytes[12..16].copy_from_slice(&self.checksum.to_le_bytes());
        bytes[16] = self.sections;
        bytes
    }
}
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Section {
    pub key: String,
    pub version: u16,
    pub reader: u16,
    pub data: Vec<u8>,
}
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Companion {
    bytes: Vec<u8>,
    sections: Vec<Section>,
}
fn text(bytes: &[u8]) -> Result<&str, SaveError> {
    let end = bytes
        .iter()
        .position(|b| *b == 0)
        .ok_or_else(|| SaveError("unterminated companion string".into()))?;
    std::str::from_utf8(&bytes[..end])
        .map_err(|_| SaveError("invalid companion string encoding".into()))
}
impl Companion {
    /// Build bounded generic sections for fixtures/tools; provider data stays opaque.
    pub fn build(
        name: &CardName,
        generation: u32,
        sections: &[Section],
    ) -> Result<(Vec<u8>, CompanionLink), SaveError> {
        require(
            generation != 0 && sections.len() <= 16,
            "invalid companion generation/count",
        )?;
        let mut sorted: Vec<_> = sections.iter().collect();
        sorted.sort_by(|a, b| a.key.cmp(&b.key));
        let mut bytes = vec![0; 96 + 64 * sorted.len()];
        bytes[..8].copy_from_slice(b"PEEX\x01\0\x60\0");
        bytes[8..12].copy_from_slice(&generation.to_le_bytes());
        bytes[16..18].copy_from_slice(&(sorted.len() as u16).to_le_bytes());
        bytes[20..20 + name.as_str().len()].copy_from_slice(name.as_str().as_bytes());
        for (i, section) in sorted.iter().enumerate() {
            require(
                !section.key.is_empty()
                    && section.key.len() <= 47
                    && section.key.bytes().all(|b| {
                        b.is_ascii_lowercase() || b.is_ascii_digit() || b"_-/.".contains(&b)
                    })
                    && !section.data.is_empty()
                    && bytes.len() + section.data.len() <= FILE_LIMIT,
                "invalid companion section size",
            )?;
            let at = 96 + i * 64;
            let offset = bytes.len() as u32;
            bytes[at..at + section.key.len()].copy_from_slice(section.key.as_bytes());
            bytes[at + 48..at + 50].copy_from_slice(&section.version.to_le_bytes());
            bytes[at + 50..at + 52].copy_from_slice(&section.reader.to_le_bytes());
            bytes[at + 52..at + 56].copy_from_slice(&offset.to_le_bytes());
            bytes[at + 56..at + 60].copy_from_slice(&(section.data.len() as u32).to_le_bytes());
            bytes[at + 60..at + 64].copy_from_slice(&crc32(&section.data).to_le_bytes());
            bytes.extend(&section.data);
        }
        let size = bytes.len() as u32;
        bytes[12..16].copy_from_slice(&size.to_le_bytes());
        let internal = crc32(&bytes);
        bytes[88..92].copy_from_slice(&internal.to_le_bytes());
        let link = CompanionLink {
            generation,
            size,
            checksum: crc32(&bytes),
            sections: sections.len() as u8,
        };
        Self::parse(bytes.clone(), name, &link)?;
        Ok((bytes, link))
    }
    pub fn parse(bytes: Vec<u8>, name: &CardName, link: &CompanionLink) -> Result<Self, SaveError> {
        CompanionLink::parse(1, &link.payload())?;
        require(
            (96..=FILE_LIMIT).contains(&bytes.len()) && bytes.len() == link.size as usize,
            "companion size mismatch",
        )?;
        require(
            crc32(&bytes) == link.checksum,
            "companion outer checksum mismatch",
        )?;
        require(
            &bytes[..8] == b"PEEX\x01\0\x60\0",
            "unsupported companion header",
        )?;
        require(
            u32_at(&bytes, 8) == link.generation && u32_at(&bytes, 12) as usize == bytes.len(),
            "companion generation/size mismatch",
        )?;
        require(
            text(&bytes[20..84])? == name.as_str(),
            "companion belongs to another card",
        )?;
        let mut check = bytes.clone();
        check[88..92].fill(0);
        require(
            crc32(&check) == u32_at(&bytes, 88),
            "companion internal checksum mismatch",
        )?;
        let count = usize::from(u16_at(&bytes, 16));
        let mut next = 96 + count * 64;
        require(
            count <= 16
                && count == usize::from(link.sections)
                && u16_at(&bytes, 18) == 0
                && next <= bytes.len(),
            "invalid companion directory",
        )?;
        let mut sections: Vec<Section> = vec![];
        for i in 0..count {
            let at = 96 + i * 64;
            let key = text(&bytes[at..at + 48])?;
            require(
                !key.is_empty()
                    && key.bytes().all(|b| {
                        b.is_ascii_lowercase() || b.is_ascii_digit() || b"_-/.".contains(&b)
                    }),
                "invalid companion section key",
            )?;
            require(
                sections.last().map_or(true, |s| s.key.as_str() < key),
                "duplicate or unsorted companion section",
            )?;
            let version = u16_at(&bytes, at + 48);
            let reader = u16_at(&bytes, at + 50);
            let size = u32_at(&bytes, at + 56) as usize;
            require(
                version != 0
                    && reader != 0
                    && size != 0
                    && u32_at(&bytes, at + 52) as usize == next
                    && size <= bytes.len() - next,
                "invalid companion section bounds",
            )?;
            let data = bytes[next..next + size].to_vec();
            require(
                crc32(&data) == u32_at(&bytes, at + 60),
                "companion section checksum mismatch",
            )?;
            sections.push(Section {
                key: key.into(),
                version,
                reader,
                data,
            });
            next += size;
        }
        require(next == bytes.len(), "trailing companion bytes")?;
        Ok(Self { bytes, sections })
    }
    pub fn bytes(&self) -> &[u8] {
        &self.bytes
    }
    pub fn sections(&self) -> &[Section] {
        &self.sections
    }
}
