use anyhow::{bail, Result};
use serde::{Deserialize, Serialize};

use crate::charset::{decode_sjis_title, format_sjis_title_bytes, pe_decode, pe_encode_fixed};
use crate::crc::{
    calc_save_crc16, read_stored_crc, update_save_crc16, SAVE_BLOCK_SIZE, SAVE_CRC_OFFSET,
};
use crate::disc::{
    game_data, hp_with_armor, HP_STAT, ITEM_CAPACITY_STAT, MAX_STAT_BAG_SLOTS, MAX_STAT_MAX_HP,
    TOP_LEVEL_STAT,
};
use crate::tables::{
    encode_mod_byte, game_icon_and_kind, item_metadata, map_location_descriptive, mod_metadata,
    save_header_location_name, ItemCategory, BAG_CAPACITY_BY_LEVEL, EQUIPMENT_TEMPLATES, PARASITE_SPELLS,
};

pub const RT_OFFSET: usize = 0x0100;
pub const TAIL_OFFSET: usize = 0x13E4;
pub const SCRIPT_VAR_OFFSET: usize = 0x13E4;
pub const SCRIPT_VAR_SIZE: usize = 0x0800;
pub const BATTLE_STATE_OFFSET: usize = 0x1BFC;
/// Item Capacity stat value of the top level (98), which gives `MAX_STAT_BAG_SLOTS`.
pub const MAX_BAG_ITEM_CAPACITY_STAT: u16 = TOP_LEVEL_STAT;
/// Armor mod id of HP Max Up (`AMOD_HP_MAX_UP`).
const ARMOR_MOD_HP_MAX_UP: u8 = 0x0B;

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct EquipmentRecord {
    pub index: usize, // 0..128 (inventory value = 0x100 + index)
    pub icon_id: u8,
    pub tail_capacity: u8,
    pub reserved02: [u8; 2],
    pub item_id: u8,
    pub flags: u8,
    pub kind: u8,
    pub base_stats: [u8; 3],
    pub ammo: u16,
    pub reserve_ammo: u16,
    pub bonus_stats: [i16; 3],
    pub tail_count: u8,
    pub tail_data: [u8; 11],
}

impl EquipmentRecord {
    pub fn empty(index: usize) -> Self {
        Self {
            index,
            icon_id: 0,
            tail_capacity: 0,
            reserved02: [0; 2],
            item_id: 0,
            flags: 0,
            kind: 0,
            base_stats: [0; 3],
            ammo: 0,
            reserve_ammo: 0,
            bonus_stats: [0; 3],
            tail_count: 0,
            tail_data: [0; 11],
        }
    }

    /// A new record for `item_id`, made the way the game makes one: a verbatim copy of the item's
    /// base record (`Inv_FindSlotByIndex`). The verified USA templates also work
    /// without a mounted disc; no guessed stats or missing factory effects.
    pub fn from_item_template(index: usize, item_id: u8) -> Self {
        if let Some(bytes) = game_data().and_then(|g| g.item_record(item_id)) {
            let mut record = Self::parse(index, &bytes);
            record.tail_capacity = record.tail_capacity.min(10);
            return record;
        }
        if (0x3F..=0xC7).contains(&item_id) {
            let mut record = Self::parse(index, &EQUIPMENT_TEMPLATES[(item_id - 0x3F) as usize]);
            // The unused retail Debug SMG has a capacity of 100, beyond the
            // record's ten editable effect slots and the port's save bounds.
            record.tail_capacity = record.tail_capacity.min(10);
            record
        } else {
            Self::empty(index)
        }
    }

    /// Icon (bag picture) and kind (weapon class: the model Aya holds and how it fires) for
    /// `item_id`: from the disc when loaded, else the game's value for the item's category.
    pub fn icon_and_kind_for(item_id: u8) -> (u8, u8) {
        if let Some(rec) = game_data().and_then(|g| g.item_record(item_id)) {
            return (rec[0x00], rec[0x06]);
        }
        let meta = item_metadata(item_id);
        game_icon_and_kind(meta.category).unwrap_or((meta.default_icon_id.max(1), meta.default_kind))
    }

    pub fn is_active(&self) -> bool {
        self.icon_id != 0 && self.item_id != 0
    }

    pub fn is_engraved(&self) -> bool {
        (self.flags & 0x10) != 0
    }

    pub fn set_engraved(&mut self, engraved: bool) {
        if engraved {
            self.flags |= 0x10;
        } else {
            self.flags &= !0x10;
        }
    }

    pub fn is_armor(&self) -> bool {
        self.kind == 9 || item_metadata(self.item_id).category.is_armor()
    }

    pub fn is_weapon(&self) -> bool {
        !self.is_armor() && self.is_active()
    }

