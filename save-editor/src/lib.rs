pub mod charset;
pub mod cli;
pub mod crc;
pub mod disc;
pub mod gui;
pub mod memcard;
pub mod save;
pub mod tables;

#[cfg(test)]
mod tests {
    use std::path::PathBuf;

    use crate::memcard::MemoryCardContainer;

    #[test]
    fn inventory_preflight_rejects_broken_references_without_writing() {
        let path = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("samples/Parasite_Eve_PS1_Sample_Card.srm");
        let mut card = MemoryCardContainer::open(&path).unwrap();
        for slot in card.slots.iter().flatten() { slot.validate_inventory().unwrap(); }
        let mut slot = card.slots[0].as_ref().unwrap().clone();
        slot.inventory_items.fill(0);slot.storage.fill(0);slot.special_storage.fill(0);
        for item in &mut slot.equipment { item.icon_id = 0; }
        slot.equipped_weapon_slot = 0;slot.equipped_armor_slot = -1;
        slot.equipment[0] = crate::save::EquipmentRecord::from_item_template(0, 0x78);
        slot.inventory_items[0] = 0x100;
        slot.validate_inventory().unwrap();
        for fault in 0..9 {
            let mut bad = slot.clone();
            match fault {
                0 => bad.inventory_items[49] = 0x100,
                1 => bad.storage[99] = 0x100,
                2 => bad.special_storage[81] = 0x100,
                3 => bad.equipment[0].tail_count = 11,
                4 => bad.equipment[0].tail_capacity = 11,
                5 => bad.equipment[0].icon_id = 0,
                6 => bad.equipped_weapon_slot = -2,
                7 => bad.equipped_armor_slot = 0,
                _ => bad.equipment[0].index = 127,
            }
            assert!(bad.validate_inventory().is_err(), "fault {}", fault);
            card.slots[0] = Some(bad);
            let target = std::env::temp_dir().join(format!("pe-invalid-card-{}-{}.srm", std::process::id(), fault));
            std::fs::write(&target, b"previous save").unwrap();
            assert!(card.export_to_ps1_card_image(&target).is_err());
            assert_eq!(std::fs::read(&target).unwrap(), b"previous save");
            std::fs::remove_file(target).unwrap();
        }
        slot.inventory_items[1] = 0x200;slot.storage[0] = 0x200;
        slot.equipment[0].bonus_stats = [999, 500, 123];
        slot.validate_inventory().unwrap();
    }

    #[test]
    fn test_real_ps1_sample_card_parsing_and_crc() {
        let sample_path =
            PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("samples/Parasite_Eve_PS1_Sample_Card.srm");
        let container = MemoryCardContainer::open(&sample_path).expect("open sample .srm card");

        let active = container.active_slot_indices();
        assert_eq!(active, vec![0, 1, 2, 3, 4, 5, 6]);

        let expected_crcs = [0x7231u16, 0x0A51, 0xE7FE, 0xF331, 0xE70C, 0xF2C1, 0x08DC];
        let expected_levels = [7u8, 14, 18, 24, 32, 34, 1];

        for (i, &exp_crc) in expected_crcs.iter().enumerate() {
            let slot = container.slots[i].as_ref().expect("slot exists");
            assert!(
                slot.is_crc_valid(),
                "Slot {} CRC mismatch: stored 0x{:04X} vs calc 0x{:04X}",
                i + 1,
                slot.stored_crc,
                slot.calculated_crc
            );
            assert_eq!(slot.stored_crc, exp_crc);
            assert_eq!(slot.display_level(), expected_levels[i]);
            assert_eq!(slot.aya_name, "Aya");
        }

        // Slot 7 is Clear Data
        let slot7 = container.slots[6].as_ref().unwrap();
        assert!(slot7.is_clear_data);
    }

