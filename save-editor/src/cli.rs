use std::fs;
use std::path::PathBuf;

use anyhow::{bail, Context, Result};
use clap::{Args, Parser, Subcommand};

use crate::memcard::MemoryCardContainer;
use crate::save::PeSaveSlot;
use crate::tables::{
    ARMOR_MOD_TABLE, ITEM_TABLE, PARASITE_SPELLS, STAT_GROWTH_NAMES, WEAPON_MOD_TABLE,
};

#[derive(Parser, Debug)]
#[command(
    name = "pe1-save-editor",
    version,
    about = "Parasite Eve (SLUS-006.62 / PC Port) Save Editor & PsyZ Memory Card Manager (GUI + CLI)"
)]
pub struct Cli {
    /// Optional path to open directly in GUI when no subcommand is provided (e.g. `bu00`, `.srm`, `.mcr`)
    pub path: Option<PathBuf>,

    #[command(subcommand)]
    pub command: Option<Command>,
}

#[derive(Subcommand, Debug)]
pub enum Command {
    /// Launch the graphical Save Editor (default when run without arguments)
    Gui {
        /// Optional path to a PsyZ directory (`bu00`/`bu10`), raw slot file, or PS1 card (`.srm`/`.mcr`/`.gme`)
        path: Option<PathBuf>,
    },
    /// Inspect all slots or a specific slot in a PsyZ folder (`bu00`) or PS1 memory card (`.srm`/`.mcr`)
    Inspect {
        /// Path to `bu00` directory, raw slot file, or `.srm`/`.mcr`/`.mcd`/`.gme`/`.mcs` file
        path: PathBuf,
        /// Optional 1-based slot number (1..=15, where 1 = Slot A)
        #[arg(short, long)]
        slot: Option<usize>,
        /// Show detailed inventory, equipment records, mods, and Wayne's storage
        #[arg(short, long)]
        verbose: bool,
    },
    /// Edit a save slot from the command line and automatically recalculate CRC-16 + battle state
    Edit(EditArgs),
    /// Import PS1 memory card saves (`.srm`, `.mcr`, `.mcd`, `.gme`, `.mcs`) into a PsyZ `bu00` or `bu10` directory
    ImportPsyz {
        /// Source PS1 memory card or single save file
        source: PathBuf,
        /// Target PsyZ directory (e.g. `./bu00`)
        target_dir: PathBuf,
    },
    /// Export a PsyZ directory (`bu00`/`bu10`) or save into a 128 KB PS1 memory card image (`.srm`/`.mcr`)
    ExportCard {
        /// Source PsyZ directory (e.g. `./bu00`) or save file
        source: PathBuf,
        /// Target `.srm` or `.mcr` file path
        target_file: PathBuf,
    },
    /// Export a save slot to a human-readable JSON file
    ExportJson {
        /// Source container (`bu00`, `.srm`, `.mcr`, or raw slot)
        source: PathBuf,
        /// Output `.json` file path
        output: PathBuf,
        /// 1-based slot number (1..=15). Defaults to the first active slot.
        #[arg(short, long)]
        slot: Option<usize>,
    },
    /// Import a JSON save slot back into a container (`bu00`, `.srm`, `.mcr`, or raw slot)
    ImportJson {
        /// Input `.json` file path
        input: PathBuf,
        /// Target container (`bu00`, `.srm`, `.mcr`, or raw slot)
        target: PathBuf,
        /// Optional 1-based target slot override (1..=15)
        #[arg(short, long)]
        slot: Option<usize>,
    },
    /// List all 256 Parasite Eve Item IDs, 32 Weapon/Armor Mods, and 20 Parasite Energy spells
    ListItems {
        /// Optional case-insensitive search filter
        #[arg(short, long)]
        filter: Option<String>,
    },
}

#[derive(Args, Debug)]
pub struct EditArgs {
    /// Path to `bu00` directory, raw slot file, or `.srm`/`.mcr`/`.mcd`/`.gme`/`.mcs` file
    pub path: PathBuf,

