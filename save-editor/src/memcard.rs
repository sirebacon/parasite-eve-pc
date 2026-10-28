use std::fs;
use std::path::{Path, PathBuf};

use anyhow::{bail, Context, Result};

use crate::crc::{update_mc_frame_xor, SAVE_BLOCK_SIZE};
use crate::save::PeSaveSlot;

pub const PS1_CARD_SIZE: usize = 0x20000; // 131,072 bytes
pub const GME_HEADER_SIZE: usize = 0x0F40; // 3,904 bytes
pub const GME_CARD_SIZE: usize = PS1_CARD_SIZE + GME_HEADER_SIZE; // 134,976 bytes
pub const MCS_SAVE_SIZE: usize = SAVE_BLOCK_SIZE + 0x80; // 8,320 bytes
pub const MAX_CARD_SLOTS: usize = 15;

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum ContainerKind {
    /// PsyZ host directory (`bu00/` or `bu10/`) containing `BASLUS-00662000000<0|1><A..O>` files
    PsyzDirectory,
    /// Single raw `0x2000`-byte PsyZ save file
    RawSlotFile,
    /// Single `.mcs` `0x2080`-byte PS1 save file (128-byte directory header + `0x2000` block)
    McsSingleFile,
    /// Standard 128 KB PS1 memory card image (`.srm`, `.mcr`, `.mcd`)
    Ps1CardImage,
    /// DexDrive `.gme` memory card image (`0xF40` header + 128 KB card)
    DexDriveGmeImage,
}

impl ContainerKind {
    pub fn label(&self) -> &'static str {
        match self {
            Self::PsyzDirectory => "PsyZ Directory (bu00 / bu10)",
            Self::RawSlotFile => "Raw PsyZ Save Slot (8 KB)",
            Self::McsSingleFile => "PS1 Single Save (.mcs)",
            Self::Ps1CardImage => "PS1 Memory Card Image (.srm / .mcr / .mcd)",
            Self::DexDriveGmeImage => "DexDrive Memory Card (.gme)",
        }
    }
}

#[derive(Debug, Clone)]
pub struct MemoryCardContainer {
    pub path: PathBuf,
    pub kind: ContainerKind,
    /// 15 slots (`0..15` corresponding to `'A'..'O'`)
    pub slots: [Option<PeSaveSlot>; MAX_CARD_SLOTS],
    /// Underlying raw card bytes for `.srm`/`.mcr`/`.gme`/`.mcs` so non-PE slots and headers are preserved
    raw_container_bytes: Vec<u8>,
}

impl MemoryCardContainer {
    /// Opens a PsyZ directory (`bu00`/`bu10`), a raw PsyZ slot file, a `.mcs` single save,
    /// or a full PS1 memory card image (`.srm`, `.mcr`, `.mcd`, `.gme`).
    pub fn open(path: impl AsRef<Path>) -> Result<Self> {
        let path = path.as_ref().to_path_buf();
        if path.is_dir() {
            return Self::open_psyz_dir(path);
        }
        if !path.exists() {
            bail!("Path does not exist: {}", path.display());
        }
        let bytes = fs::read(&path)
            .with_context(|| format!("Failed to read file {}", path.display()))?;

        match bytes.len() {
            SAVE_BLOCK_SIZE => {
                let fname = path
                    .file_name()
                    .and_then(|s| s.to_str())
                    .unwrap_or("BASLUS-006620000000A")
                    .to_string();
                let slot_idx = slot_index_from_filename(&fname).unwrap_or(0);
                let slot = PeSaveSlot::from_bytes(&bytes, slot_idx, fname)?;
                let mut slots: [Option<PeSaveSlot>; MAX_CARD_SLOTS] = Default::default();
                slots[slot_idx] = Some(slot);
                Ok(Self {
                    path,
                    kind: ContainerKind::RawSlotFile,
                    slots,
                    raw_container_bytes: bytes,
                })
            }
            MCS_SAVE_SIZE => {
                let dir_frame = &bytes[..0x80];
                let fname = extract_ascii_filename(&dir_frame[0x0A..0x1E]);
                let slot_idx = slot_index_from_filename(&fname).unwrap_or(0);
                let slot = PeSaveSlot::from_bytes(&bytes[0x80..], slot_idx, fname)?;
                let mut slots: [Option<PeSaveSlot>; MAX_CARD_SLOTS] = Default::default();
                slots[slot_idx] = Some(slot);
                Ok(Self {
                    path,
                    kind: ContainerKind::McsSingleFile,
                    slots,
                    raw_container_bytes: bytes,
                })
            }
            PS1_CARD_SIZE => Self::from_ps1_card_bytes(path, ContainerKind::Ps1CardImage, bytes, 0),
            GME_CARD_SIZE => Self::from_ps1_card_bytes(
                path,
                ContainerKind::DexDriveGmeImage,
                bytes,
                GME_HEADER_SIZE,
            ),
            other => bail!(
                "Unsupported save file size ({} bytes) for {}. Expected 8192 (PsyZ slot), 8320 (.mcs), 131072 (.srm/.mcr/.mcd), or 134976 (.gme).",
                other,
                path.display()
            ),
        }
    }