    #[test]
    fn test_edit_and_roundtrip_serialization() {
        let sample_path =
            PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("samples/Parasite_Eve_PS1_Sample_Card.srm");
        let mut container = MemoryCardContainer::open(&sample_path).expect("open sample .srm card");

        let slot = container.slots[0].as_mut().unwrap();
        slot.apply_cheat_max_aya();
        slot.apply_cheat_unlock_all_pe();
        slot.apply_cheat_max_ammo();
        slot.apply_cheat_give_ultimate_loadout().unwrap();

        let bytes = slot.to_bytes();
        let reparsed =
            crate::save::PeSaveSlot::from_bytes(&bytes, 0, &slot.filename).expect("reparse slot");
        assert!(reparsed.is_crc_valid());
        assert_eq!(reparsed.display_level(), 99);
        assert_eq!(reparsed.current_hp, 999);
        assert_eq!(reparsed.max_hp, 999);
        assert_eq!(reparsed.bonus_points, 99_999);
        assert_eq!(reparsed.inventory_slot_count, 46);
        assert_eq!(reparsed.category_counts[0], 999);
    }
    #[test]
    fn factory_equipment_preserves_native_type_ammo_bonuses_and_effects() {
        use crate::save::EquipmentRecord;
        let p90 = EquipmentRecord::from_item_template(7, 0x78);
        assert_eq!((p90.index, p90.icon_id, p90.kind), (7, 4, 3));
        assert_eq!(p90.base_stats, [122, 51, 200]);
        assert_eq!(p90.ammo, 201);
        assert_eq!(p90.bonus_stats, [0, 2, 1]);
        assert_eq!((p90.tail_count, p90.tail_capacity), (5, 7));
        assert_eq!(&p90.tail_data[..2], &[37, 72]);
        let armor = EquipmentRecord::from_item_template(8, 0xB7);
        assert_eq!((armor.icon_id, armor.kind), (18, 9));
        assert_eq!(armor.base_stats, [106, 102, 48]);
        assert_eq!(armor.bonus_stats, [1, 2, 3]);
        assert_eq!((armor.tail_count, armor.tail_capacity), (7, 10));
        assert_eq!(armor.tail_data[0], 11);
        for id in 0x3F..=0xC7 {
            let item = EquipmentRecord::from_item_template(0, id);
            assert_eq!(item.item_id, id);
            assert!((1..=9).contains(&item.kind));
            assert!(item.tail_count <= 10 && item.tail_capacity <= 10);
            let meta = crate::tables::item_metadata(id);
            assert_eq!(meta.default_kind, item.kind);
            assert_eq!(meta.default_icon_id, item.icon_id);
        }
    }

    #[test]
    fn bag_capacity_edits_update_growth_and_preserve_other_stats() {
        let path = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
            .join("samples/Parasite_Eve_PS1_Sample_Card.srm");
        let card = MemoryCardContainer::open(&path).unwrap();
        let original = card.slots[0].as_ref().unwrap();
        let mut native_test_data = Vec::new();
        for id in 0x3F..=0xC7 {
            let item = crate::save::EquipmentRecord::from_item_template(0, id);
            let mut bytes = [0; 32];
            item.write(&mut bytes);
            native_test_data.extend_from_slice(&bytes);
        }
        for capacity in 10..=46 {
            let mut slot = original.clone();
            slot.inventory_slot_count = capacity;
            let bytes = slot.to_bytes();
            let decoded = crate::save::PeSaveSlot::from_bytes(&bytes, 0, &slot.filename).unwrap();
            let growth = decoded.stats_levels[6];
            assert_eq!(crate::tables::BAG_CAPACITY_BY_LEVEL[(growth / 10).min(98) as usize], capacity);
            assert_eq!(&decoded.stats_levels[..6], &original.stats_levels[..6]);
            assert!(decoded.is_crc_valid());
            native_test_data.extend_from_slice(&(capacity as u16).to_le_bytes());
            native_test_data.extend_from_slice(&growth.to_le_bytes());
        }
        // Optional cross-language fixture: the native asset test compares every
        // editor-created record with the disc and recalculates all bag sizes.
        if let Some(path) = std::env::var_os("PE_TEST_EDITOR_DATA") {
            std::fs::write(path, native_test_data).unwrap();
        }
        let mut unchanged = original.clone();
        assert_eq!(unchanged.to_bytes(), original.raw_bytes);
    }

    #[test]
    fn repair_legacy_equipment_preserves_custom_upgrades() {
        let path = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
            .join("samples/Parasite_Eve_PS1_Sample_Card.srm");
        let mut card = MemoryCardContainer::open(&path).unwrap();
        let save = card.slots[0].as_mut().unwrap();
        let mut p90 = crate::save::EquipmentRecord::from_item_template(7, 0x78);
        p90.icon_id = 5;
        p90.kind = 4;
        p90.bonus_stats = [900, 333, 222];
        p90.ammo = 700;
        p90.set_engraved(true);
        p90.set_mod_at(0, 20);
        save.equipment[7] = p90.clone();
        save.repair_equipment_types();
        let fixed = &save.equipment[7];
        let mut expected = [0; 32];
        p90.write(&mut expected);
        expected[0] = 4;
        expected[6] = 3;
        let mut actual = [0; 32];
        fixed.write(&mut actual);
        assert_eq!(actual, expected);
        assert_eq!(save.repair_equipment_types(), 0);
    }