    /// 1-based slot number to edit (1..=15, where 1 = Slot A). Defaults to the first active slot.
    #[arg(short, long)]
    pub slot: Option<usize>,

    /// Set Aya's display level (1..=99)
    #[arg(long)]
    pub level: Option<u8>,

    /// Set current HP (1..=999)
    #[arg(long)]
    pub hp: Option<u16>,

    /// Set maximum HP (1..=999). The game sets it from the HP stat, so the stat is raised to
    /// match (exactly with a disc). The top is 869, or 999 with HP Max Up armor equipped.
    #[arg(long)]
    pub max_hp: Option<u16>,

    /// Set total EXP
    #[arg(long)]
    pub exp: Option<u32>,

    /// Set Bonus Points (BP, 0..=99999)
    #[arg(long)]
    pub bp: Option<u32>,

    /// Set Aya's base bag capacity (10..=46; armor can add up to 4 slots)
    #[arg(long, value_parser = clap::value_parser!(u8).range(10..=46))]
    pub slots: Option<u8>,

    /// Parasite Eve (USA) disc image (`.cue`, `.bin` or `.iso`) to read item records and the bag
    /// table from. Defaults to the disc in the game's `parasite-eve.toml`.
    #[arg(long)]
    pub disc: Option<PathBuf>,

    /// Set story day (1..=6)
    #[arg(long)]
    pub day: Option<i16>,

    /// Set current room/map ID (1..=437)
    #[arg(long)]
    pub map: Option<i16>,

    /// Set EX Game playthrough round count (0 = 1st Playthrough, 1 = 2nd Round, etc.)
    #[arg(long)]
    pub ex_round: Option<u8>,

    /// Mark or unmark as Clear Data / EX Game save
    #[arg(long)]
    pub clear_data: Option<bool>,

    /// Set Aya's name (max 12 chars)
    #[arg(long)]
    pub aya_name: Option<String>,

    /// Set 9mm / standard bullets pool count
    #[arg(long)]
    pub ammo_9mm: Option<u16>,

    /// Set rockets pool count
    #[arg(long)]
    pub ammo_rocket: Option<u16>,

    /// Set DNA / Maeda bullets pool count
    #[arg(long)]
    pub ammo_dna: Option<u16>,

    /// Add an item by hex (`0x36`) or decimal (`54`) ID to the first free inventory slot (auto-creates equipment record for weapons/armor)
    #[arg(long)]
    pub add_item: Vec<String>,

    /// Repair equipment icons/model classes from older editor versions, preserving upgrades
    #[arg(long)]
    pub repair_equipment_types: bool,

    /// Sort Aya's bag (weapons, armor, ammo, medicine, other, key items; the equipped gun and
    /// armor stay equipped)
    #[arg(long)]
    pub sort_bag: bool,

    /// Sort Wayne's item shelf and equipment vault
    #[arg(long)]
    pub sort_wayne: bool,

    /// Cheat: Max Aya stats (Lv 99, top HP: 869, or 999 with HP Max Up armor; 99,999 BP, 46 item
    /// slots, max stat growth)
    #[arg(long)]
    pub max_aya: bool,

    /// Cheat: Unlock all playable Parasite Energy spells
    #[arg(long)]
    pub unlock_all_pe: bool,

    /// Cheat: Max all ammo pools (999) and refill all weapons
    #[arg(long)]
    pub max_ammo: bool,

    /// Cheat: Upgrade all active weapons and armor (+999 bonus stats, 10 mod slots, Wayne engraving)
    #[arg(long)]
    pub max_gear: bool,

    /// Cheat: Add and equip an ultimate P90 + Cm Suit 2 (old gear stays in the bag), plus Super
    /// Tool Kit + Full Recover. The suit's HP Max Up lets --max-aya reach 999 HP
    #[arg(long)]
    pub ultimate_loadout: bool,
}