    pub fn open_psyz_dir(dir: PathBuf) -> Result<Self> {
        if !dir.exists() {
            fs::create_dir_all(&dir)
                .with_context(|| format!("Failed to create PsyZ directory {}", dir.display()))?;
        }
        let mut slots: [Option<PeSaveSlot>; MAX_CARD_SLOTS] = Default::default();

        let mut entries: Vec<_> = fs::read_dir(&dir)
            .with_context(|| format!("Failed to read PsyZ directory {}", dir.display()))?
            .filter_map(|e| e.ok())
            .collect();
        entries.sort_by_key(|e| e.file_name());

        for entry in entries {
            let file_path = entry.path();
            if !file_path.is_file() {
                continue;
            }
            let fname = entry.file_name().to_string_lossy().to_string();
            let meta = match entry.metadata() {
                Ok(m) => m,
                Err(_) => continue,
            };
            if meta.len() < SAVE_BLOCK_SIZE as u64 {
                continue;
            }
            let bytes = match fs::read(&file_path) {
                Ok(b) => b,
                Err(_) => continue,
            };
            if bytes.len() < SAVE_BLOCK_SIZE || bytes[0] != b'S' || bytes[1] != b'C' {
                continue;
            }

            let slot_idx = slot_index_from_filename(&fname).unwrap_or_else(|| {
                // Fallback for 19-char truncated PsyZ filename (`BASLUS-006620000000`): find first free slot
                slots.iter().position(|s| s.is_none()).unwrap_or(0)
            });

            if let Ok(slot) = PeSaveSlot::from_bytes(&bytes[..SAVE_BLOCK_SIZE], slot_idx, &fname) {
                slots[slot_idx] = Some(slot);
            }
        }

        Ok(Self {
            path: dir,
            kind: ContainerKind::PsyzDirectory,
            slots,
            raw_container_bytes: Vec::new(),
        })
    }