    pub fn display_name(&self) -> &'static str {
        item_metadata(self.item_id).display_name
    }

    pub fn effective_stats(&self) -> [i32; 3] {
        [
            self.base_stats[0] as i32 + self.bonus_stats[0] as i32,
            self.base_stats[1] as i32 + self.bonus_stats[1] as i32,
            self.base_stats[2] as i32 + self.bonus_stats[2] as i32,
        ]
    }

    pub fn mod_id_at(&self, slot: usize) -> u8 {
        if slot < 11 {
            self.tail_data[slot] & 0x1F
        } else {
            0
        }
    }

    pub fn set_mod_at(&mut self, slot: usize, mod_id: u8) {
        if slot < 11 {
            let is_armor = self.is_armor();
            self.tail_data[slot] = encode_mod_byte(mod_id, is_armor);
        }
    }

    pub fn parse(index: usize, bytes: &[u8]) -> Self {
        let mut reserved02 = [0u8; 2];
        reserved02.copy_from_slice(&bytes[0x02..0x04]);
        let mut base_stats = [0u8; 3];
        base_stats.copy_from_slice(&bytes[0x07..0x0A]);
        let ammo = u16::from_le_bytes([bytes[0x0A], bytes[0x0B]]);
        let reserve_ammo = u16::from_le_bytes([bytes[0x0C], bytes[0x0D]]);
        let bonus_stats = [
            i16::from_le_bytes([bytes[0x0E], bytes[0x0F]]),
            i16::from_le_bytes([bytes[0x10], bytes[0x11]]),
            i16::from_le_bytes([bytes[0x12], bytes[0x13]]),
        ];
        let mut tail_data = [0u8; 11];
        tail_data.copy_from_slice(&bytes[0x15..0x20]);

        Self {
            index,
            icon_id: bytes[0x00],
            tail_capacity: bytes[0x01],
            reserved02,
            item_id: bytes[0x04],
            flags: bytes[0x05],
            kind: bytes[0x06],
            base_stats,
            ammo,
            reserve_ammo,
            bonus_stats,
            tail_count: bytes[0x14],
            tail_data,
        }
    }

    pub fn write(&self, out: &mut [u8]) {
        out[0x00] = self.icon_id;
        out[0x01] = self.tail_capacity;
        out[0x02..0x04].copy_from_slice(&self.reserved02);
        out[0x04] = self.item_id;
        out[0x05] = self.flags;
        out[0x06] = self.kind;
        out[0x07..0x0A].copy_from_slice(&self.base_stats);
        out[0x0A..0x0C].copy_from_slice(&self.ammo.to_le_bytes());
        out[0x0C..0x0E].copy_from_slice(&self.reserve_ammo.to_le_bytes());
        for (i, bonus) in self.bonus_stats.iter().enumerate() {
            let off = 0x0E + i * 2;
            out[off..off + 2].copy_from_slice(&bonus.to_le_bytes());
        }
        out[0x14] = self.tail_count;
        out[0x15..0x20].copy_from_slice(&self.tail_data);
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct PeSaveSlot {
    /// Optional slot number on memory card (0..15)
    pub slot_index: usize,
    /// Optional PsyZ / PS1 20-char filename (`BASLUS-006620000000A`)
    pub filename: String,
    /// Whether this save is marked as Clear Data / EX Game (`titleStyleFlag != 0`)
    pub is_clear_data: bool,
    /// Decoded Shift-JIS header title (`MemCardFileHeader.title`)
    pub header_title: String,
    /// Automatically re-format the Shift-JIS header title when saving
    pub auto_update_header_title: bool,

    // SaveRuntimeState top-level fields (`0x0100..0x0120`)
    pub save_name: String,
    pub play_time_seconds: i32,
    pub game_time_seconds: i32,
    pub aya_name: String,
    pub menu_layout: u8,
    pub draw_base_y: i8,

    // AyaSaveState (`0x0120..0x01CC`)
    pub total_exp: u32,
    pub max_hp: u16,
    pub current_hp: u16,
    /// 0-based internal level (`0..=98` -> display Lv `1..=99`)
    pub level: u8,
    /// EX Game round counter (`0` = 1st Playthrough, `1` = 2nd Round, etc.)
    pub new_game_count: u8,
    /// Aya's own bag size (`10..=46`), set by the game from the Item Capacity stat; equipped
    /// armor mods add up to 50 at run time
    pub inventory_slot_count: u8,
    /// Bonus Points (`0..=99999`)
    pub bonus_points: u32,
    /// PE energy pool bonus
    pub pe_bonus_pool: u16,
    /// Equipped weapon slot index in `inventory_items` (`0..49` or `-1`)
    pub equipped_weapon_slot: i8,
    /// Equipped armor slot index in `inventory_items` (`0..49` or `-1`)
    pub equipped_armor_slot: i8,
    /// 20-bit bitmask of unlocked Parasite Energy spells
    pub parasite_spell_flags: u32,
    /// 7 stat growth levels (HP, Offense, Defense, P.Energy, Status Rec, ATB, Item Cap; base = level * 10, max = 1000)
    pub stats_levels: [u16; 7],
    /// Story day shown on save card (`1..=6`)
    pub play_time_class: i16,
    /// Current room / map number (`1..=437`, or `< 0` for Clear Data)
    pub current_map_number: i16,
    /// Menu clamp value (`rt + 0x60`)
    pub menu_clamp_value: i16,
    /// UI window RGB color (`0x00BBGGRR`)
    pub draw_blend_color: u32,

    /// Aya's 50 inventory slots (`0` = empty, `1..=255` = base item ID, `0x100..=0x17F` = equipment index, `0x200..=0x208` = special ammo/crate)
    pub inventory_items: Vec<i16>,
    /// 128 equipment records (`rt + 0xCC..0x10CC`)
    pub equipment: Vec<EquipmentRecord>,
    /// 6 ammo category counts (`[0]` 9mm/Bullets, `[1]` Rockets, `[2]` DNA Bullets)
    pub category_counts: [u16; 6],
    /// Wayne's 100 regular item storage slots (`rt + 0x10D8..0x11A0`)
    pub storage: Vec<i16>,
    /// Wayne's 82 equipment storage slots (`rt + 0x11A0..0x1244`)
    pub special_storage: Vec<i16>,

    // Tail fields (`0x13E4..0x1C8C`)
    pub room_type: u16,
    pub requested_entity_bank: u16,
    pub current_story_day: i8,
    pub pending_story_day: i8,
    pub story_day_flags: u8,
    pub fade_mode: u8,

    // Checksum diagnostics
    pub stored_crc: u16,
    pub calculated_crc: u16,

    /// Full raw `0x2000` bytes preserved so untouched script flags, TIM icon, and tail data are 100% lossless
    #[serde(with = "hex_bytes_serde")]
    pub raw_bytes: Vec<u8>,
}

mod hex_bytes_serde {
    use serde::{Deserialize, Deserializer, Serializer};

    pub fn serialize<S>(bytes: &Vec<u8>, serializer: S) -> Result<S::Ok, S::Error>
    where
        S: Serializer,
    {
        let mut s = String::with_capacity(bytes.len() * 2);
        for b in bytes {
            use std::fmt::Write;
            let _ = write!(&mut s, "{:02X}", b);
        }
        serializer.serialize_str(&s)
    }

    pub fn deserialize<'de, D>(deserializer: D) -> Result<Vec<u8>, D::Error>
    where
        D: Deserializer<'de>,
    {
        let s = String::deserialize(deserializer)?;
        if s.len() % 2 != 0 {
            return Err(serde::de::Error::custom("hex string length must be even"));
        }
        let mut out = Vec::with_capacity(s.len() / 2);
        for i in (0..s.len()).step_by(2) {
            let byte = u8::from_str_radix(&s[i..i + 2], 16)
                .map_err(serde::de::Error::custom)?;
            out.push(byte);
        }
        Ok(out)
    }
}

impl PeSaveSlot {
    pub fn from_bytes(
        raw: &[u8],
        slot_index: usize,
        filename: impl Into<String>,
    ) -> Result<Self> {
        if raw.len() < SAVE_BLOCK_SIZE {
            bail!(
                "Save block is too short: expected {} bytes (0x2000), got {} bytes",
                SAVE_BLOCK_SIZE,
                raw.len()
            );
        }
        let block = &raw[..SAVE_BLOCK_SIZE];
        if block[0] != b'S' || block[1] != b'C' {
            bail!(
                "Invalid PS1 save header magic: expected 'SC' (0x53, 0x43), got {:02X} {:02X}",
                block[0],
                block[1]
            );
        }

        let filename = filename.into();
        let (_fullwidth_title, header_title) = decode_sjis_title(&block[0x04..0x44]);
        let is_clear_data = filename.as_bytes().get(18) == Some(&b'1')
            || header_title.contains("Clear");

        let rt = &block[RT_OFFSET..];
        let save_name = pe_decode(&rt[0x00..0x08]);
        let play_time_seconds = i32::from_le_bytes(rt[0x08..0x0C].try_into().unwrap());
        let game_time_seconds = i32::from_le_bytes(rt[0x0C..0x10].try_into().unwrap());
        let aya_name = pe_decode(&rt[0x10..0x1D]);
        let menu_layout = rt[0x1D];
        let draw_base_y = rt[0x1F] as i8;

        let total_exp = u32::from_le_bytes(rt[0x20..0x24].try_into().unwrap());
        let max_hp = u16::from_le_bytes(rt[0x26..0x28].try_into().unwrap());
        let current_hp = u16::from_le_bytes(rt[0x28..0x2A].try_into().unwrap());
        let level = rt[0x2A];
        let new_game_count = rt[0x2B];
        let inventory_slot_count = rt[0x2C];
        let bonus_points = u32::from_le_bytes(rt[0x30..0x34].try_into().unwrap());
        let pe_bonus_pool = u16::from_le_bytes(rt[0x3E..0x40].try_into().unwrap());
        let equipped_weapon_slot = rt[0x40] as i8;
        let equipped_armor_slot = rt[0x42] as i8;
        let parasite_spell_flags = u32::from_le_bytes(rt[0x44..0x48].try_into().unwrap());

        let mut stats_levels = [0u16; 7];
        for i in 0..7 {
            let off = 0x48 + i * 2;
            stats_levels[i] = u16::from_le_bytes(rt[off..off + 2].try_into().unwrap());
        }

        let play_time_class = i16::from_le_bytes(rt[0x5C..0x5E].try_into().unwrap());
        let current_map_number = i16::from_le_bytes(rt[0x5E..0x60].try_into().unwrap());
        let menu_clamp_value = i16::from_le_bytes(rt[0x60..0x62].try_into().unwrap());
        let draw_blend_color = u32::from_le_bytes(rt[0x64..0x68].try_into().unwrap());

        let mut inventory_items = Vec::with_capacity(50);
        for i in 0..50 {
            let off = 0x68 + i * 2;
            inventory_items.push(i16::from_le_bytes(rt[off..off + 2].try_into().unwrap()));
        }

        let mut equipment = Vec::with_capacity(128);
        for i in 0..128 {
            let off = 0xCC + i * 0x20;
            equipment.push(EquipmentRecord::parse(i, &rt[off..off + 0x20]));
        }

        let mut category_counts = [0u16; 6];
        for i in 0..6 {
            let off = 0x10CC + i * 2;
            category_counts[i] = u16::from_le_bytes(rt[off..off + 2].try_into().unwrap());
        }

        let mut storage = Vec::with_capacity(100);
        for i in 0..100 {
            let off = 0x10D8 + i * 2;
            storage.push(i16::from_le_bytes(rt[off..off + 2].try_into().unwrap()));
        }

        let mut special_storage = Vec::with_capacity(82);
        for i in 0..82 {
            let off = 0x11A0 + i * 2;
            special_storage.push(i16::from_le_bytes(rt[off..off + 2].try_into().unwrap()));
        }

        let room_type = u16::from_le_bytes(block[0x1BF4..0x1BF6].try_into().unwrap());
        let requested_entity_bank = u16::from_le_bytes(block[0x1BF6..0x1BF8].try_into().unwrap());
        let current_story_day = block[0x1BF8] as i8;
        let pending_story_day = block[0x1BF9] as i8;
        let story_day_flags = block[0x1BFA];
        let fade_mode = block[0x1BFB];

        let stored_crc = (read_stored_crc(block) & 0xFFFF) as u16;
        let calculated_crc = calc_save_crc16(block);

        Ok(Self {
            slot_index,
            filename,
            is_clear_data,
            header_title,
            auto_update_header_title: true,
            save_name,
            play_time_seconds,
            game_time_seconds,
            aya_name,
            menu_layout,
            draw_base_y,
            total_exp,
            max_hp,
            current_hp,
            level,
            new_game_count,
            inventory_slot_count,
            bonus_points,
            pe_bonus_pool,
            equipped_weapon_slot,
            equipped_armor_slot,
            parasite_spell_flags,
            stats_levels,
            play_time_class,
            current_map_number,
            menu_clamp_value,
            draw_blend_color,
            inventory_items,
            equipment,
            category_counts,
            storage,
            special_storage,
            room_type,
            requested_entity_bank,
            current_story_day,
            pending_story_day,
            story_day_flags,
            fade_mode,
            stored_crc,
            calculated_crc,
            raw_bytes: block.to_vec(),
        })
    }

    /// Check structural references before any save/export writes a container.
    /// Stat bonuses, ammo and legitimate custom effects are left untouched.
    pub fn validate_inventory(&self) -> Result<()> {
        if self.inventory_slot_count > 50 || self.inventory_items.len() != 50
            || self.storage.len() != 100 || self.special_storage.len() != 82
            || self.equipment.len() != 128 {
            bail!("Invalid inventory dimensions");
        }
        for (index, item) in self.equipment.iter().enumerate() {
            if item.index != index {
                bail!("Equipment record {} has an invalid index", index);
            }
            if item.icon_id != 0 && (item.item_id == 0 || item.kind >= 24
                || item.tail_count > 10 || item.tail_capacity > 10) {
                bail!("Equipment record {} has invalid type or modification slots", index);
            }
        }
        let mut seen = [false; 128];
        for list in [&self.inventory_items, &self.storage, &self.special_storage] {
            for &value in list {
                let id = value as u16;
                if id <= 255 || (0x200..=0x208).contains(&id) { continue; }
                if !(0x100..0x180).contains(&id) {
                    bail!("Invalid inventory reference 0x{:04X}", id);
                }
                let index = (id - 0x100) as usize;
                if self.equipment[index].icon_id == 0 || seen[index] {
                    bail!("Equipment record {} is missing or referenced more than once", index);
                }
                seen[index] = true;
            }
        }
        for (armor, slot) in [(false, self.equipped_weapon_slot), (true, self.equipped_armor_slot)] {
            if slot == -1 { continue; }
            if !(0..50).contains(&slot) { bail!("Invalid equipped inventory slot {}", slot); }
            let id = self.inventory_items[slot as usize] as u16;
            let kind = if (0x100..0x180).contains(&id) {
                self.equipment[(id - 0x100) as usize].kind
            } else if (1..=255).contains(&id) {
                item_metadata(id as u8).default_kind
            } else { 0 };
            if if armor { kind != 9 } else { !(1..=8).contains(&kind) } {
                bail!("Equipped slot {} does not contain compatible equipment", slot);
            }
        }
        Ok(())
    }

    /// Serializes all modified fields back into `self.raw_bytes`, syncs `g_AyaBattleState`,
    /// updates the Shift-JIS header title if relevant fields changed, and recalculates the CRC-16 at `0x1C8C`.
    pub fn to_bytes(&mut self) -> Vec<u8> {
        if self.raw_bytes.len() < SAVE_BLOCK_SIZE {
            self.raw_bytes.resize(SAVE_BLOCK_SIZE, 0);
        }
        self.raw_bytes[0] = b'S';
        self.raw_bytes[1] = b'C';
        if self.raw_bytes[2] == 0 {
            self.raw_bytes[2] = 0x11;
        }
        if self.raw_bytes[3] == 0 {
            self.raw_bytes[3] = 0x01;
        }

        let orig_slot_idx = crate::memcard::slot_index_from_filename(&self.filename)
            .unwrap_or(self.slot_index);
        let orig_clear = self.filename.as_bytes().get(18) == Some(&b'1');

        // Update filename style character and slot letter
        let slot_letter = (b'A' + (self.slot_index.min(14) as u8)) as char;
        let style_char = if self.is_clear_data { '1' } else { '0' };
        self.filename = format!("BASLUS-00662000000{}{}", style_char, slot_letter);

        // Check which core fields changed relative to `raw_bytes`
        let orig_play_time =
            i32::from_le_bytes(self.raw_bytes[RT_OFFSET + 0x08..RT_OFFSET + 0x0C].try_into().unwrap());
        let orig_max_hp =
            u16::from_le_bytes(self.raw_bytes[RT_OFFSET + 0x26..RT_OFFSET + 0x28].try_into().unwrap());
        let orig_cur_hp =
            u16::from_le_bytes(self.raw_bytes[RT_OFFSET + 0x28..RT_OFFSET + 0x2A].try_into().unwrap());
        let orig_level = self.raw_bytes[RT_OFFSET + 0x2A];
        let orig_slot_count = self.raw_bytes[RT_OFFSET + 0x2C];
        let orig_day =
            i16::from_le_bytes(self.raw_bytes[RT_OFFSET + 0x5C..RT_OFFSET + 0x5E].try_into().unwrap());
        let orig_map =
            i16::from_le_bytes(self.raw_bytes[RT_OFFSET + 0x5E..RT_OFFSET + 0x60].try_into().unwrap());

        let header_needs_update = self.auto_update_header_title
            && (self.slot_index != orig_slot_idx
                || self.is_clear_data != orig_clear
                || self.play_time_seconds != orig_play_time
                || self.play_time_class != orig_day
                || self.current_map_number != orig_map);

        if header_needs_update {
            let loc = save_header_location_name(self.current_map_number, self.is_clear_data);
            let title_bytes = format_sjis_title_bytes(
                self.slot_index + 1,
                self.play_time_seconds,
                self.play_time_class,
                Some(loc),
                self.is_clear_data,
            );
            self.raw_bytes[0x04..0x44].copy_from_slice(&title_bytes);
            self.header_title = decode_sjis_title(&title_bytes).1;
        }

        let stat_off = RT_OFFSET + 0x48 + ITEM_CAPACITY_STAT * 2;
        let orig_item_capacity =
            u16::from_le_bytes(self.raw_bytes[stat_off..stat_off + 2].try_into().unwrap());
        self.sync_bag_with_item_capacity(orig_slot_count, orig_item_capacity);
        let hp_off = RT_OFFSET + 0x48 + HP_STAT * 2;
        let orig_hp_stat =
            u16::from_le_bytes(self.raw_bytes[hp_off..hp_off + 2].try_into().unwrap());
        self.sync_max_hp_with_hp_stat(orig_max_hp, orig_hp_stat);

        let battle_state_needs_sync = self.max_hp != orig_max_hp
            || self.current_hp != orig_cur_hp
            || self.level != orig_level
            || self.inventory_slot_count != orig_slot_count;

        {
            let rt = &mut self.raw_bytes[RT_OFFSET..];
            if pe_decode(&rt[0x00..0x08]) != self.save_name {
                pe_encode_fixed(&self.save_name, &mut rt[0x00..0x08], 7);
            }
            rt[0x08..0x0C].copy_from_slice(&self.play_time_seconds.to_le_bytes());
            rt[0x0C..0x10].copy_from_slice(&self.game_time_seconds.to_le_bytes());
            if pe_decode(&rt[0x10..0x1D]) != self.aya_name {
                pe_encode_fixed(&self.aya_name, &mut rt[0x10..0x1D], 12);
            }
            rt[0x1D] = self.menu_layout;
            rt[0x1F] = self.draw_base_y as u8;

            rt[0x20..0x24].copy_from_slice(&self.total_exp.to_le_bytes());
            rt[0x26..0x28].copy_from_slice(&self.max_hp.to_le_bytes());
            rt[0x28..0x2A].copy_from_slice(&self.current_hp.to_le_bytes());
            rt[0x2A] = self.level;
            rt[0x2B] = self.new_game_count;
            rt[0x2C] = self.inventory_slot_count;
            rt[0x30..0x34].copy_from_slice(&self.bonus_points.to_le_bytes());
            rt[0x3E..0x40].copy_from_slice(&self.pe_bonus_pool.to_le_bytes());
            rt[0x40] = self.equipped_weapon_slot as u8;
            rt[0x42] = self.equipped_armor_slot as u8;
            rt[0x44..0x48].copy_from_slice(&self.parasite_spell_flags.to_le_bytes());

            for (i, val) in self.stats_levels.iter().enumerate() {
                let off = 0x48 + i * 2;
                rt[off..off + 2].copy_from_slice(&val.to_le_bytes());
            }

            rt[0x5C..0x5E].copy_from_slice(&self.play_time_class.to_le_bytes());
            rt[0x5E..0x60].copy_from_slice(&self.current_map_number.to_le_bytes());
            rt[0x60..0x62].copy_from_slice(&self.menu_clamp_value.to_le_bytes());
            rt[0x64..0x68].copy_from_slice(&self.draw_blend_color.to_le_bytes());

            for i in 0..50 {
                let val = self.inventory_items.get(i).copied().unwrap_or(0);
                let off = 0x68 + i * 2;
                rt[off..off + 2].copy_from_slice(&val.to_le_bytes());
            }

            for i in 0..128 {
                let off = 0xCC + i * 0x20;
                if let Some(rec) = self.equipment.get(i) {
                    rec.write(&mut rt[off..off + 0x20]);
                }
            }

            for (i, val) in self.category_counts.iter().enumerate() {
                let off = 0x10CC + i * 2;
                rt[off..off + 2].copy_from_slice(&val.to_le_bytes());
            }

            for i in 0..100 {
                let val = self.storage.get(i).copied().unwrap_or(0);
                let off = 0x10D8 + i * 2;
                rt[off..off + 2].copy_from_slice(&val.to_le_bytes());
            }

            for i in 0..82 {
                let val = self.special_storage.get(i).copied().unwrap_or(0);
                let off = 0x11A0 + i * 2;
                rt[off..off + 2].copy_from_slice(&val.to_le_bytes());
            }
        }

        // Update Tail & g_AyaBattleState (`0x1BFC..0x1C6C`)
        self.raw_bytes[0x1BF4..0x1BF6].copy_from_slice(&self.room_type.to_le_bytes());
        self.raw_bytes[0x1BF6..0x1BF8].copy_from_slice(&self.requested_entity_bank.to_le_bytes());
        self.raw_bytes[0x1BF8] = self.current_story_day as u8;
        self.raw_bytes[0x1BF9] = self.pending_story_day as u8;
        self.raw_bytes[0x1BFA] = self.story_day_flags;
        self.raw_bytes[0x1BFB] = self.fade_mode;

        if battle_state_needs_sync {
            let display_lvl = (self.level as i16).saturating_add(1);
            self.raw_bytes[BATTLE_STATE_OFFSET + 0x04..BATTLE_STATE_OFFSET + 0x06]
                .copy_from_slice(&display_lvl.to_le_bytes());
            self.raw_bytes[BATTLE_STATE_OFFSET + 0x0C..BATTLE_STATE_OFFSET + 0x0E]
                .copy_from_slice(&self.current_hp.to_le_bytes());
            self.raw_bytes[BATTLE_STATE_OFFSET + 0x0E..BATTLE_STATE_OFFSET + 0x10]
                .copy_from_slice(&self.current_hp.to_le_bytes());
            self.raw_bytes[BATTLE_STATE_OFFSET + 0x1C..BATTLE_STATE_OFFSET + 0x1E]
                .copy_from_slice(&self.max_hp.to_le_bytes());
            let slot_cap = self.inventory_slot_count as i16;
            self.raw_bytes[BATTLE_STATE_OFFSET + 0x26..BATTLE_STATE_OFFSET + 0x28]
                .copy_from_slice(&slot_cap.to_le_bytes());
        }

        // Compute and store CRC-16 at 0x1C8C (32-bit word with upper 16 bits zero)
        let _ = SAVE_CRC_OFFSET;
        let crc = update_save_crc16(&mut self.raw_bytes);
        self.stored_crc = crc;
        self.calculated_crc = crc;

        self.raw_bytes.clone()
    }

    /// The game sets the bag size from the Item Capacity stat whenever it recomputes Aya's stats
    /// (`Inv_RecalcSlotStats`: level ups, equipment changes, the status menu), so an edited bag
    /// size only sticks when the stat gives it. A changed stat sets the bag; otherwise a changed
    /// bag sets the stat. The stat tops out at 46 slots; armor mods add the rest (to 50) at run
    /// time and are not stored here.
    fn sync_bag_with_item_capacity(&mut self, orig_slots: u8, orig_stat: u16) {
        let stat = &mut self.stats_levels[ITEM_CAPACITY_STAT];
        if *stat != orig_stat {
            self.inventory_slot_count = BAG_CAPACITY_BY_LEVEL[(*stat / 10).min(98) as usize];
        } else if self.inventory_slot_count != orig_slots {
            self.inventory_slot_count = self.inventory_slot_count.clamp(10, MAX_STAT_BAG_SLOTS);
            if BAG_CAPACITY_BY_LEVEL[(*stat / 10).min(98) as usize] != self.inventory_slot_count {
                *stat = (BAG_CAPACITY_BY_LEVEL.iter().position(|&n| n == self.inventory_slot_count).unwrap() * 10) as u16;
            }
        }
    }

    /// Whether the equipped armor carries HP Max Up, which lifts max HP by 3/20
    /// (`BattleCmd_LoadWeaponModifiers` reads the mods in the armor's first `tail_count` slots).
    pub fn armor_has_hp_max_up(&self) -> bool {
        Self::has_hp_max_up(self.equipped_armor_slot, &self.inventory_items, |i| {
            self.equipment.get(i).cloned()
        })
    }

    /// `armor_has_hp_max_up` as the save stood when loaded or last written (`raw_bytes`).
    fn raw_armor_has_hp_max_up(&self) -> bool {
        let rt = &self.raw_bytes[RT_OFFSET..];
        let inventory: Vec<i16> = (0..50)
            .map(|i| i16::from_le_bytes([rt[0x68 + i * 2], rt[0x69 + i * 2]]))
            .collect();
        Self::has_hp_max_up(rt[0x42] as i8, &inventory, |i| {
            (i < 128).then(|| EquipmentRecord::parse(i, &rt[0xCC + i * 0x20..0xEC + i * 0x20]))
        })
    }

    fn has_hp_max_up(
        armor_slot: i8,
        inventory: &[i16],
        record: impl Fn(usize) -> Option<EquipmentRecord>,
    ) -> bool {
        let Some(&val) = usize::try_from(armor_slot)
            .ok()
            .and_then(|slot| inventory.get(slot))
        else {
            return false;
        };
        let Some(rec) = usize::try_from(val as i32 - 0x100)
            .ok()
            .and_then(record)
            .filter(|r| r.is_active())
        else {
            return false;
        };
        (0..(rec.tail_count as usize).min(11)).any(|i| rec.mod_id_at(i) == ARMOR_MOD_HP_MAX_UP)
    }

    /// The highest max HP the game allows with the armor Aya has on now.
    pub fn max_hp_limit(&self) -> u16 {
        hp_with_armor(MAX_STAT_MAX_HP, self.armor_has_hp_max_up())
    }

    /// The game recomputes max HP from the HP stat at the start of every battle, on level ups and
    /// in the status menu (`Inv_RecalcSlotStats`), so an edited max HP only sticks when the stat
    /// gives it. A changed stat sets max HP; otherwise a changed max HP sets the stat. The top
    /// is 869, or 999 while the equipped armor has HP Max Up, so putting on or taking off such
    /// armor also resets max HP. Current HP is kept within max HP, and a full bar stays full.
    fn sync_max_hp_with_hp_stat(&mut self, orig_max_hp: u16, orig_stat: u16) {
        let hp_max_up = self.armor_has_hp_max_up();
        let armor_changed = hp_max_up != self.raw_armor_has_hp_max_up();
        let was_full = self.current_hp >= self.max_hp;
        let limit = self.max_hp_limit();
        let stat = &mut self.stats_levels[HP_STAT];
        match game_data() {
            Some(g) => {
                if *stat != orig_stat || armor_changed {
                    self.max_hp = g.max_hp_for_stat(*stat, hp_max_up);
                } else if self.max_hp != orig_max_hp {
                    let wanted = self.max_hp.min(limit);
                    if g.max_hp_for_stat(*stat, hp_max_up) != wanted {
                        if let Some(v) = g.stat_for_max_hp(wanted, hp_max_up) {
                            *stat = v;
                        }
                    }
                    self.max_hp = g.max_hp_for_stat(*stat, hp_max_up);
                }
            }
            None => {
                if self.max_hp != orig_max_hp && self.max_hp >= limit {
                    *stat = (*stat).max(TOP_LEVEL_STAT);
                }
                if *stat >= TOP_LEVEL_STAT
                    && (*stat != orig_stat || self.max_hp != orig_max_hp || armor_changed)
                {
                    self.max_hp = limit;
                }
            }
        }
        self.current_hp = if was_full {
            self.max_hp
        } else {
            self.current_hp.min(self.max_hp)
        };
    }

    pub fn is_crc_valid(&self) -> bool {
        self.stored_crc == self.calculated_crc
    }

    /// 1-based display level (`1..=99`)
    pub fn display_level(&self) -> u8 {
        self.level.saturating_add(1).clamp(1, 99)
    }

    pub fn set_display_level(&mut self, display_level: u8) {
        let clamped = display_level.clamp(1, 99);
        self.level = clamped - 1;
    }

    pub fn formatted_play_time(&self) -> String {
        let total = self.play_time_seconds.max(0);
        let hours = total / 3600;
        let minutes = (total % 3600) / 60;
        let seconds = total % 60;
        format!("{:02}:{:02}:{:02}", hours, minutes, seconds)
    }

    pub fn set_play_time_hms(&mut self, hours: i32, minutes: i32, seconds: i32) {
        self.play_time_seconds =
            hours.max(0) * 3600 + minutes.clamp(0, 59) * 60 + seconds.clamp(0, 59);
    }

    pub fn location_description(&self) -> &'static str {
        if self.is_clear_data {
            "Clear Data (EX Game)"
        } else {
            map_location_descriptive(self.current_map_number)
        }
    }

    /// Formats any inventory or storage slot value (`0`, `1..0xFF`, `0x100..0x17F`, `0x200..0x208`)
    pub fn describe_slot_value(&self, val: i16) -> String {
        if val <= 0 {
            return "--- Empty ---".to_string();
        }
        if (0x100..0x180).contains(&val) {
            let eq_idx = (val - 0x100) as usize;
            if let Some(rec) = self.equipment.get(eq_idx) {
                if rec.is_active() {
                    let engraved = if rec.is_engraved() { " [Wayne]" } else { "" };
                    let eff = rec.effective_stats();
                    if rec.is_armor() {
                        return format!(
                            "[E#{:02X}] {}{} (Def {} / PE {} / Crit {}, Slots {}/{})",
                            eq_idx,
                            rec.display_name(),
                            engraved,
                            eff[0],
                            eff[1],
                            eff[2],
                            rec.tail_count,
                            rec.tail_capacity
                        );
                    } else {
                        return format!(
                            "[E#{:02X}] {}{} (Atk {} / Rng {} / Bul {}, Ammo {}, Slots {}/{})",
                            eq_idx,
                            rec.display_name(),
                            engraved,
                            eff[0],
                            eff[1],
                            eff[2],
                            rec.ammo,
                            rec.tail_count,
                            rec.tail_capacity
                        );
                    }
                }
                return format!("[E#{:02X}] (Inactive Equipment Record)", eq_idx);
            }
        }
        if (1..=0xFF).contains(&val) {
            let meta = item_metadata(val as u8);
            return format!("{} (0x{:02X})", meta.display_name, val);
        }
        if (0x200..=0x208).contains(&val) {
            let idx = (val - 0x200) as usize;
            let label = match idx {
                0 => format!("9mm / Standard Bullets Pool ({})", self.category_counts[0]),
                1 => format!("Rockets Pool ({})", self.category_counts[1]),
                2 => format!("DNA / Maeda Bullets Pool ({})", self.category_counts[2]),
                _ => format!("Special Pool #{}", idx),
            };
            return format!("[0x{:03X}] {}", val, label);
        }
        format!("Unknown (0x{:04X})", val as u16)
    }

    pub fn has_spell(&self, bit_index: u8) -> bool {
        if bit_index >= 32 {
            return false;
        }
        (self.parasite_spell_flags & (1u32 << bit_index)) != 0
    }

    pub fn set_spell(&mut self, bit_index: u8, unlocked: bool) {
        if bit_index >= 32 {
            return;
        }
        if unlocked {
            self.parasite_spell_flags |= 1u32 << bit_index;
        } else {
            self.parasite_spell_flags &= !(1u32 << bit_index);
        }
    }

    /// Allocates a free `EquipmentRecord` in `self.equipment[0..128]` and initializes it from `item_id`.
    /// Returns the inventory slot value `0x100 + eq_index`.
    pub fn allocate_equipment_from_item(&mut self, item_id: u8) -> Option<i16> {
        let free_idx = self.equipment.iter().position(|r| !r.is_active())?;
        self.equipment[free_idx] = EquipmentRecord::from_item_template(free_idx, item_id);
        Some((0x100 + free_idx) as i16)
    }

    /// Adds an item to the first empty slot in Aya's inventory.
    /// If `item_id` is a weapon or armor (`0x3F..=0xC7`), automatically allocates an `EquipmentRecord`!
    pub fn add_item_to_inventory(&mut self, item_id: u8) -> Result<usize> {
        let empty_slot = self
            .inventory_items
            .iter()
            .position(|&v| v == 0)
            .ok_or_else(|| anyhow::anyhow!("Aya's inventory is full (all 50 slots occupied)"))?;

        let meta = item_metadata(item_id);
        if meta.category.is_equipment() {
            let eq_val = self.allocate_equipment_from_item(item_id).ok_or_else(|| {
                anyhow::anyhow!("All 128 equipment records are occupied")
            })?;
            self.inventory_items[empty_slot] = eq_val;
        } else {
            self.inventory_items[empty_slot] = item_id as i16;
        }
        Ok(empty_slot)
    }

    /// Sort order of a slot value: weapons, armor, ammo, medicine, other items, key items; then by
    /// item id, and copies of one weapon or armor by their total stats, best first.
    fn sort_key(&self, val: i16) -> (u8, u16, i32) {
        let rank = |c: ItemCategory| match c {
            ItemCategory::Handgun => 0,
            ItemCategory::MachineGun => 1,
            ItemCategory::Rifle => 2,
            ItemCategory::Shotgun => 3,
            ItemCategory::GrenadeLauncher => 4,
            ItemCategory::RocketLauncher => 5,
            ItemCategory::MeleeWeapon => 6,
            ItemCategory::Armor => 7,
            ItemCategory::Ammo | ItemCategory::Crate => 8,
            ItemCategory::Consumable => 9,
            ItemCategory::StatUpgrade => 10,
            ItemCategory::Tool => 11,
            ItemCategory::ParasiteEnergy => 12,
            ItemCategory::TradingCard => 13,
            ItemCategory::KeyItem => 14,
            ItemCategory::Empty => 15,
        };
        match val {
            1..=0xFF => (rank(item_metadata(val as u8).category), val as u16, 0),
            0x100..=0x17F => match self.equipment.get((val - 0x100) as usize) {
                Some(rec) if rec.is_active() => {
                    let cat = item_metadata(rec.item_id).category;
                    let cat = if cat.is_equipment() {
                        cat
                    } else if rec.is_armor() {
                        ItemCategory::Armor
                    } else {
                        ItemCategory::MeleeWeapon
                    };
                    (rank(cat), rec.item_id as u16, -rec.effective_stats().iter().sum::<i32>())
                }
                _ => (16, val as u16, 0),
            },
            0x200..=0x208 => (rank(ItemCategory::Ammo), val as u16, 0),
            _ => (16, val as u16, 0),
        }
    }

    /// Sorts a list in place, filled slots first (empty ones at the end).
    fn sort_values(&self, list: &mut [i16]) {
        list.sort_by_key(|&v| (v == 0, if v == 0 { (0, 0, 0) } else { self.sort_key(v) }));
    }

    /// Sorts Aya's bag. The equipped weapon and armor stay equipped (their slot numbers follow
    /// them), as the game's own Sort does.
    pub fn sort_aya_bag(&mut self) {
        let held = |slot: i8, items: &[i16]| {
            usize::try_from(slot)
                .ok()
                .and_then(|s| items.get(s).copied())
                .filter(|&v| v != 0)
        };
        let weapon = held(self.equipped_weapon_slot, &self.inventory_items);
        let armor = held(self.equipped_armor_slot, &self.inventory_items);
        let mut items = std::mem::take(&mut self.inventory_items);
        self.sort_values(&mut items);
        let find = |v: Option<i16>, old: i8| match v {
            Some(v) => items.iter().position(|&x| x == v).map_or(old, |p| p as i8),
            None => old,
        };
        self.equipped_weapon_slot = find(weapon, self.equipped_weapon_slot);
        self.equipped_armor_slot = find(armor, self.equipped_armor_slot);
        self.inventory_items = items;
    }

    /// Sorts Wayne's storage: his item shelf and his equipment vault.
    pub fn sort_wayne_storage(&mut self) {
        let mut storage = std::mem::take(&mut self.storage);
        let mut special = std::mem::take(&mut self.special_storage);
        self.sort_values(&mut storage);
        self.sort_values(&mut special);
        self.storage = storage;
        self.special_storage = special;
    }

    /// Decodes the 16x16 4bpp PS1 save icon at `0x60..0x100` into a 16x16 RGBA8 image (1024 bytes).
    pub fn decode_icon_rgba(&self) -> [u8; 16 * 16 * 4] {
        let mut rgba = [0u8; 16 * 16 * 4];
        if self.raw_bytes.len() < 0x100 {
            return rgba;
        }
        let mut clut = [[0u8; 4]; 16];
        for (i, entry) in clut.iter_mut().enumerate() {
            let off = 0x60 + i * 2;
            let raw_color = u16::from_le_bytes([self.raw_bytes[off], self.raw_bytes[off + 1]]);
            if raw_color == 0 && i == 0 {
                *entry = [0, 0, 0, 0];
            } else {
                let r5 = (raw_color & 0x1F) as u8;
                let g5 = ((raw_color >> 5) & 0x1F) as u8;
                let b5 = ((raw_color >> 10) & 0x1F) as u8;
                *entry = [
                    (r5 << 3) | (r5 >> 2),
                    (g5 << 3) | (g5 >> 2),
                    (b5 << 3) | (b5 >> 2),
                    255,
                ];
            }
        }

        for y in 0..16 {
            for x_pair in 0..8 {
                let byte = self.raw_bytes[0x80 + y * 8 + x_pair];
                let idx0 = (byte & 0x0F) as usize;
                let idx1 = (byte >> 4) as usize;
                let p0 = (y * 16 + x_pair * 2) * 4;
                let p1 = p0 + 4;
                rgba[p0..p0 + 4].copy_from_slice(&clut[idx0]);
                rgba[p1..p1 + 4].copy_from_slice(&clut[idx1]);
            }
        }
        rgba
    }

    // =========================================================================
    // Quick Cheat & Preset Helpers
    // =========================================================================

    /// Repair icon/model classes written by older editor templates without
    /// replacing a player's upgrades, ammunition, effects or engraving.
    pub fn repair_equipment_types(&mut self) -> usize {
        let mut repaired = 0;
        for item in self.equipment.iter_mut().filter(|item| item.is_active()) {
            if (0x3F..=0xC7).contains(&item.item_id) {
                let meta = item_metadata(item.item_id);
                if item.icon_id != meta.default_icon_id || item.kind != meta.default_kind {
                    item.icon_id = meta.default_icon_id;
                    item.kind = meta.default_kind;
                    repaired += 1;
                }
            }
        }
        repaired
    }

    pub fn apply_cheat_max_aya(&mut self) {
        self.set_display_level(99);
        self.total_exp = 9_999_999;
        // The game's own top: 869, or 999 with HP Max Up armor equipped (see `to_bytes`).
        self.max_hp = self.max_hp_limit();
        self.current_hp = self.max_hp;
        self.bonus_points = 99_999;
        self.inventory_slot_count = MAX_STAT_BAG_SLOTS;
        self.pe_bonus_pool = 999;
        self.stats_levels = [1000; 7];
    }

    pub fn apply_cheat_unlock_all_pe(&mut self) {
        // Unlock all standard playable PE spells (0..=12, 18, 19), skipping Mito14..Mito18 (13..=17)
        for spell in PARASITE_SPELLS.iter() {
            if !spell.category.contains("Dummy") {
                self.set_spell(spell.bit_index, true);
            }
        }
    }

    pub fn apply_cheat_unlock_all_pe_including_hidden(&mut self) {
        self.parasite_spell_flags |= 0x000F_FFFF;
    }

    pub fn apply_cheat_max_ammo(&mut self) {
        self.category_counts[0] = 999; // 9mm / Standard Bullets
        self.category_counts[1] = 999; // Rockets
        self.category_counts[2] = 999; // DNA Bullets
        for rec in &mut self.equipment {
            if rec.is_active() && rec.is_weapon() {
                let cap = (rec.base_stats[2] as i32 + rec.bonus_stats[2] as i32).clamp(1, 999) as u16;
                rec.ammo = cap;
            }
        }
    }

    pub fn apply_cheat_max_equipped_gear(&mut self) {
        for rec in &mut self.equipment {
            if rec.is_active() {
                rec.bonus_stats = [999, 999, 999];
                rec.tail_capacity = 10;
                rec.tail_count = 10;
                rec.set_engraved(true);
                if rec.is_weapon() {
                    rec.ammo = 999;
                }
            }
        }
    }

    /// Adds an ultimate P90 and Cm Suit 2 and equips them (the old gear stays in the bag), plus
    /// tool kits and medicine. The suit carries HP Max Up, so with Max Aya max HP reaches 999.
    pub fn apply_cheat_give_ultimate_loadout(&mut self) -> Result<()> {
        self.inventory_slot_count = MAX_STAT_BAG_SLOTS;
        self.stats_levels[ITEM_CAPACITY_STAT] =
            self.stats_levels[ITEM_CAPACITY_STAT].max(MAX_BAG_ITEM_CAPACITY_STAT);
        self.apply_cheat_max_ammo();

        // Create Ultimate DE50AE7 / P90 and Cm Suit 2 if free slots exist
        if let Some(weapon_eq_val) = self.allocate_equipment_from_item(0x78) {
            // 0x78 = P90 Machine Gun
            let eq_idx = (weapon_eq_val - 0x100) as usize;
            let rec = &mut self.equipment[eq_idx];
            rec.bonus_stats = [900, 900, 900];
            rec.ammo = 950;
            rec.tail_capacity = 10;
            rec.tail_count = 10;
            rec.set_engraved(true);
            for (slot, mod_id) in [0x03u8, 0x14, 0x0F, 0x0E, 0x12, 0x0C, 0x09, 0x0A, 0x0B, 0x0D]
                .into_iter()
                .enumerate()
            {
                rec.set_mod_at(slot, mod_id);
            }
            if let Some(pos) = self.inventory_items.iter().position(|&v| v == 0) {
                self.inventory_items[pos] = weapon_eq_val;
                self.equipped_weapon_slot = pos as i8;
            }
        }

        if let Some(armor_eq_val) = self.allocate_equipment_from_item(0xC7) {
            // 0xC7 = Cm Suit 2
            let eq_idx = (armor_eq_val - 0x100) as usize;
            let rec = &mut self.equipment[eq_idx];
            rec.bonus_stats = [900, 900, 900];
            rec.tail_capacity = 10;
            rec.tail_count = 10;
            rec.set_engraved(true);
            for (slot, mod_id) in [0x0Au8, 0x0B, 0x06, 0x07, 0x0E, 0x01, 0x02, 0x03, 0x05, 0x04]
                .into_iter()
                .enumerate()
            {
                rec.set_mod_at(slot, mod_id);
            }
            if let Some(pos) = self.inventory_items.iter().position(|&v| v == 0) {
                self.inventory_items[pos] = armor_eq_val;
                // Worn, so its HP Max Up counts: with Max Aya, max HP reaches 999.
                self.equipped_armor_slot = pos as i8;
            }
        }

        // Also add Super Tool Kit (0x36), Tool Kit (0x35), Full Recover (0x0A), Revive (0x12), Chrysler Keys (0x38..=0x3E)
        for &item_id in &[0x36u8, 0x35, 0x0A, 0x12, 0x11] {
            if let Some(pos) = self.inventory_items.iter().position(|&v| v == 0) {
                self.inventory_items[pos] = item_id as i16;
            }
        }
        Ok(())
    }

    pub fn read_script_u8(&self, offset: usize) -> u8 {
        if offset < SCRIPT_VAR_SIZE {
            self.raw_bytes[SCRIPT_VAR_OFFSET + offset]
        } else {
            0
        }
    }

    pub fn write_script_u8(&mut self, offset: usize, value: u8) {
        if offset < SCRIPT_VAR_SIZE {
            self.raw_bytes[SCRIPT_VAR_OFFSET + offset] = value;
        }
    }

    pub fn format_equipment_mods(rec: &EquipmentRecord) -> String {
        let mut names = Vec::new();
        let is_armor = rec.is_armor();
        for i in 0..(rec.tail_count.max(rec.tail_capacity).min(10) as usize) {
            let raw_mod = rec.tail_data[i];
            if (raw_mod & 0x1F) != 0 {
                names.push(mod_metadata(raw_mod, is_armor).name);
            }
        }
        if names.is_empty() {
            "None".to_string()
        } else {
            names.join(", ")
        }
    }
}