pub fn run_cli_command(cmd: Command) -> Result<()> {
    match cmd {
        Command::Gui { .. } => unreachable!("Handled in main"),
        Command::Inspect {
            path,
            slot,
            verbose,
        } => cmd_inspect(&path, slot, verbose),
        Command::Edit(args) => cmd_edit(args),
        Command::ImportPsyz { source, target_dir } => cmd_import_psyz(&source, &target_dir),
        Command::ExportCard {
            source,
            target_file,
        } => cmd_export_card(&source, &target_file),
        Command::ExportJson {
            source,
            output,
            slot,
        } => cmd_export_json(&source, &output, slot),
        Command::ImportJson {
            input,
            target,
            slot,
        } => cmd_import_json(&input, &target, slot),
        Command::ListItems { filter } => cmd_list_items(filter.as_deref()),
    }
}

fn resolve_slot_index(container: &MemoryCardContainer, user_slot_1based: Option<usize>) -> Result<usize> {
    if let Some(s) = user_slot_1based {
        if !(1..=15).contains(&s) {
            bail!("Slot number must be between 1 and 15 (got {})", s);
        }
        let idx = s - 1;
        if container.slots[idx].is_none() {
            bail!("Slot {} ({}) is empty in {}", s, (b'A' + idx as u8) as char, container.path.display());
        }
        Ok(idx)
    } else {
        container.first_active_slot_index().ok_or_else(|| {
            anyhow::anyhow!("No active Parasite Eve save slots found in {}", container.path.display())
        })
    }
}

fn cmd_inspect(path: &PathBuf, slot: Option<usize>, verbose: bool) -> Result<()> {
    let container = MemoryCardContainer::open(path)?;
    println!("================================================================================");
    println!(" Container : {}", container.path.display());
    println!(" Format    : {}", container.kind.label());
    println!(" Active    : {} / 15 slots", container.active_slot_indices().len());
    println!("================================================================================");

    if let Some(s) = slot {
        let idx = resolve_slot_index(&container, Some(s))?;
        let save = container.slots[idx].as_ref().unwrap();
        print_slot_summary(save, true);
    } else {
        for idx in container.active_slot_indices() {
            let save = container.slots[idx].as_ref().unwrap();
            print_slot_summary(save, verbose);
            println!("--------------------------------------------------------------------------------");
        }
    }
    Ok(())
}