    fn from_ps1_card_bytes(
        path: PathBuf,
        kind: ContainerKind,
        bytes: Vec<u8>,
        card_offset: usize,
    ) -> Result<Self> {
        let card = &bytes[card_offset..card_offset + PS1_CARD_SIZE];
        if &card[0..2] != b"MC" {
            bail!(
                "Invalid PS1 Memory Card header in {}: expected 'MC' at offset 0x{:X}",
                path.display(),
                card_offset
            );
        }

        let mut slots: [Option<PeSaveSlot>; MAX_CARD_SLOTS] = Default::default();
        for i in 0..MAX_CARD_SLOTS {
            let dir_offset = (i + 1) * 0x80;
            let dir_frame = &card[dir_offset..dir_offset + 0x80];
            let alloc_state = dir_frame[0];
            // 0x51 = active first block of a save
            if alloc_state != 0x51 {
                continue;
            }
            let fname = extract_ascii_filename(&dir_frame[0x0A..0x1E]);
            let data_offset = (i + 1) * SAVE_BLOCK_SIZE;
            let block = &card[data_offset..data_offset + SAVE_BLOCK_SIZE];
            if block[0] != b'S' || block[1] != b'C' {
                continue;
            }
            // Check if it is a Parasite Eve save (or parse any valid PE-compatible block)
            if fname.starts_with("BASLUS-00662")
                || fname.starts_with("BISLPS-0123")
                || fname.is_empty()
            {
                let slot_idx = slot_index_from_filename(&fname).unwrap_or(i);
                let target_idx = if slots[slot_idx].is_none() { slot_idx } else { i };
                let normalized_name = if fname.is_empty() {
                    format!("BASLUS-006620000000{}", (b'A' + target_idx as u8) as char)
                } else {
                    fname
                };
                if let Ok(mut parsed) = PeSaveSlot::from_bytes(block, target_idx, normalized_name) {
                    parsed.slot_index = target_idx;
                    slots[target_idx] = Some(parsed);
                }
            }
        }

        Ok(Self {
            path,
            kind,
            slots,
            raw_container_bytes: bytes,
        })
    }

    /// Saves all modified slots back to the current container on disk (`bu00` directory, `.srm`, `.mcr`, `.gme`, `.mcs`, or raw file).
    pub fn save(&mut self) -> Result<()> {
        self.validate_slots()?;
        match self.kind {
            ContainerKind::PsyzDirectory => {
                fs::create_dir_all(&self.path)?;
                for i in 0..MAX_CARD_SLOTS {
                    let slot_letter = (b'A' + i as u8) as char;
                    let path_normal = self
                        .path
                        .join(format!("BASLUS-006620000000{}", slot_letter));
                    let path_clear = self
                        .path
                        .join(format!("BASLUS-006620000001{}", slot_letter));

                    if let Some(slot) = &mut self.slots[i] {
                        slot.slot_index = i;
                        let data = slot.to_bytes();
                        let target_path = self.path.join(&slot.filename);
                        // Remove alternate style file if it existed
                        if path_normal != target_path && path_normal.exists() {
                            let _ = fs::remove_file(&path_normal);
                        }
                        if path_clear != target_path && path_clear.exists() {
                            let _ = fs::remove_file(&path_clear);
                        }
                        fs::write(&target_path, &data).with_context(|| {
                            format!("Failed to write PsyZ slot file {}", target_path.display())
                        })?;
                    }
                }
                Ok(())
            }
            ContainerKind::RawSlotFile => {
                let active_slot = self
                    .slots
                    .iter_mut()
                    .find_map(|s| s.as_mut())
                    .ok_or_else(|| anyhow::anyhow!("No active save slot to write"))?;
                let bytes = active_slot.to_bytes();
                self.raw_container_bytes = bytes.clone();
                fs::write(&self.path, &bytes)
                    .with_context(|| format!("Failed to write {}", self.path.display()))
            }
            ContainerKind::McsSingleFile => {
                let active_slot = self
                    .slots
                    .iter_mut()
                    .find_map(|s| s.as_mut())
                    .ok_or_else(|| anyhow::anyhow!("No active save slot to write"))?;
                let bytes = active_slot.to_bytes();
                if self.raw_container_bytes.len() < MCS_SAVE_SIZE {
                    self.raw_container_bytes.resize(MCS_SAVE_SIZE, 0);
                }
                write_ps1_dir_frame(
                    &mut self.raw_container_bytes[0..0x80],
                    &active_slot.filename,
                );
                self.raw_container_bytes[0x80..MCS_SAVE_SIZE].copy_from_slice(&bytes);
                fs::write(&self.path, &self.raw_container_bytes)
                    .with_context(|| format!("Failed to write {}", self.path.display()))
            }
            ContainerKind::Ps1CardImage | ContainerKind::DexDriveGmeImage => {
                let card_offset = if self.kind == ContainerKind::DexDriveGmeImage {
                    GME_HEADER_SIZE
                } else {
                    0
                };
                if self.raw_container_bytes.len() < card_offset + PS1_CARD_SIZE {
                    self.raw_container_bytes = create_blank_ps1_card();
                }
                let card =
                    &mut self.raw_container_bytes[card_offset..card_offset + PS1_CARD_SIZE];
                for i in 0..MAX_CARD_SLOTS {
                    if let Some(slot) = &mut self.slots[i] {
                        slot.slot_index = i;
                        let block_bytes = slot.to_bytes();
                        let dir_offset = (i + 1) * 0x80;
                        write_ps1_dir_frame(
                            &mut card[dir_offset..dir_offset + 0x80],
                            &slot.filename,
                        );
                        let data_offset = (i + 1) * SAVE_BLOCK_SIZE;
                        card[data_offset..data_offset + SAVE_BLOCK_SIZE]
                            .copy_from_slice(&block_bytes);
                    }
                }
                fs::write(&self.path, &self.raw_container_bytes)
                    .with_context(|| format!("Failed to write {}", self.path.display()))
            }
        }
    }