    #[test]
    fn test_new_gear_uses_the_games_icon_and_kind() {
        let sample_path =
            PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("samples/Parasite_Eve_PS1_Sample_Card.srm");
        let mut container = MemoryCardContainer::open(&sample_path).expect("open sample .srm card");
        let slot = container.slots[0].as_mut().unwrap();

        // P90 (machine gun) and a handgun: icon 4 / kind 3 and icon 1 / kind 1 in the game's records.
        for (item_id, icon, kind) in [(0x78u8, 4u8, 3u8), (0x44, 1, 1)] {
            let val = slot.allocate_equipment_from_item(item_id).unwrap();
            let rec = &slot.equipment[(val - 0x100) as usize];
            assert_eq!((rec.item_id, rec.icon_id, rec.kind), (item_id, icon, kind));
        }
    }

    #[test]
    fn test_sort_orders_bag_and_storage_and_keeps_equipment_on() {
        let sample_path =
            PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("samples/Parasite_Eve_PS1_Sample_Card.srm");
        let mut container = MemoryCardContainer::open(&sample_path).expect("open sample .srm card");
        let slot = container.slots[0].as_mut().unwrap();
        slot.add_item_to_inventory(0x06).unwrap(); // Medicine 1
        slot.add_item_to_inventory(0xC7).unwrap(); // Cm Suit 2
        slot.add_item_to_inventory(0x78).unwrap(); // P90
        slot.inventory_items.swap(0, 3); // a gap and some disorder
        slot.inventory_items[1] = 0;
        let weapon = slot.inventory_items[slot.equipped_weapon_slot as usize];
        let armor = slot.inventory_items[slot.equipped_armor_slot as usize];
        let mut before: Vec<i16> = slot.inventory_items.clone();

        slot.sort_aya_bag();
        let after = &slot.inventory_items;
        let mut a = after.clone();
        before.sort();
        a.sort();
        assert_eq!(a, before, "same items");
        let filled = after.iter().take_while(|&&v| v != 0).count();
        assert!(after[filled..].iter().all(|&v| v == 0), "empty slots last");
        let pos = |v: i16| after.iter().position(|&x| x == v).unwrap();
        let p90 = pos(after.iter().copied().find(|&v| v >= 0x100 && slot.equipment[(v - 0x100) as usize].item_id == 0x78).unwrap());
        let suit = pos(after.iter().copied().find(|&v| v >= 0x100 && slot.equipment[(v - 0x100) as usize].item_id == 0xC7).unwrap());
        assert!(p90 < suit && suit < pos(0x06), "weapons, then armor, then medicine");
        assert_eq!(after[slot.equipped_weapon_slot as usize], weapon);
        assert_eq!(after[slot.equipped_armor_slot as usize], armor);

        slot.storage[5] = 0x06;
        slot.storage[2] = 0x38;
        slot.sort_wayne_storage();
        assert!(slot.storage.iter().position(|&v| v == 0x06) < slot.storage.iter().position(|&v| v == 0));
    }

    #[test]
    fn test_mod_bytes_use_the_games_groups_and_names() {
        use crate::tables::{encode_mod_byte, mod_metadata};
        // The P90's own record holds Rate of Fire x10 and Attack All as 0x25 and 0x48.
        assert_eq!(encode_mod_byte(0x05, false), 0x25);
        assert_eq!(encode_mod_byte(0x08, false), 0x48);
        assert_eq!(encode_mod_byte(0x09, false), 0x09); // Heat: no group
        assert_eq!(encode_mod_byte(0x14, false), 0x74); // 3 Commands: group 3
        assert_eq!(encode_mod_byte(0x10, false), 0x90); // Steal: group 4
        assert_eq!(encode_mod_byte(0x0A, true), 0xAA); // Bag +4: group 5
        // An effect no table knows is kept, never turned into an empty slot.
        assert_eq!(encode_mod_byte(0x15, false), 0x15);
        assert_eq!(mod_metadata(0x15, false).name, "Unknown Effect");

        // The economy mod's armor effects 17..=20, and the game's own names for 13 and 15.
        assert_eq!(mod_metadata(17, true).name, "Bargain (mod)");
        assert_eq!(mod_metadata(18, true).name, "Scavenger (mod)");
        assert_eq!(mod_metadata(20, true).name, "Salvager (mod)");
        assert_eq!(mod_metadata(13, true).name, "Slower Active Time");
        assert_eq!(mod_metadata(15, true).name, "Attack Down");
        for effect in 1..32u8 {
            for armor in [false, true] {
                assert_ne!(mod_metadata(effect, armor).name, "Empty Slot", "effect {}", effect);
            }
        }
    }