fn print_slot_summary(save: &PeSaveSlot, verbose: bool) {
    let slot_letter = (b'A' + save.slot_index as u8) as char;
    let crc_status = if save.is_crc_valid() {
        format!("VALID (0x{:04X})", save.stored_crc)
    } else {
        format!(
            "MISMATCH (stored 0x{:04X} != expected 0x{:04X})",
            save.stored_crc, save.calculated_crc
        )
    };
    println!(
        "Slot {:2} ({}) | File: {} | CRC-16: {}",
        save.slot_index + 1,
        slot_letter,
        save.filename,
        crc_status
    );
    println!("  Header Title : {}", save.header_title);
    println!(
        "  Aya Name     : {:?} (Save Name: {:?})",
        save.aya_name, save.save_name
    );
    println!(
        "  Level / HP   : Lv {:2} | HP {:3} / {:3} | EXP: {} | BP: {}",
        save.display_level(),
        save.current_hp,
        save.max_hp,
        save.total_exp,
        save.bonus_points
    );
    println!(
        "  Story / Map  : Day {} (Story Day {}) | Map {:3} ({}) | EX Round: {}{}",
        save.play_time_class,
        save.current_story_day,
        save.current_map_number,
        save.location_description(),
        save.new_game_count + 1,
        if save.is_clear_data { " [CLEAR DATA]" } else { "" }
    );
    println!(
        "  Play Time    : {} ({} s) | Item Capacity: {} slots | PE Bonus: {}",
        save.formatted_play_time(),
        save.play_time_seconds,
        save.inventory_slot_count,
        save.pe_bonus_pool
    );
    println!(
        "  Ammo Pools   : 9mm Bullets = {} | Rockets = {} | DNA Bullets = {}",
        save.category_counts[0], save.category_counts[1], save.category_counts[2]
    );
    println!(
        "  Equipped     : Weapon Slot = {} | Armor Slot = {}",
        save.equipped_weapon_slot, save.equipped_armor_slot
    );

    let unlocked_spells: Vec<&str> = PARASITE_SPELLS
        .iter()
        .filter(|sp| save.has_spell(sp.bit_index))
        .map(|sp| sp.name)
        .collect();
    println!(
        "  PE Spells    : [0x{:05X}] {}",
        save.parasite_spell_flags,
        if unlocked_spells.is_empty() {
            "None".to_string()
        } else {
            unlocked_spells.join(", ")
        }
    );

    if verbose {
        println!("  Stat Growth Levels (base = Lv*10, max = 1000):");
        for (i, &val) in save.stats_levels.iter().enumerate() {
            println!("    {} = {}", STAT_GROWTH_NAMES[i], val);
        }

        println!("  Aya's Inventory (up to {} active slots):", save.inventory_slot_count);
        for (i, &val) in save.inventory_items.iter().enumerate() {
            if val != 0 || i < save.inventory_slot_count as usize {
                let eq_tag = if save.equipped_weapon_slot == i as i8 {
                    " [EQUIPPED WEAPON]"
                } else if save.equipped_armor_slot == i as i8 {
                    " [EQUIPPED ARMOR]"
                } else {
                    ""
                };
                if val != 0 {
                    println!(
                        "    Slot {:02}: 0x{:04X} -> {}{}",
                        i,
                        val as u16,
                        save.describe_slot_value(val),
                        eq_tag
                    );
                }
            }
        }

        println!("  Active Equipment Records (0x100..0x17F):");
        for rec in save.equipment.iter().filter(|r| r.is_active()) {
            let eff = rec.effective_stats();
            let axes = if rec.is_armor() {
                format!(
                    "Def {:3} ({}+{:3}), PE {:3} ({}+{:3}), Crit {:3} ({}+{:3})",
                    eff[0], rec.base_stats[0], rec.bonus_stats[0],
                    eff[1], rec.base_stats[1], rec.bonus_stats[1],
                    eff[2], rec.base_stats[2], rec.bonus_stats[2],
                )
            } else {
                format!(
                    "Atk {:3} ({}+{:3}), Rng {:3} ({}+{:3}), Bul {:3} ({}+{:3}), Ammo {}",
                    eff[0], rec.base_stats[0], rec.bonus_stats[0],
                    eff[1], rec.base_stats[1], rec.bonus_stats[1],
                    eff[2], rec.base_stats[2], rec.bonus_stats[2],
                    rec.ammo
                )
            };
            println!(
                "    [E#{:02X} / 0x{:03X}] {:14} (ID 0x{:02X}, Kind {}, Slots {}/{}{}) | {} | Mods: {}",
                rec.index,
                0x100 + rec.index,
                rec.display_name(),
                rec.item_id,
                rec.kind,
                rec.tail_count,
                rec.tail_capacity,
                if rec.is_engraved() { ", Wayne Engraved" } else { "" },
                axes,
                PeSaveSlot::format_equipment_mods(rec)
            );
        }

        let wayne_items: Vec<(usize, i16)> = save
            .storage
            .iter()
            .copied()
            .enumerate()
            .filter(|&(_, v)| v != 0)
            .collect();
        let wayne_equip: Vec<(usize, i16)> = save
            .special_storage
            .iter()
            .copied()
            .enumerate()
            .filter(|&(_, v)| v != 0)
            .collect();
        if !wayne_items.is_empty() || !wayne_equip.is_empty() {
            println!(
                "  Wayne's Storage: {} regular items, {} special/equipment items",
                wayne_items.len(),
                wayne_equip.len()
            );
            for (idx, val) in wayne_items {
                println!("    Storage[{:02}]: {}", idx, save.describe_slot_value(val));
            }
            for (idx, val) in wayne_equip {
                println!("    SpecialStorage[{:02}]: {}", idx, save.describe_slot_value(val));
            }
        }
    }
}

fn parse_u8_hex_or_dec(s: &str) -> Result<u8> {
    let trimmed = s.trim();
    if let Some(hex) = trimmed.strip_prefix("0x").or_else(|| trimmed.strip_prefix("0X")) {
        Ok(u8::from_str_radix(hex, 16)?)
    } else {
        Ok(trimmed.parse::<u8>()?)
    }
}