    /// Exports all active slots into a PsyZ memory card directory (`bu00/` or `bu10/`).
    /// Also writes a 19-char fallback copy (`BASLUS-006620000000`) for Slot A if requested,
    /// though standard 20-char `BASLUS-00662000000<0|1><A..O>` files are always written.
    pub fn export_to_psyz_dir(&mut self, target_dir: impl AsRef<Path>) -> Result<Vec<PathBuf>> {
        self.validate_slots()?;
        let target_dir = target_dir.as_ref();
        fs::create_dir_all(target_dir).with_context(|| {
            format!(
                "Failed to create target PsyZ directory {}",
                target_dir.display()
            )
        })?;

        let mut written_files = Vec::new();
        for i in 0..MAX_CARD_SLOTS {
            if let Some(slot) = &mut self.slots[i] {
                slot.slot_index = i;
                let data = slot.to_bytes();
                let out_path = target_dir.join(&slot.filename);
                fs::write(&out_path, &data).with_context(|| {
                    format!("Failed to write PsyZ slot file {}", out_path.display())
                })?;
                written_files.push(out_path);
            }
        }
        Ok(written_files)
    }

    /// Exports all active slots into a standard 128 KB PS1 Memory Card image (`.srm` / `.mcr` / `.mcd`).
    pub fn export_to_ps1_card_image(&mut self, target_file: impl AsRef<Path>) -> Result<()> {
        self.validate_slots()?;
        let target_file = target_file.as_ref();
        let mut card = create_blank_ps1_card();
        for i in 0..MAX_CARD_SLOTS {
            if let Some(slot) = &mut self.slots[i] {
                slot.slot_index = i;
                let block_bytes = slot.to_bytes();
                let dir_offset = (i + 1) * 0x80;
                write_ps1_dir_frame(&mut card[dir_offset..dir_offset + 0x80], &slot.filename);
                let data_offset = (i + 1) * SAVE_BLOCK_SIZE;
                card[data_offset..data_offset + SAVE_BLOCK_SIZE].copy_from_slice(&block_bytes);
            }
        }
        if let Some(parent) = target_file.parent() {
            if !parent.as_os_str().is_empty() {
                fs::create_dir_all(parent)?;
            }
        }
        fs::write(target_file, &card)
            .with_context(|| format!("Failed to write PS1 card image {}", target_file.display()))
    }

    fn validate_slots(&self) -> Result<()> {
        for (i, slot) in self.slots.iter().enumerate() {
            if let Some(slot) = slot {
                slot.validate_inventory().with_context(|| format!("Save slot {}", i + 1))?;
            }
        }
        Ok(())
    }