    #[test]
    fn test_edits_keep_mod_data_in_the_free_tail() {
        // Mods keep their own blocks after the checksum word (0x1C90..0x2000), inside the CRC.
        let sample_path =
            PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("samples/Parasite_Eve_PS1_Sample_Card.srm");
        let mut container = MemoryCardContainer::open(&sample_path).expect("open sample .srm card");
        let slot = container.slots[0].as_mut().unwrap();
        let tail: Vec<u8> = (0..0x370u32).map(|i| (i * 7 + 3) as u8).collect();
        slot.raw_bytes[0x1C90..0x2000].copy_from_slice(&tail);

        slot.apply_cheat_max_aya();
        slot.add_item_to_inventory(0x78).unwrap();
        let bytes = slot.to_bytes();
        let reparsed = crate::save::PeSaveSlot::from_bytes(&bytes, 0, &slot.filename).unwrap();
        assert!(reparsed.is_crc_valid());
        assert_eq!(&bytes[0x1C90..0x2000], &tail[..]);
    }

    #[test]
    fn test_max_hp_follows_hp_stat_and_armor_without_disc() {
        let sample_path =
            PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("samples/Parasite_Eve_PS1_Sample_Card.srm");
        let mut container = MemoryCardContainer::open(&sample_path).expect("open sample .srm card");
        let slot = container.slots[0].as_mut().unwrap();
        slot.equipped_armor_slot = -1;
        let _ = slot.to_bytes();

        // No armor: 999 is out of reach, the game's top is 869 and the HP stat goes to its top level.
        slot.max_hp = 999;
        slot.current_hp = 999;
        let bytes = slot.to_bytes();
        let reparsed = crate::save::PeSaveSlot::from_bytes(&bytes, 0, &slot.filename).unwrap();
        assert_eq!((reparsed.max_hp, reparsed.current_hp), (869, 869));
        assert!(reparsed.stats_levels[0] >= crate::disc::TOP_LEVEL_STAT);

        // Armor with HP Max Up equipped: 999.
        let val = slot.allocate_equipment_from_item(0xC7).unwrap();
        let rec = &mut slot.equipment[(val - 0x100) as usize];
        rec.tail_count = rec.tail_count.max(1);
        rec.set_mod_at(0, 0x0B);
        let pos = slot.inventory_items.iter().position(|&v| v == 0).unwrap();
        slot.inventory_items[pos] = val;
        slot.equipped_armor_slot = pos as i8;
        assert!(slot.armor_has_hp_max_up());
        slot.apply_cheat_max_aya();
        let bytes = slot.to_bytes();
        let reparsed = crate::save::PeSaveSlot::from_bytes(&bytes, 0, &slot.filename).unwrap();
        assert_eq!((reparsed.max_hp, reparsed.current_hp), (999, 999));
    }

    #[test]
    fn test_max_aya_and_ultimate_loadout_give_999_hp_in_either_order() {
        let sample_path =
            PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("samples/Parasite_Eve_PS1_Sample_Card.srm");
        for max_aya_first in [true, false] {
            let mut container = MemoryCardContainer::open(&sample_path).expect("open sample .srm card");
            let slot = container.slots[0].as_mut().unwrap();
            // As the GUI does: each cheat is written out before the next.
            for step in 0..2 {
                if (step == 0) == max_aya_first {
                    slot.apply_cheat_max_aya();
                } else {
                    slot.apply_cheat_give_ultimate_loadout().unwrap();
                }
                let _ = slot.to_bytes();
            }
            let bytes = slot.to_bytes();
            let r = crate::save::PeSaveSlot::from_bytes(&bytes, 0, &slot.filename).unwrap();
            assert_eq!((r.max_hp, r.current_hp), (999, 999), "max_aya_first = {}", max_aya_first);

            let worn = |s: i8| r.equipment[(r.inventory_items[s as usize] - 0x100) as usize].item_id;
            assert_eq!(worn(r.equipped_weapon_slot), 0x78);
            assert_eq!(worn(r.equipped_armor_slot), 0xC7);
        }
    }

    #[test]
    fn test_bag_size_follows_item_capacity_stat_without_disc() {
        let sample_path =
            PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("samples/Parasite_Eve_PS1_Sample_Card.srm");
        let mut container = MemoryCardContainer::open(&sample_path).expect("open sample .srm card");
        let slot = container.slots[0].as_mut().unwrap();
        slot.stats_levels[6] = 0;
        let _ = slot.to_bytes();

        slot.inventory_slot_count = 50;
        let bytes = slot.to_bytes();
        let reparsed = crate::save::PeSaveSlot::from_bytes(&bytes, 0, &slot.filename).unwrap();
        assert_eq!(reparsed.inventory_slot_count, 46);
        assert_eq!(crate::tables::BAG_CAPACITY_BY_LEVEL[(reparsed.stats_levels[6] / 10) as usize], 46);
    }
}