fn cmd_edit(args: EditArgs) -> Result<()> {
    let game = match &args.disc {
        Some(disc) => Some(crate::disc::load_disc(disc)?),
        None => crate::disc::load_configured_disc(),
    };
    match &game {
        Some(g) => println!("Game data: {}", g.source.display()),
        None if !args.add_item.is_empty() || args.ultimate_loadout => println!(
            "Using verified USA equipment templates. Use --disc for exact HP growth calculations."
        ),
        None => {}
    }

    let mut container = MemoryCardContainer::open(&args.path)?;
    let idx = resolve_slot_index(&container, args.slot)?;
    let save = container.slots[idx].as_mut().unwrap();

    if args.repair_equipment_types {
        println!("Repaired {} equipment icon/model classes", save.repair_equipment_types());
    }
    if args.max_aya {
        save.apply_cheat_max_aya();
    }
    if args.unlock_all_pe {
        save.apply_cheat_unlock_all_pe();
    }
    if args.max_ammo {
        save.apply_cheat_max_ammo();
    }
    if args.max_gear {
        save.apply_cheat_max_equipped_gear();
    }
    if args.ultimate_loadout {
        save.apply_cheat_give_ultimate_loadout()?;
    }

    if let Some(lv) = args.level {
        save.set_display_level(lv);
    }
    if let Some(max_hp) = args.max_hp {
        save.max_hp = max_hp.clamp(1, 999);
    }
    if let Some(hp) = args.hp {
        save.current_hp = hp.clamp(1, 999);
    }
    if let Some(exp) = args.exp {
        save.total_exp = exp;
    }
    if let Some(bp) = args.bp {
        save.bonus_points = bp.min(99_999);
    }
    if let Some(slots) = args.slots {
        save.inventory_slot_count = slots;
    }
    if let Some(day) = args.day {
        save.play_time_class = day.clamp(1, 6);
        save.current_story_day = day.clamp(1, 8) as i8;
    }
    if let Some(map) = args.map {
        save.current_map_number = map;
    }
    if let Some(ex) = args.ex_round {
        save.new_game_count = ex;
    }
    if let Some(cd) = args.clear_data {
        save.is_clear_data = cd;
    }
    if let Some(name) = args.aya_name {
        save.aya_name = name;
    }
    if let Some(ammo) = args.ammo_9mm {
        save.category_counts[0] = ammo;
    }
    if let Some(rockets) = args.ammo_rocket {
        save.category_counts[1] = rockets;
    }
    if let Some(dna) = args.ammo_dna {
        save.category_counts[2] = dna;
    }

    for item_str in &args.add_item {
        let item_id = parse_u8_hex_or_dec(item_str)
            .with_context(|| format!("Invalid item ID '{}'", item_str))?;
        let added_slot = save.add_item_to_inventory(item_id)?;
        println!(
            "Added item 0x{:02X} to inventory slot #{}",
            item_id, added_slot
        );
    }

    if args.sort_bag {
        save.sort_aya_bag();
    }
    if args.sort_wayne {
        save.sort_wayne_storage();
    }

    let _ = save.to_bytes();
    let updated_summary = save.clone();
    container.save()?;

    println!("Successfully updated Slot {} in {}:", idx + 1, container.path.display());
    print_slot_summary(&updated_summary, false);
    Ok(())
}

fn cmd_import_psyz(source: &PathBuf, target_dir: &PathBuf) -> Result<()> {
    let mut container = MemoryCardContainer::open(source)?;
    let written = container.export_to_psyz_dir(target_dir)?;
    println!(
        "Imported {} Parasite Eve save slot(s) from {} into PsyZ directory {}:",
        written.len(),
        source.display(),
        target_dir.display()
    );
    for p in written {
        println!("  -> {}", p.display());
    }
    Ok(())
}