    pub fn active_slot_indices(&self) -> Vec<usize> {
        self.slots
            .iter()
            .enumerate()
            .filter_map(|(i, s)| s.as_ref().map(|_| i))
            .collect()
    }

    pub fn first_active_slot_index(&self) -> Option<usize> {
        self.slots.iter().position(|s| s.is_some())
    }

    pub fn clone_slot(&mut self, from_idx: usize, to_idx: usize) -> Result<()> {
        if from_idx >= MAX_CARD_SLOTS || to_idx >= MAX_CARD_SLOTS {
            bail!("Slot index out of range (0..14)");
        }
        let mut cloned = self.slots[from_idx]
            .clone()
            .ok_or_else(|| anyhow::anyhow!("Source slot {} is empty", from_idx + 1))?;
        cloned.slot_index = to_idx;
        let _ = cloned.to_bytes();
        self.slots[to_idx] = Some(cloned);
        Ok(())
    }

    pub fn delete_slot(&mut self, idx: usize) -> Result<()> {
        if idx >= MAX_CARD_SLOTS {
            bail!("Slot index out of range (0..14)");
        }
        if self.kind == ContainerKind::PsyzDirectory {
            if let Some(slot) = &self.slots[idx] {
                let p = self.path.join(&slot.filename);
                if p.exists() {
                    let _ = fs::remove_file(p);
                }
            }
        }
        self.slots[idx] = None;
        Ok(())
    }
}

pub fn slot_index_from_filename(filename: &str) -> Option<usize> {
    let bytes = filename.as_bytes();
    if bytes.len() >= 20 && filename.starts_with("BASLUS-00662") {
        let last = bytes[19];
        if (b'A'..=b'O').contains(&last) {
            return Some((last - b'A') as usize);
        }
    }
    None
}

fn extract_ascii_filename(raw: &[u8]) -> String {
    let end = raw.iter().position(|&b| b == 0).unwrap_or(raw.len());
    String::from_utf8_lossy(&raw[..end]).trim().to_string()
}

fn write_ps1_dir_frame(frame: &mut [u8], filename: &str) {
    frame.fill(0);
    frame[0x00] = 0x51; // Active single/first block
    frame[0x04..0x08].copy_from_slice(&(SAVE_BLOCK_SIZE as u32).to_le_bytes());
    frame[0x08..0x0A].copy_from_slice(&0xFFFFu16.to_le_bytes()); // Next block pointer = end of chain
    let name_bytes = filename.as_bytes();
    let copy_len = name_bytes.len().min(20);
    frame[0x0A..0x0A + copy_len].copy_from_slice(&name_bytes[..copy_len]);
    update_mc_frame_xor(frame);
}

pub fn create_blank_ps1_card() -> Vec<u8> {
    let mut card = vec![0u8; PS1_CARD_SIZE];
    // Frame 0: "MC" header
    card[0] = b'M';
    card[1] = b'C';
    card[0x7F] = b'M' ^ b'C';

    // Frames 1..=15: Free directory frames (0xA0)
    for i in 1..=MAX_CARD_SLOTS {
        let off = i * 0x80;
        card[off] = 0xA0;
        card[off + 0x08] = 0xFF;
        card[off + 0x09] = 0xFF;
        update_mc_frame_xor(&mut card[off..off + 0x80]);
    }

    // Frames 16..=35: Broken sector list (0xFFFFFFFF)
    for i in 16..=35 {
        let off = i * 0x80;
        card[off..off + 4].copy_from_slice(&0xFFFF_FFFFu32.to_le_bytes());
        card[off + 0x08] = 0xFF;
        card[off + 0x09] = 0xFF;
        update_mc_frame_xor(&mut card[off..off + 0x80]);
    }

    // Frame 63: Write test frame (copy of Frame 0)
    let f63 = 63 * 0x80;
    card[f63] = b'M';
    card[f63 + 1] = b'C';
    card[f63 + 0x7F] = b'M' ^ b'C';

    card
}