fn cmd_export_card(source: &PathBuf, target_file: &PathBuf) -> Result<()> {
    let mut container = MemoryCardContainer::open(source)?;
    let count = container.active_slot_indices().len();
    container.export_to_ps1_card_image(target_file)?;
    println!(
        "Exported {} Parasite Eve save slot(s) from {} to PS1 memory card image {}.",
        count,
        source.display(),
        target_file.display()
    );
    Ok(())
}

fn cmd_export_json(source: &PathBuf, output: &PathBuf, slot: Option<usize>) -> Result<()> {
    let container = MemoryCardContainer::open(source)?;
    let idx = resolve_slot_index(&container, slot)?;
    let save = container.slots[idx].as_ref().unwrap();
    let json = serde_json::to_string_pretty(save)?;
    fs::write(output, json)
        .with_context(|| format!("Failed to write JSON to {}", output.display()))?;
    println!(
        "Exported Slot {} ({}) to {}",
        idx + 1,
        save.filename,
        output.display()
    );
    Ok(())
}

fn cmd_import_json(input: &PathBuf, target: &PathBuf, slot: Option<usize>) -> Result<()> {
    let json_str = fs::read_to_string(input)
        .with_context(|| format!("Failed to read JSON file {}", input.display()))?;
    let mut imported: PeSaveSlot = serde_json::from_str(&json_str)
        .with_context(|| format!("Failed to parse PeSaveSlot JSON from {}", input.display()))?;

    let mut container = MemoryCardContainer::open(target)?;
    let target_idx = if let Some(s) = slot {
        if !(1..=15).contains(&s) {
            bail!("Slot must be 1..=15");
        }
        s - 1
    } else {
        imported.slot_index.min(14)
    };
    imported.slot_index = target_idx;
    let _ = imported.to_bytes();
    container.slots[target_idx] = Some(imported);
    container.save()?;
    println!(
        "Imported JSON save into Slot {} of {}",
        target_idx + 1,
        target.display()
    );
    Ok(())
}

fn cmd_list_items(filter: Option<&str>) -> Result<()> {
    let needle = filter.map(|s| s.to_lowercase());

    println!("=== PARASITE EVE ITEMS (0x00..=0xFF) ===");
    for meta in ITEM_TABLE.iter() {
        let line = format!(
            "0x{:02X} ({:3}) | {:18} | {:24} | {}",
            meta.id,
            meta.id,
            meta.category.label(),
            meta.display_name,
            meta.description
        );
        if let Some(ref n) = needle {
            if !line.to_lowercase().contains(n) && !meta.code_name.to_lowercase().contains(n) {
                continue;
            }
        }
        println!("  {}", line);
    }

    println!("\n=== WEAPON MODIFICATIONS (0x00..=0x14) ===");
    for m in WEAPON_MOD_TABLE.iter() {
        let line = format!("0x{:02X} | Weapon   | {:24} | {}", m.id, m.name, m.description);
        if let Some(ref n) = needle {
            if !line.to_lowercase().contains(n) && !m.code_name.to_lowercase().contains(n) {
                continue;
            }
        }
        println!("  {}", line);
    }

    println!("\n=== ARMOR MODIFICATIONS (0x00..=0x11) ===");
    for m in ARMOR_MOD_TABLE.iter() {
        let line = format!("0x{:02X} | Armor    | {:24} | {}", m.id, m.name, m.description);
        if let Some(ref n) = needle {
            if !line.to_lowercase().contains(n) && !m.code_name.to_lowercase().contains(n) {
                continue;
            }
        }
        println!("  {}", line);
    }

    println!("\n=== PARASITE ENERGY SPELLS (Bits 0..19) ===");
    for sp in PARASITE_SPELLS.iter() {
        let line = format!(
            "Bit {:2} (0x{:05X}) | {:16} | {:18} | {}",
            sp.bit_index,
            1u32 << sp.bit_index,
            sp.category,
            sp.name,
            sp.pe_cost_note
        );
        if let Some(ref n) = needle {
            if !line.to_lowercase().contains(n) {
                continue;
            }
        }
        println!("  {}", line);
    }
    Ok(())
}
