use std::path::{Path, PathBuf};

use eframe::egui::{
    self, Color32, ColorImage, CornerRadius, CursorIcon, Margin, RichText, Sense, Stroke,
    TextWrapMode, TextureHandle, TextureOptions, Vec2,
};

use crate::memcard::{MemoryCardContainer, MAX_CARD_SLOTS};
use crate::save::{EquipmentRecord, PeSaveSlot, SCRIPT_VAR_SIZE};
use crate::tables::{
    item_metadata, map_location_descriptive, mod_metadata, ItemCategory, ARMOR_MOD_TABLE,
    ITEM_TABLE, MAP_TELEPORT_PRESETS, PARASITE_SPELLS, STAT_GROWTH_NAMES, WEAPON_MOD_TABLE,
};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum EditorTab {
    InventoryAndLoadout,
    CharacterAndStats,
    ParasiteEnergy,
    ArmoryRecords,
    WayneVault,
    StoryAndTeleport,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum ItemPickerFilter {
    All,
    Consumables,
    Ammo,
    UpgradesAndTools,
    Weapons,
    Armor,
    KeysAndCards,
}

impl ItemPickerFilter {
    fn label(self) -> &'static str {
        match self {
            Self::All => "All",
            Self::Consumables => "💊 Meds",
            Self::Ammo => "弹 Ammo",
            Self::UpgradesAndTools => "🔧 Tools / +N",
            Self::Weapons => "🔫 Weapons",
            Self::Armor => "🛡 Armor",
            Self::KeysAndCards => "🔑 Keys & Cards",
        }
    }

    fn matches(self, cat: ItemCategory) -> bool {
        match self {
            Self::All => cat != ItemCategory::Empty,
            Self::Consumables => cat == ItemCategory::Consumable,
            Self::Ammo => matches!(cat, ItemCategory::Ammo | ItemCategory::Crate),
            Self::UpgradesAndTools => matches!(cat, ItemCategory::StatUpgrade | ItemCategory::Tool),
            Self::Weapons => cat.is_weapon(),
            Self::Armor => cat.is_armor(),
            Self::KeysAndCards => matches!(
                cat,
                ItemCategory::KeyItem | ItemCategory::TradingCard | ItemCategory::ParasiteEnergy
            ),
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum WayneSubTab {
    EquipmentVault,
    RegularItems,
}

pub struct SaveEditorApp {
    container: Option<MemoryCardContainer>,
    selected_slot: usize,
    active_tab: EditorTab,
    slot_textures: [Option<TextureHandle>; MAX_CARD_SLOTS],
    status_message: String,
    is_error_status: bool,

    // Master-detail selections
    selected_inv_slot: usize,
    show_locked_inv_slots: bool,
    inv_replace_mode: bool,

    selected_equip_index: usize,
    show_only_active_equip: bool,

    wayne_subtab: WayneSubTab,
    selected_wayne_regular_slot: usize,
    selected_wayne_special_slot: usize,

    // Shared item picker filter state
    picker_category: ItemPickerFilter,
    picker_search: String,

    // Diagnostics / Advanced
    show_hidden_pe_spells: bool,
    show_script_hex_inspector: bool,
    script_inspector_offset: usize,
}

const BAG_SIZE_HELP: &str = "The game sets the bag size from the Item Capacity stat (up to 46), \
    so saving raises or lowers that stat to match. Armor with \
    Item Capacity mods adds up to 50 in game.";

const MAX_HP_HELP: &str = "The game recomputes max HP from the HP stat at every battle, so saving \
    raises or lowers that stat to match (exactly with a game disc loaded). The top is 869, or 999 \
    while the equipped armor has HP Max Up.";

const SORT_HELP: &str = "Weapons, armor, ammo, medicine, other items, then key items; empty slots \
    last. Copies of one weapon or armor go best first. The equipped gun and armor stay equipped. \
    Takes effect when you save.";

// Theme Palette Constants
const BG_DARKEST: Color32 = Color32::from_rgb(11, 16, 14);
const BG_PANEL: Color32 = Color32::from_rgb(16, 23, 20);
const BG_CARD: Color32 = Color32::from_rgb(22, 32, 27);
const BG_CARD_HOVER: Color32 = Color32::from_rgb(30, 46, 38);
const BG_CARD_SELECTED: Color32 = Color32::from_rgb(28, 64, 45);
const BORDER_SUBTLE: Color32 = Color32::from_rgb(42, 62, 52);
const BORDER_ACTIVE: Color32 = Color32::from_rgb(95, 230, 150);
const ACCENT_GREEN: Color32 = Color32::from_rgb(105, 240, 160);
const ACCENT_AMBER: Color32 = Color32::from_rgb(245, 200, 95);
const ACCENT_CYAN: Color32 = Color32::from_rgb(110, 210, 245);
const TEXT_MUTED: Color32 = Color32::from_rgb(145, 168, 155);

impl SaveEditorApp {
    pub fn new(cc: &eframe::CreationContext<'_>, initial_path: Option<PathBuf>) -> Self {
        configure_mito_theme(&cc.egui_ctx);

        let mut app = Self {
            container: None,
            selected_slot: 0,
            active_tab: EditorTab::InventoryAndLoadout,
            slot_textures: Default::default(),
            status_message:
                "Ready. Open a PsyZ folder (bu00/bu10) or a PS1 Memory Card (.srm/.mcr/.gme)."
                    .to_string(),
            is_error_status: false,
            selected_inv_slot: 0,
            show_locked_inv_slots: false,
            inv_replace_mode: false,
            selected_equip_index: 0,
            show_only_active_equip: true,
            wayne_subtab: WayneSubTab::EquipmentVault,
            selected_wayne_regular_slot: 0,
            selected_wayne_special_slot: 0,
            picker_category: ItemPickerFilter::All,
            picker_search: String::new(),
            show_hidden_pe_spells: false,
            show_script_hex_inspector: false,
            script_inspector_offset: 0,
        };

        crate::disc::load_configured_disc();

        if let Some(path) = initial_path {
            app.load_path(&cc.egui_ctx, path);
        } else {
            let candidates = [
                PathBuf::from("bu00"),
                PathBuf::from("../bu00"),
                PathBuf::from("save-editor/samples/Parasite_Eve_PS1_Sample_Card.srm"),
                PathBuf::from("samples/Parasite_Eve_PS1_Sample_Card.srm"),
            ];
            for cand in candidates {
                if cand.exists() {
                    app.load_path(&cc.egui_ctx, cand);
                    break;
                }
            }
        }

        app
    }

    fn set_status(&mut self, msg: impl Into<String>, is_error: bool) {
        self.status_message = msg.into();
        self.is_error_status = is_error;
    }

    fn load_path(&mut self, ctx: &egui::Context, path: PathBuf) {
        match MemoryCardContainer::open(&path) {
            Ok(container) => {
                let first_active = container.first_active_slot_index().unwrap_or(0);
                let active_count = container.active_slot_indices().len();
                let kind_label = container.kind.label();
                self.container = Some(container);
                self.selected_slot = first_active;
                self.selected_inv_slot = 0;
                self.rebuild_icon_textures(ctx);
                self.set_status(
                    format!(
                        "Loaded {} ({}, {} active slot(s)).",
                        path.display(),
                        kind_label,
                        active_count
                    ),
                    false,
                );
            }
            Err(err) => {
                self.set_status(format!("Error opening {}: {:#}", path.display(), err), true);
            }
        }
    }

    fn rebuild_icon_textures(&mut self, ctx: &egui::Context) {
        self.slot_textures = Default::default();
        if let Some(container) = &self.container {
            for i in 0..MAX_CARD_SLOTS {
                if let Some(slot) = &container.slots[i] {
                    let rgba = slot.decode_icon_rgba();
                    let img = ColorImage::from_rgba_unmultiplied([16, 16], &rgba);
                    let tex = ctx.load_texture(
                        format!("pe_slot_icon_{}", i),
                        img,
                        TextureOptions::NEAREST,
                    );
                    self.slot_textures[i] = Some(tex);
                }
            }
        }
    }

    fn save_current_container(&mut self) {
        if let Some(container) = &mut self.container {
            match container.save() {
                Ok(()) => {
                    let path_str = container.path.display().to_string();
                    self.set_status(
                        format!("✔ Saved all slots & recalculated CRC-16 to {}.", path_str),
                        false,
                    );
                }
                Err(err) => {
                    self.set_status(format!("Save failed: {:#}", err), true);
                }
            }
        }
    }
}

fn configure_mito_theme(ctx: &egui::Context) {
    let mut style = (*ctx.style()).clone();
    style.spacing.item_spacing = Vec2::new(8.0, 6.0);
    style.spacing.button_padding = Vec2::new(10.0, 5.0);
    style.visuals = egui::Visuals::dark();
    style.visuals.window_fill = BG_PANEL;
    style.visuals.panel_fill = BG_PANEL;
    style.visuals.faint_bg_color = BG_CARD;
    style.visuals.extreme_bg_color = BG_DARKEST;
    style.visuals.widgets.noninteractive.bg_stroke = Stroke::new(1.0_f32, BORDER_SUBTLE);
    style.visuals.widgets.inactive.bg_fill = Color32::from_rgb(26, 38, 32);
    style.visuals.widgets.inactive.corner_radius = CornerRadius::same(6);
    style.visuals.widgets.hovered.bg_fill = Color32::from_rgb(38, 58, 48);
    style.visuals.widgets.hovered.bg_stroke = Stroke::new(1.0_f32, ACCENT_GREEN);
    style.visuals.widgets.hovered.corner_radius = CornerRadius::same(6);
    style.visuals.widgets.active.bg_fill = BG_CARD_SELECTED;
    style.visuals.widgets.active.corner_radius = CornerRadius::same(6);
    style.visuals.selection.bg_fill = BG_CARD_SELECTED;
    style.visuals.selection.stroke = Stroke::new(1.5_f32, BORDER_ACTIVE);
    ctx.set_style(style);
}

/// Renders a full-width, bounded clickable card that never overflows its parent column or panel.
fn interactive_card(
    ui: &mut egui::Ui,
    id_source: impl std::hash::Hash,
    is_selected: bool,
    accent_border: Option<Color32>,
    add_contents: impl FnOnce(&mut egui::Ui),
) -> egui::Response {
    let id = ui.make_persistent_id(id_source);
    let is_hovered = ui
        .ctx()
        .read_response(id)
        .map(|r| r.hovered())
        .unwrap_or(false);

    let fill = if is_selected {
        BG_CARD_SELECTED
    } else if is_hovered {
        BG_CARD_HOVER
    } else {
        BG_CARD
    };

    let border_color = if is_selected {
        accent_border.unwrap_or(BORDER_ACTIVE)
    } else if is_hovered {
        accent_border.unwrap_or(Color32::from_rgb(70, 130, 95))
    } else {
        accent_border
            .map(|c| c.gamma_multiply(0.45))
            .unwrap_or(BORDER_SUBTLE)
    };

    let stroke_width = if is_selected { 1.8_f32 } else { 1.0_f32 };
    let target_width = ui.available_width().max(60.0);
    let inner_width = (target_width - 20.0).max(40.0);

    let frame = egui::Frame::NONE
        .fill(fill)
        .stroke(Stroke::new(stroke_width, border_color))
        .corner_radius(CornerRadius::same(7))
        .inner_margin(Margin::symmetric(10, 7));

    let inner = frame.show(ui, |ui| {
        ui.set_min_width(inner_width);
        ui.set_max_width(inner_width);
        ui.style_mut().wrap_mode = Some(TextWrapMode::Truncate);
        add_contents(ui);
    });

    let resp = ui.interact(inner.response.rect, id, Sense::click());
    if resp.hovered() {
        ui.ctx().set_cursor_icon(CursorIcon::PointingHand);
    }
    resp
}

fn badge(ui: &mut egui::Ui, text: &str, bg: Color32, fg: Color32) {
    egui::Frame::NONE
        .fill(bg)
        .corner_radius(CornerRadius::same(4))
        .inner_margin(Margin::symmetric(6, 2))
        .show(ui, |ui| {
            ui.label(RichText::new(text).color(fg).small().strong());
        });
}

fn slot_value_icon_and_title(
    save: &PeSaveSlot,
    val: i16,
) -> (&'static str, String, String, Color32) {
    if val <= 0 {
        return (
            "·",
            "Empty Slot".to_string(),
            "Click to place an item or weapon".to_string(),
            TEXT_MUTED,
        );
    }
    if (0x100..0x180).contains(&val) {
        let eq_idx = (val - 0x100) as usize;
        if let Some(rec) = save.equipment.get(eq_idx) {
            if rec.is_active() {
                let eff = rec.effective_stats();
                let mods_count = (0..(rec.tail_count.min(10) as usize))
                    .filter(|&i| (rec.tail_data[i] & 0x1F) != 0)
                    .count();
                if rec.is_armor() {
                    let title = format!(
                        "{}{}",
                        rec.display_name(),
                        if rec.is_engraved() { " ★" } else { "" }
                    );
                    let sub = format!(
                        "Def {} • PE {} • Crit {}  (Mods {}/{})",
                        eff[0], eff[1], eff[2], mods_count, rec.tail_count
                    );
                    return ("🛡", title, sub, ACCENT_CYAN);
                } else {
                    let title = format!(
                        "{}{}",
                        rec.display_name(),
                        if rec.is_engraved() { " ★" } else { "" }
                    );
                    let sub = format!(
                        "Atk {} • Rng {} • Bul {} • Ammo {} (Mods {}/{})",
                        eff[0], eff[1], eff[2], rec.ammo, mods_count, rec.tail_count
                    );
                    return ("🔫", title, sub, ACCENT_GREEN);
                }
            }
        }
        return (
            "❓",
            format!("Unlinked Gear (E#{:02X})", val - 0x100),
            "Inactive equipment record".to_string(),
            ACCENT_AMBER,
        );
    }
    if (1..=0xFF).contains(&val) {
        let meta = item_metadata(val as u8);
        let (icon, color) = match meta.category {
            ItemCategory::Ammo | ItemCategory::Crate => ("弹", ACCENT_AMBER),
            ItemCategory::Consumable => ("💊", Color32::from_rgb(130, 235, 165)),
            ItemCategory::StatUpgrade | ItemCategory::Tool => ("🔧", ACCENT_CYAN),
            ItemCategory::KeyItem => ("🔑", ACCENT_AMBER),
            ItemCategory::TradingCard => ("🃏", Color32::from_rgb(215, 165, 250)),
            ItemCategory::ParasiteEnergy => ("🧬", ACCENT_GREEN),
            _ => ("📦", Color32::WHITE),
        };
        return (
            icon,
            meta.display_name.to_string(),
            meta.description.to_string(),
            color,
        );
    }
    if (0x200..=0x208).contains(&val) {
        let idx = (val - 0x200) as usize;
        let (name, sub) = match idx {
            0 => (
                "9mm Bullets Pool".to_string(),
                format!("Reserve: {} rounds", save.category_counts[0]),
            ),
            1 => (
                "Rockets Pool".to_string(),
                format!("Reserve: {} rockets", save.category_counts[1]),
            ),
            2 => (
                "DNA Bullets Pool".to_string(),
                format!("Reserve: {} rounds", save.category_counts[2]),
            ),
            _ => (
                format!("Special Reserve #{}", idx),
                format!("Raw code 0x{:03X}", val),
            ),
        };
        return ("弹", name, sub, ACCENT_AMBER);
    }
    (
        "📦",
        format!("Code 0x{:04X}", val as u16),
        "Special value".to_string(),
        TEXT_MUTED,
    )
}

impl eframe::App for SaveEditorApp {
    fn update(&mut self, ctx: &egui::Context, _frame: &mut eframe::Frame) {
        if ctx.input_mut(|i| i.consume_key(egui::Modifiers::COMMAND, egui::Key::S)) {
            self.save_current_container();
        }

        // =====================================================================
        // 1. TOP APPLICATION BAR
        // =====================================================================
        egui::TopBottomPanel::top("top_app_bar")
            .frame(
                egui::Frame::NONE
                    .fill(BG_DARKEST)
                    .inner_margin(Margin::symmetric(14, 8))
                    .stroke(Stroke::new(1.0_f32, BORDER_SUBTLE)),
            )
            .show(ctx, |ui| {
                ui.horizontal_wrapped(|ui| {
                    ui.label(
                        RichText::new("🧬 PARASITE EVE")
                            .color(ACCENT_GREEN)
                            .size(16.5)
                            .strong(),
                    );
                    ui.label(RichText::new("SAVE EDITOR").color(TEXT_MUTED).size(12.5));
                    ui.separator();

                    if ui.button("📂 Open PsyZ (bu00)").clicked() {
                        if let Some(folder) = rfd::FileDialog::new()
                            .set_title("Select PsyZ Memory Card Folder (bu00 or bu10)")
                            .pick_folder()
                        {
                            self.load_path(ctx, folder);
                        }
                    }

                    if ui.button("💾 Open PS1 Card (.srm/.mcr)").clicked() {
                        if let Some(file) = rfd::FileDialog::new()
                            .set_title("Open PS1 Memory Card or Save Slot")
                            .add_filter(
                                "PS1 & PsyZ Saves",
                                &["srm", "mcr", "mcd", "gme", "mcs", "bin"],
                            )
                            .add_filter("All Files", &["*"])
                            .pick_file()
                        {
                            self.load_path(ctx, file);
                        }
                    }

                    ui.menu_button("⚡ Quick Open", |ui| {
                        for (label, rel_path) in [
                            ("PsyZ Card 1 (./bu00)", "bu00"),
                            ("PsyZ Card 1 (../bu00)", "../bu00"),
                            ("PsyZ Card 2 (./bu10)", "bu10"),
                            ("PsyZ Card 2 (../bu10)", "../bu10"),
                            (
                                "Sample PS1 Card (7 Saves)",
                                "samples/Parasite_Eve_PS1_Sample_Card.srm",
                            ),
                            (
                                "Sample PS1 Card (save-editor/samples)",
                                "save-editor/samples/Parasite_Eve_PS1_Sample_Card.srm",
                            ),
                        ] {
                            let p = PathBuf::from(rel_path);
                            if p.exists() && ui.button(label).clicked() {
                                self.load_path(ctx, p);
                                ui.close_menu();
                            }
                        }
                    });

                    ui.menu_button("⇄ Convert / Export", |ui| {
                        let has_container = self.container.is_some();
                        if ui
                            .add_enabled(
                                has_container,
                                egui::Button::new("📤 Export All Slots to PsyZ Folder (bu00/)"),
                            )
                            .clicked()
                        {
                            ui.close_menu();
                            if let Some(target_dir) = rfd::FileDialog::new()
                                .set_title("Select Target PsyZ Directory (e.g. bu00)")
                                .pick_folder()
                            {
                                if let Some(container) = &mut self.container {
                                    match container.export_to_psyz_dir(&target_dir) {
                                        Ok(files) => self.set_status(
                                            format!(
                                                "Exported {} slot(s) to PsyZ folder {}.",
                                                files.len(),
                                                target_dir.display()
                                            ),
                                            false,
                                        ),
                                        Err(e) => {
                                            self.set_status(format!("Export error: {:#}", e), true)
                                        }
                                    }
                                }
                            }
                        }

                        if ui
                            .add_enabled(
                                has_container,
                                egui::Button::new("💿 Export All Slots to PS1 Card (.srm / .mcr)"),
                            )
                            .clicked()
                        {
                            ui.close_menu();
                            if let Some(target_file) = rfd::FileDialog::new()
                                .set_title("Export 128 KB PS1 Memory Card (.srm / .mcr)")
                                .set_file_name("ParasiteEve_Card.srm")
                                .add_filter("PS1 Memory Card", &["srm", "mcr", "mcd"])
                                .save_file()
                            {
                                if let Some(container) = &mut self.container {
                                    match container.export_to_ps1_card_image(&target_file) {
                                        Ok(()) => self.set_status(
                                            format!(
                                                "Exported PS1 card image to {}.",
                                                target_file.display()
                                            ),
                                            false,
                                        ),
                                        Err(e) => {
                                            self.set_status(format!("Export error: {:#}", e), true)
                                        }
                                    }
                                }
                            }
                        }
                    });

                    let game = crate::disc::game_data();
                    let disc_btn = ui
                        .button(if game.is_some() { "💿 Game Disc ✔" } else { "💿 Game Disc…" })
                        .on_hover_text(match &game {
                            Some(g) => format!(
                                "Item records and the bag table come from {}.",
                                g.source.display()
                            ),
                            None => "Verified USA equipment templates are available without a disc. \
                                     Pick a USA disc for exact HP growth calculations."
                                .to_string(),
                        });
                    if disc_btn.clicked() {
                        if let Some(file) = rfd::FileDialog::new()
                            .set_title("Select Parasite Eve (USA) Disc Image")
                            .add_filter("Disc Images", &["cue", "bin", "iso", "img"])
                            .add_filter("All Files", &["*"])
                            .pick_file()
                        {
                            match crate::disc::load_disc(&file) {
                                Ok(g) => self.set_status(
                                    format!("Game data loaded from {}.", g.source.display()),
                                    false,
                                ),
                                Err(e) => self.set_status(format!("{:#}", e), true),
                            }
                        }
                    }

                    ui.separator();

                    let has_container = self.container.is_some();
                    let save_btn = egui::Button::new(
                        RichText::new("💾 Save Changes (Cmd+S)")
                            .color(Color32::from_rgb(10, 20, 14))
                            .strong(),
                    )
                    .fill(ACCENT_GREEN)
                    .corner_radius(CornerRadius::same(6));

                    if ui.add_enabled(has_container, save_btn).clicked() {
                        self.save_current_container();
                    }
                });
            });

        // =====================================================================
        // 2. BOTTOM STATUS BAR
        // =====================================================================
        egui::TopBottomPanel::bottom("bottom_status_bar")
            .frame(
                egui::Frame::NONE
                    .fill(BG_DARKEST)
                    .inner_margin(Margin::symmetric(14, 5))
                    .stroke(Stroke::new(1.0_f32, BORDER_SUBTLE)),
            )
            .show(ctx, |ui| {
                ui.horizontal(|ui| {
                    let color = if self.is_error_status {
                        Color32::from_rgb(255, 110, 110)
                    } else {
                        ACCENT_GREEN
                    };
                    ui.label(RichText::new(&self.status_message).color(color).small());
                    if let Some(container) = &self.container {
                        ui.separator();
                        ui.label(
                            RichText::new(format!(
                                "{} • {}",
                                container.kind.label(),
                                container.path.display()
                            ))
                            .color(TEXT_MUTED)
                            .small(),
                        );
                    }
                });
            });

        // =====================================================================
        // 3. LEFT SIDEBAR: 15 INTERACTIVE MEMORY CARD SLOTS
        // =====================================================================
        egui::SidePanel::left("slots_sidebar")
            .resizable(true)
            .default_width(270.0)
            .min_width(235.0)
            .max_width(340.0)
            .frame(
                egui::Frame::NONE
                    .fill(BG_DARKEST)
                    .inner_margin(Margin::symmetric(10, 10))
                    .stroke(Stroke::new(1.0_f32, BORDER_SUBTLE)),
            )
            .show(ctx, |ui| {
                ui.horizontal(|ui| {
                    ui.label(
                        RichText::new("MEMORY CARD SLOTS")
                            .color(TEXT_MUTED)
                            .small()
                            .strong(),
                    );
                    if let Some(c) = &self.container {
                        badge(
                            ui,
                            &format!("{}/15", c.active_slot_indices().len()),
                            BG_CARD,
                            ACCENT_GREEN,
                        );
                    }
                });
                ui.add_space(4.0);

                let Some(container) = &self.container else {
                    ui.label(RichText::new("No memory card loaded.").color(TEXT_MUTED));
                    return;
                };

                let mut clicked_slot: Option<usize> = None;

                egui::ScrollArea::vertical()
                    .id_salt("slots_sidebar_scroll")
                    .auto_shrink([false, false])
                    .show(ui, |ui| {
                        for i in 0..MAX_CARD_SLOTS {
                            let slot_letter = (b'A' + i as u8) as char;
                            let is_selected = self.selected_slot == i;

                            if let Some(save) = &container.slots[i] {
                                let accent = if save.is_clear_data {
                                    Some(ACCENT_AMBER)
                                } else {
                                    None
                                };
                                let resp = interactive_card(
                                    ui,
                                    ("mc_slot_card", i),
                                    is_selected,
                                    accent,
                                    |ui| {
                                        ui.horizontal(|ui| {
                                            if let Some(tex) = &self.slot_textures[i] {
                                                ui.image((tex.id(), Vec2::new(34.0, 34.0)));
                                            }
                                            ui.vertical(|ui| {
                                                ui.horizontal(|ui| {
                                                    ui.label(
                                                        RichText::new(format!(
                                                            "Slot {:02} ({})",
                                                            i + 1,
                                                            slot_letter
                                                        ))
                                                        .strong()
                                                        .size(13.0),
                                                    );
                                                    if save.is_clear_data {
                                                        badge(
                                                            ui,
                                                            "CLEAR",
                                                            Color32::from_rgb(75, 55, 18),
                                                            ACCENT_AMBER,
                                                        );
                                                    } else {
                                                        badge(
                                                            ui,
                                                            &format!("DAY {}", save.play_time_class),
                                                            Color32::from_rgb(24, 52, 38),
                                                            ACCENT_GREEN,
                                                        );
                                                    }
                                                    badge(
                                                        ui,
                                                        &format!("Lv {}", save.display_level()),
                                                        Color32::from_rgb(28, 42, 56),
                                                        ACCENT_CYAN,
                                                    );
                                                });

                                                ui.label(
                                                    RichText::new(save.location_description())
                                                        .color(Color32::from_rgb(210, 225, 215))
                                                        .size(11.5),
                                                );

                                                ui.label(
                                                    RichText::new(format!(
                                                        "HP {}/{}  •  {}",
                                                        save.current_hp,
                                                        save.max_hp,
                                                        save.formatted_play_time()
                                                    ))
                                                    .color(TEXT_MUTED)
                                                    .small(),
                                                );
                                            });
                                        });
                                    },
                                );
                                if resp.clicked() {
                                    clicked_slot = Some(i);
                                }
                            } else {
                                let resp = interactive_card(
                                    ui,
                                    ("mc_slot_empty", i),
                                    is_selected,
                                    None,
                                    |ui| {
                                        ui.horizontal(|ui| {
                                            ui.label(
                                                RichText::new(format!(
                                                    "Slot {:02} ({}) — Empty",
                                                    i + 1,
                                                    slot_letter
                                                ))
                                                .color(TEXT_MUTED)
                                                .size(12.5),
                                            );
                                        });
                                    },
                                );
                                if resp.clicked() {
                                    clicked_slot = Some(i);
                                }
                            }
                            ui.add_space(3.0);
                        }
                    });

                if let Some(idx) = clicked_slot {
                    self.selected_slot = idx;
                }
            });

        // =====================================================================
        // 4. CHECK FOR EMPTY CONTAINER OR EMPTY SLOT BEFORE RENDERING PANELS
        // =====================================================================
        if self.container.is_none() {
            egui::CentralPanel::default()
                .frame(egui::Frame::NONE.fill(BG_PANEL).inner_margin(Margin::same(20)))
                .show(ctx, |ui| {
                    ui.centered_and_justified(|ui| {
                        ui.vertical_centered(|ui| {
                            ui.add_space(100.0);
                            ui.heading("No Save Container Loaded");
                            ui.label(
                                RichText::new("Use the top bar to open a PsyZ directory (bu00) or PS1 Memory Card (.srm/.mcr).")
                                    .color(TEXT_MUTED),
                            );
                        });
                    });
                });
            return;
        }

        let is_empty_slot = self
            .container
            .as_ref()
            .and_then(|c| c.slots[self.selected_slot].as_ref())
            .is_none();

        if is_empty_slot {
            let first_active = self
                .container
                .as_ref()
                .and_then(|c| c.first_active_slot_index());
            let slot_letter = (b'A' + self.selected_slot as u8) as char;
            let mut clone_from: Option<usize> = None;

            egui::CentralPanel::default()
                .frame(egui::Frame::NONE.fill(BG_PANEL).inner_margin(Margin::same(20)))
                .show(ctx, |ui| {
                    ui.vertical_centered(|ui| {
                        ui.add_space(80.0);
                        ui.heading(format!(
                            "Slot {:02} ({}) is Empty",
                            self.selected_slot + 1,
                            slot_letter
                        ));
                        ui.add_space(8.0);
                        if let Some(src_idx) = first_active {
                            ui.label(
                                RichText::new(format!(
                                    "You can clone Slot {:02} into this empty slot to create a backup or experimental build.",
                                    src_idx + 1
                                ))
                                .color(TEXT_MUTED),
                            );
                            ui.add_space(12.0);
                            if ui
                                .button(
                                    RichText::new(format!(
                                        "📋 Clone Slot {:02} into Slot {:02} ({})",
                                        src_idx + 1,
                                        self.selected_slot + 1,
                                        slot_letter
                                    ))
                                    .strong(),
                                )
                                .clicked()
                            {
                                clone_from = Some(src_idx);
                            }
                        }
                    });
                });

            if let Some(src_idx) = clone_from {
                let target_idx = self.selected_slot;
                let ok = self
                    .container
                    .as_mut()
                    .map(|c| c.clone_slot(src_idx, target_idx).is_ok())
                    .unwrap_or(false);
                if ok {
                    self.rebuild_icon_textures(ctx);
                    self.set_status(
                        format!(
                            "Cloned Slot {:02} into Slot {:02}.",
                            src_idx + 1,
                            target_idx + 1
                        ),
                        false,
                    );
                }
            }
            return;
        }

        // =====================================================================
        // 5. HERO SUMMARY HEADER + PRIMARY NAVIGATION BAR (Top Panel of Workspace)
        // =====================================================================
        let mut status_toast: Option<&'static str> = None;
        let slot_tex = self.slot_textures[self.selected_slot].clone();

        egui::TopBottomPanel::top("workspace_hero_and_tabs")
            .frame(
                egui::Frame::NONE
                    .fill(BG_PANEL)
                    .inner_margin(Margin::symmetric(14, 10))
                    .stroke(Stroke::new(1.0_f32, BORDER_SUBTLE)),
            )
            .show(ctx, |ui| {
                let save = self.container.as_mut().unwrap().slots[self.selected_slot]
                    .as_mut()
                    .unwrap();

                // Hero Card (2 clean wrapped rows — never overlaps!)
                egui::Frame::NONE
                    .fill(BG_DARKEST)
                    .stroke(Stroke::new(1.0_f32, BORDER_SUBTLE))
                    .corner_radius(CornerRadius::same(8))
                    .inner_margin(Margin::symmetric(12, 8))
                    .show(ui, |ui| {
                        // Row 1: Icon + Character Identity + Location + Play Time
                        ui.horizontal_wrapped(|ui| {
                            if let Some(tex) = &slot_tex {
                                ui.image((tex.id(), Vec2::new(36.0, 36.0)));
                            }
                            ui.label(
                                RichText::new(&save.aya_name)
                                    .size(17.5)
                                    .strong()
                                    .color(Color32::WHITE),
                            );
                            badge(
                                ui,
                                &format!("Lv {}", save.display_level()),
                                Color32::from_rgb(28, 52, 70),
                                ACCENT_CYAN,
                            );
                            if save.is_clear_data {
                                badge(
                                    ui,
                                    &format!("EX ROUND {}", save.new_game_count + 1),
                                    Color32::from_rgb(75, 55, 18),
                                    ACCENT_AMBER,
                                );
                            } else {
                                badge(
                                    ui,
                                    &format!("DAY {}", save.play_time_class),
                                    Color32::from_rgb(24, 52, 38),
                                    ACCENT_GREEN,
                                );
                            }

                            ui.separator();
                            ui.label(
                                RichText::new(format!(
                                    "📍 {} (Map {})",
                                    save.location_description(),
                                    save.current_map_number
                                ))
                                .color(Color32::from_rgb(210, 230, 215))
                                .strong(),
                            );
                            ui.separator();
                            ui.label(
                                RichText::new(format!("⏱ {}", save.formatted_play_time()))
                                    .color(TEXT_MUTED),
                            );
                            ui.separator();
                            ui.label(
                                RichText::new(format!("CRC 0x{:04X}", save.stored_crc))
                                    .color(TEXT_MUTED)
                                    .monospace()
                                    .small(),
                            );
                        });

                        ui.add_space(4.0);

                        // Row 2: HP Bar + BP / EXP / Bag + Clickable Equipped Weapon & Armor Pills
                        ui.horizontal_wrapped(|ui| {
                            let hp_ratio = if save.max_hp > 0 {
                                (save.current_hp as f32 / save.max_hp as f32).clamp(0.0, 1.0)
                            } else {
                                0.0
                            };
                            ui.label(RichText::new("HP").color(TEXT_MUTED).small().strong());
                            ui.add(
                                egui::ProgressBar::new(hp_ratio)
                                    .desired_width(130.0)
                                    .fill(Color32::from_rgb(45, 165, 95))
                                    .text(format!("{} / {}", save.current_hp, save.max_hp)),
                            );
                            if save.current_hp < save.max_hp && ui.small_button("Heal").clicked() {
                                save.current_hp = save.max_hp;
                                let _ = save.to_bytes();
                            }

                            let used_slots = save
                                .inventory_items
                                .iter()
                                .take(save.inventory_slot_count as usize)
                                .filter(|&&v| v != 0)
                                .count();
                            badge(
                                ui,
                                &format!("BP: {}", save.bonus_points),
                                BG_CARD,
                                ACCENT_AMBER,
                            );
                            badge(
                                ui,
                                &format!("EXP: {}", save.total_exp),
                                BG_CARD,
                                Color32::WHITE,
                            );
                            badge(
                                ui,
                                &format!("Bag: {}/{}", used_slots, save.inventory_slot_count),
                                BG_CARD,
                                ACCENT_GREEN,
                            );

                            ui.separator();

                            // Clickable Equipped Weapon Button
                            let w_slot = save.equipped_weapon_slot;
                            let w_val = if (0..50).contains(&w_slot) {
                                save.inventory_items[w_slot as usize]
                            } else {
                                0
                            };
                            let (_, w_title, _, _) = slot_value_icon_and_title(save, w_val);
                            if ui
                                .button(
                                    RichText::new(format!("🔫 Weapon: {}", w_title))
                                        .color(ACCENT_GREEN)
                                        .strong(),
                                )
                                .on_hover_text("Click to inspect & edit equipped weapon")
                                .clicked()
                                && (0..50).contains(&w_slot)
                            {
                                self.selected_inv_slot = w_slot as usize;
                                self.inv_replace_mode = false;
                                self.active_tab = EditorTab::InventoryAndLoadout;
                            }

                            // Clickable Equipped Armor Button
                            let a_slot = save.equipped_armor_slot;
                            let a_val = if (0..50).contains(&a_slot) {
                                save.inventory_items[a_slot as usize]
                            } else {
                                0
                            };
                            let (_, a_title, _, _) = slot_value_icon_and_title(save, a_val);
                            if ui
                                .button(
                                    RichText::new(format!("🛡 Armor: {}", a_title))
                                        .color(ACCENT_CYAN)
                                        .strong(),
                                )
                                .on_hover_text("Click to inspect & edit equipped armor")
                                .clicked()
                                && (0..50).contains(&a_slot)
                            {
                                self.selected_inv_slot = a_slot as usize;
                                self.inv_replace_mode = false;
                                self.active_tab = EditorTab::InventoryAndLoadout;
                            }
                        });
                    });

                ui.add_space(6.0);

                // Navigation Tabs Row + Quick Cheats Dropdown
                ui.horizontal_wrapped(|ui| {
                    for (tab, label) in [
                        (EditorTab::InventoryAndLoadout, "🎒 Inventory & Loadout"),
                        (EditorTab::CharacterAndStats, "👩 Character & Stats"),
                        (EditorTab::ParasiteEnergy, "🧬 Parasite Energy"),
                        (EditorTab::ArmoryRecords, "🔫 All Equipment (128)"),
                        (EditorTab::WayneVault, "🏛 Wayne's Storage"),
                        (EditorTab::StoryAndTeleport, "🗺 Story & Teleport"),
                    ] {
                        let is_active = self.active_tab == tab;
                        let btn = egui::Button::new(
                            RichText::new(label)
                                .size(13.0)
                                .strong()
                                .color(if is_active {
                                    ACCENT_GREEN
                                } else {
                                    Color32::from_rgb(200, 215, 205)
                                }),
                        )
                        .fill(if is_active {
                            BG_CARD_SELECTED
                        } else {
                            BG_DARKEST
                        })
                        .stroke(Stroke::new(
                            if is_active { 1.5_f32 } else { 1.0_f32 },
                            if is_active {
                                BORDER_ACTIVE
                            } else {
                                BORDER_SUBTLE
                            },
                        ))
                        .corner_radius(CornerRadius::same(6));

                        if ui.add(btn).clicked() {
                            self.active_tab = tab;
                        }
                    }

                    ui.separator();

                    ui.menu_button(
                        RichText::new("⚡ Quick Cheats & Presets")
                            .color(ACCENT_AMBER)
                            .strong(),
                        |ui| {
                            ui.set_min_width(270.0);
                            if ui.button("Repair Equipment Icons & Models").clicked() {
                                save.repair_equipment_types();
                                let _ = save.to_bytes();
                                status_toast = Some("Repaired equipment types; upgrades preserved.");
                                ui.close_menu();
                            }
                            if ui
                                .button("✨ Max Aya (Lv 99 • Max HP • 99,999 BP • 46 Slots)")
                                .on_hover_text(MAX_HP_HELP)
                                .clicked()
                            {
                                save.apply_cheat_max_aya();
                                let _ = save.to_bytes();
                                status_toast = Some("Applied Max Aya stats.");
                                ui.close_menu();
                            }
                            if ui
                                .button("🧬 Unlock All Playable Parasite Energy Spells")
                                .clicked()
                            {
                                save.apply_cheat_unlock_all_pe();
                                let _ = save.to_bytes();
                                status_toast = Some("Unlocked all playable PE spells.");
                                ui.close_menu();
                            }
                            if ui
                                .button("弹 Max All Ammo Reserves (999) & Refill Guns")
                                .clicked()
                            {
                                save.apply_cheat_max_ammo();
                                let _ = save.to_bytes();
                                status_toast = Some("Maxed all ammo pools to 999.");
                                ui.close_menu();
                            }
                            if ui
                                .button("🔫 Upgrade All Owned Weapons & Armor (+999 / 10 Slots)")
                                .clicked()
                            {
                                save.apply_cheat_max_equipped_gear();
                                let _ = save.to_bytes();
                                status_toast = Some("Upgraded all owned weapons & armor to +999.");
                                ui.close_menu();
                            }
                            ui.separator();
                            if ui
                                .button("🏆 Equip Ultimate Loadout (P90 + Cm Suit 2 + Super Tool Kit)")
                                .on_hover_text(
                                    "Adds an ultimate P90 and Cm Suit 2 and puts them on (the old \
                                     gun and armor stay in the bag), plus tool kits and medicine. \
                                     The suit's HP Max Up lets Max Aya reach 999 HP.",
                                )
                                .clicked()
                            {
                                if save.apply_cheat_give_ultimate_loadout().is_ok() {
                                    let _ = save.to_bytes();
                                    status_toast = Some(
                                        "Equipped Ultimate P90 & Cm Suit 2; added Super Tool Kit.",
                                    );
                                }
                                ui.close_menu();
                            }
                        },
                    );
                });
            });

        if let Some(msg) = status_toast {
            self.set_status(msg, false);
        }

        // =====================================================================
        // 6. TAB BODY (Uses dedicated SidePanel::right for Master-Detail splits
        //    so Left List and Right Inspector NEVER overlap!)
        // =====================================================================
        match self.active_tab {
            EditorTab::InventoryAndLoadout => {
                self.show_tab_inventory(ctx);
            }
            EditorTab::CharacterAndStats => {
                egui::CentralPanel::default()
                    .frame(
                        egui::Frame::NONE
                            .fill(BG_PANEL)
                            .inner_margin(Margin::symmetric(14, 10)),
                    )
                    .show(ctx, |ui| {
                        let save = self.container.as_mut().unwrap().slots[self.selected_slot]
                            .as_mut()
                            .unwrap();
                        egui::ScrollArea::vertical()
                            .id_salt("char_stats_scroll")
                            .auto_shrink([false, false])
                            .show(ui, |ui| {
                                render_tab_character_and_stats(ui, save);
                            });
                    });
            }
            EditorTab::ParasiteEnergy => {
                egui::CentralPanel::default()
                    .frame(
                        egui::Frame::NONE
                            .fill(BG_PANEL)
                            .inner_margin(Margin::symmetric(14, 10)),
                    )
                    .show(ctx, |ui| {
                        let save = self.container.as_mut().unwrap().slots[self.selected_slot]
                            .as_mut()
                            .unwrap();
                        render_tab_parasite_energy_cards(
                            ui,
                            save,
                            &mut self.show_hidden_pe_spells,
                        );
                    });
            }
            EditorTab::ArmoryRecords => {
                self.show_tab_armory(ctx);
            }
            EditorTab::WayneVault => {
                self.show_tab_wayne(ctx);
            }
            EditorTab::StoryAndTeleport => {
                self.show_tab_story_and_teleport(ctx);
            }
        }
    }
}

impl SaveEditorApp {
    // =========================================================================
    // TAB 1: INVENTORY & LOADOUT (Right Inspector SidePanel + Central Bag Grid)
    // =========================================================================
    fn show_tab_inventory(&mut self, ctx: &egui::Context) {
        egui::SidePanel::right("inventory_inspector_panel")
            .resizable(true)
            .default_width(470.0)
            .min_width(370.0)
            .max_width(640.0)
            .frame(
                egui::Frame::NONE
                    .fill(BG_DARKEST)
                    .inner_margin(Margin::symmetric(14, 12))
                    .stroke(Stroke::new(1.0_f32, BORDER_SUBTLE)),
            )
            .show(ctx, |ui| {
                let save = self.container.as_mut().unwrap().slots[self.selected_slot]
                    .as_mut()
                    .unwrap();
                let idx = self.selected_inv_slot.min(49);
                let val = save.inventory_items[idx];
                let (icon, title, sub, accent) = slot_value_icon_and_title(save, val);

                // Inspector Header
                ui.horizontal_wrapped(|ui| {
                    ui.label(RichText::new(icon).size(20.0));
                    ui.label(
                        RichText::new(format!("Slot #{:02} — {}", idx + 1, title))
                            .size(15.5)
                            .strong()
                            .color(Color32::WHITE),
                    );
                    if save.equipped_weapon_slot == idx as i8 {
                        badge(
                            ui,
                            "EQUIPPED WEAPON",
                            Color32::from_rgb(24, 65, 42),
                            ACCENT_GREEN,
                        );
                    }
                    if save.equipped_armor_slot == idx as i8 {
                        badge(
                            ui,
                            "EQUIPPED ARMOR",
                            Color32::from_rgb(24, 52, 68),
                            ACCENT_CYAN,
                        );
                    }
                });
                ui.label(RichText::new(sub).color(accent).size(12.0));

                ui.add_space(4.0);
                ui.horizontal_wrapped(|ui| {
                    if (0x100..0x180).contains(&val) {
                        let eq_idx = (val - 0x100) as usize;
                        let is_armor = save.equipment[eq_idx].is_armor();
                        if !is_armor {
                            let is_eq = save.equipped_weapon_slot == idx as i8;
                            if ui
                                .add_enabled(!is_eq, egui::Button::new("🔫 Equip Weapon"))
                                .clicked()
                            {
                                save.equipped_weapon_slot = idx as i8;
                            }
                        } else {
                            let is_eq = save.equipped_armor_slot == idx as i8;
                            if ui
                                .add_enabled(!is_eq, egui::Button::new("🛡 Equip Armor"))
                                .clicked()
                            {
                                save.equipped_armor_slot = idx as i8;
                            }
                        }

                        if ui
                            .button(if self.inv_replace_mode {
                                "🔧 Edit Gear Stats & Mods"
                            } else {
                                "⇄ Replace Item in Slot"
                            })
                            .clicked()
                        {
                            self.inv_replace_mode = !self.inv_replace_mode;
                        }

                        if ui.button("🏛 Move to Wayne").clicked() {
                            if let Some(free_w) =
                                save.special_storage.iter().position(|&v| v == 0)
                            {
                                save.special_storage[free_w] = val;
                                save.inventory_items[idx] = 0;
                                return;
                            }
                        }
                    }

                    if val != 0 && ui.button("🗑 Empty Slot").clicked() {
                        save.inventory_items[idx] = 0;
                        self.inv_replace_mode = false;
                    }
                });

                ui.separator();

                if (0x100..0x180).contains(&val) && !self.inv_replace_mode {
                    let eq_idx = (val - 0x100) as usize;
                    egui::ScrollArea::vertical()
                        .id_salt("inv_embedded_gear_scroll")
                        .auto_shrink([false, false])
                        .show(ui, |ui| {
                            render_equipment_record_editor(
                                ui,
                                &mut save.equipment[eq_idx],
                                idx,
                            );
                        });
                } else {
                    render_catalog_picker_for_slot(
                        ui,
                        save,
                        idx,
                        false,
                        &mut self.picker_category,
                        &mut self.picker_search,
                        &mut self.inv_replace_mode,
                    );
                }
            });

        egui::CentralPanel::default()
            .frame(
                egui::Frame::NONE
                    .fill(BG_PANEL)
                    .inner_margin(Margin::symmetric(14, 10)),
            )
            .show(ctx, |ui| {
                let save = self.container.as_mut().unwrap().slots[self.selected_slot]
                    .as_mut()
                    .unwrap();

                // Ammo Reserves Bar
                egui::Frame::NONE
                    .fill(BG_DARKEST)
                    .stroke(Stroke::new(1.0_f32, BORDER_SUBTLE))
                    .corner_radius(CornerRadius::same(7))
                    .inner_margin(Margin::symmetric(10, 7))
                    .show(ui, |ui| {
                        ui.horizontal_wrapped(|ui| {
                            ui.label(
                                RichText::new("AMMO RESERVES:")
                                    .color(TEXT_MUTED)
                                    .small()
                                    .strong(),
                            );
                            ui.label("弹 9mm:");
                            ui.add(egui::DragValue::new(&mut save.category_counts[0]).range(0..=9999));
                            ui.separator();
                            ui.label("🚀 Rockets:");
                            ui.add(egui::DragValue::new(&mut save.category_counts[1]).range(0..=9999));
                            ui.separator();
                            ui.label("🧬 DNA:");
                            ui.add(egui::DragValue::new(&mut save.category_counts[2]).range(0..=9999));
                            if ui.small_button("Max 999").clicked() {
                                save.apply_cheat_max_ammo();
                            }
                            ui.separator();
                            ui.label("Base Bag Slots:");
                            ui.add(
                                egui::DragValue::new(&mut save.inventory_slot_count)
                                    .range(10..=crate::disc::MAX_STAT_BAG_SLOTS),
                            )
                            .on_hover_text(BAG_SIZE_HELP);
                            ui.checkbox(&mut self.show_locked_inv_slots, "Show Locked (..50)");
                            ui.separator();
                            if ui
                                .button("⇅ Sort Bag")
                                .on_hover_text(SORT_HELP)
                                .clicked()
                            {
                                save.sort_aya_bag();
                            }
                        });
                    });

                ui.add_space(6.0);

                let max_visible = if self.show_locked_inv_slots {
                    50
                } else {
                    (save.inventory_slot_count as usize).clamp(10, 50)
                };

                egui::ScrollArea::vertical()
                    .id_salt("inv_bag_cards_scroll")
                    .auto_shrink([false, false])
                    .show(ui, |ui| {
                        let use_two_cols = ui.available_width() >= 460.0;
                        let step = if use_two_cols { 2 } else { 1 };

                        for row_start in (0..max_visible).step_by(step) {
                            ui.columns(step, |cols| {
                                for offset in 0..step {
                                    let idx = row_start + offset;
                                    if idx >= max_visible {
                                        continue;
                                    }
                                    let val = save.inventory_items[idx];
                                    let is_sel = self.selected_inv_slot == idx;
                                    let is_eq_wpn = save.equipped_weapon_slot == idx as i8;
                                    let is_eq_arm = save.equipped_armor_slot == idx as i8;
                                    let is_locked = idx >= save.inventory_slot_count as usize;

                                    let (icon, title, sub, accent) =
                                        slot_value_icon_and_title(save, val);
                                    let border_override = if is_eq_wpn {
                                        Some(ACCENT_GREEN)
                                    } else if is_eq_arm {
                                        Some(ACCENT_CYAN)
                                    } else {
                                        None
                                    };

                                    let resp = interactive_card(
                                        &mut cols[offset],
                                        ("aya_inv_slot_card", idx),
                                        is_sel,
                                        border_override,
                                        |ui| {
                                            ui.horizontal(|ui| {
                                                ui.label(
                                                    RichText::new(format!("#{:02}", idx + 1))
                                                        .color(if is_locked {
                                                            Color32::from_rgb(100, 90, 90)
                                                        } else {
                                                            TEXT_MUTED
                                                        })
                                                        .monospace()
                                                        .small(),
                                                );
                                                if is_eq_wpn {
                                                    badge(
                                                        ui,
                                                        "EQUIPPED WPN",
                                                        Color32::from_rgb(24, 65, 42),
                                                        ACCENT_GREEN,
                                                    );
                                                } else if is_eq_arm {
                                                    badge(
                                                        ui,
                                                        "EQUIPPED ARM",
                                                        Color32::from_rgb(24, 52, 68),
                                                        ACCENT_CYAN,
                                                    );
                                                }
                                                ui.label(RichText::new(icon).size(13.5));
                                                ui.label(
                                                    RichText::new(&title)
                                                        .color(if val <= 0 {
                                                            TEXT_MUTED
                                                        } else {
                                                            Color32::WHITE
                                                        })
                                                        .strong()
                                                        .size(13.0),
                                                );
                                            });
                                            ui.label(
                                                RichText::new(sub)
                                                    .color(accent.gamma_multiply(0.85))
                                                    .size(11.0),
                                            );
                                        },
                                    );

                                    if resp.clicked() {
                                        self.selected_inv_slot = idx;
                                        self.inv_replace_mode = false;
                                    }
                                }
                            });
                            ui.add_space(2.0);
                        }
                    });
            });
    }

    // =========================================================================
    // TAB 4: ARMORY RECORDS (Right Inspector SidePanel + Central Record List)
    // =========================================================================
    fn show_tab_armory(&mut self, ctx: &egui::Context) {
        egui::SidePanel::right("armory_inspector_panel")
            .resizable(true)
            .default_width(470.0)
            .min_width(370.0)
            .max_width(640.0)
            .frame(
                egui::Frame::NONE
                    .fill(BG_DARKEST)
                    .inner_margin(Margin::symmetric(14, 12))
                    .stroke(Stroke::new(1.0_f32, BORDER_SUBTLE)),
            )
            .show(ctx, |ui| {
                let save = self.container.as_mut().unwrap().slots[self.selected_slot]
                    .as_mut()
                    .unwrap();
                let idx = self.selected_equip_index.min(127);
                let target_val = (0x100 + idx) as i16;

                ui.horizontal_wrapped(|ui| {
                    ui.heading(format!(
                        "Record E#{:02X} — {}",
                        idx,
                        save.equipment[idx].display_name()
                    ));
                    if !save.inventory_items.contains(&target_val)
                        && ui.button("🎒 Put in Aya's Bag").clicked()
                    {
                        if let Some(free_s) = save.inventory_items.iter().position(|&v| v == 0) {
                            save.inventory_items[free_s] = target_val;
                        }
                    }
                });
                ui.separator();

                egui::ScrollArea::vertical()
                    .id_salt("armory_detail_scroll")
                    .auto_shrink([false, false])
                    .show(ui, |ui| {
                        render_equipment_record_editor(
                            ui,
                            &mut save.equipment[idx],
                            1000 + idx,
                        );
                    });
            });

        egui::CentralPanel::default()
            .frame(
                egui::Frame::NONE
                    .fill(BG_PANEL)
                    .inner_margin(Margin::symmetric(14, 10)),
            )
            .show(ctx, |ui| {
                let save = self.container.as_mut().unwrap().slots[self.selected_slot]
                    .as_mut()
                    .unwrap();

                ui.horizontal(|ui| {
                    ui.label(
                        RichText::new("EQUIPMENT RECORDS (0x100..0x17F)")
                            .color(TEXT_MUTED)
                            .small()
                            .strong(),
                    );
                    ui.checkbox(&mut self.show_only_active_equip, "Show Only Active Records");
                });
                ui.add_space(4.0);

                egui::ScrollArea::vertical()
                    .id_salt("armory_records_scroll")
                    .auto_shrink([false, false])
                    .show(ui, |ui| {
                        for i in 0..128 {
                            let rec = &save.equipment[i];
                            if self.show_only_active_equip
                                && !rec.is_active()
                                && self.selected_equip_index != i
                            {
                                continue;
                            }
                            let target_val = (0x100 + i) as i16;
                            let in_aya_bag =
                                save.inventory_items.iter().position(|&v| v == target_val);
                            let in_wayne =
                                save.special_storage.iter().position(|&v| v == target_val);

                            let owner_badge = if let Some(slot) = in_aya_bag {
                                if save.equipped_weapon_slot == slot as i8
                                    || save.equipped_armor_slot == slot as i8
                                {
                                    Some(("EQUIPPED", ACCENT_GREEN))
                                } else {
                                    Some(("AYA BAG", ACCENT_CYAN))
                                }
                            } else if in_wayne.is_some() {
                                Some(("WAYNE VAULT", ACCENT_AMBER))
                            } else {
                                None
                            };

                            let resp = interactive_card(
                                ui,
                                ("armory_rec_card", i),
                                self.selected_equip_index == i,
                                owner_badge.map(|(_, c)| c),
                                |ui| {
                                    ui.horizontal(|ui| {
                                        ui.label(
                                            RichText::new(format!("E#{:02X}", i))
                                                .monospace()
                                                .small()
                                                .color(TEXT_MUTED),
                                        );
                                        if let Some((tag, col)) = owner_badge {
                                            badge(ui, tag, BG_DARKEST, col);
                                        }
                                        if rec.is_active() {
                                            ui.label(RichText::new(if rec.is_armor() {
                                                "🛡"
                                            } else {
                                                "🔫"
                                            }));
                                            ui.label(
                                                RichText::new(rec.display_name())
                                                    .strong()
                                                    .color(Color32::WHITE),
                                            );
                                            if rec.is_engraved() {
                                                badge(
                                                    ui,
                                                    "★ WAYNE",
                                                    Color32::from_rgb(75, 55, 18),
                                                    ACCENT_AMBER,
                                                );
                                            }
                                        } else {
                                            ui.label(
                                                RichText::new("Free Record Slot")
                                                    .color(TEXT_MUTED),
                                            );
                                        }
                                    });

                                    if rec.is_active() {
                                        let eff = rec.effective_stats();
                                        ui.label(
                                            RichText::new(format!(
                                                "Stats: {}/{}/{}  •  Mods: {}",
                                                eff[0],
                                                eff[1],
                                                eff[2],
                                                PeSaveSlot::format_equipment_mods(rec)
                                            ))
                                            .color(TEXT_MUTED)
                                            .small(),
                                        );
                                    }
                                },
                            );

                            if resp.clicked() {
                                self.selected_equip_index = i;
                            }
                            ui.add_space(2.0);
                        }
                    });
            });
    }

    // =========================================================================
    // TAB 5: WAYNE'S STORAGE (Right Inspector SidePanel + Central Vault List)
    // =========================================================================
    fn show_tab_wayne(&mut self, ctx: &egui::Context) {
        egui::SidePanel::right("wayne_inspector_panel")
            .resizable(true)
            .default_width(470.0)
            .min_width(370.0)
            .max_width(640.0)
            .frame(
                egui::Frame::NONE
                    .fill(BG_DARKEST)
                    .inner_margin(Margin::symmetric(14, 12))
                    .stroke(Stroke::new(1.0_f32, BORDER_SUBTLE)),
            )
            .show(ctx, |ui| {
                let save = self.container.as_mut().unwrap().slots[self.selected_slot]
                    .as_mut()
                    .unwrap();

                match self.wayne_subtab {
                    WayneSubTab::EquipmentVault => {
                        let idx = self.selected_wayne_special_slot.min(81);
                        let val = save.special_storage[idx];
                        let (_, title, _, _) = slot_value_icon_and_title(save, val);

                        ui.horizontal_wrapped(|ui| {
                            ui.heading(format!("Wayne Vault #{:02} — {}", idx + 1, title));
                            if val != 0 && ui.button("🎒 Move to Aya's Bag").clicked() {
                                if let Some(free_s) =
                                    save.inventory_items.iter().position(|&v| v == 0)
                                {
                                    save.inventory_items[free_s] = val;
                                    save.special_storage[idx] = 0;
                                    return;
                                }
                            }
                            if val != 0 && ui.button("🗑 Clear").clicked() {
                                save.special_storage[idx] = 0;
                            }
                        });
                        ui.separator();

                        if (0x100..0x180).contains(&val) {
                            let eq_idx = (val - 0x100) as usize;
                            egui::ScrollArea::vertical()
                                .id_salt("wayne_spec_eq_editor")
                                .auto_shrink([false, false])
                                .show(ui, |ui| {
                                    render_equipment_record_editor(
                                        ui,
                                        &mut save.equipment[eq_idx],
                                        2000 + idx,
                                    );
                                });
                        } else {
                            ui.label(
                                RichText::new("Click any weapon or armor below to store it in this Wayne Vault slot:")
                                    .color(TEXT_MUTED),
                            );
                            egui::ScrollArea::vertical()
                                .id_salt("wayne_spec_picker")
                                .auto_shrink([false, false])
                                .show(ui, |ui| {
                                    for meta in ITEM_TABLE
                                        .iter()
                                        .filter(|m| m.category.is_equipment())
                                    {
                                        if interactive_card(
                                            ui,
                                            ("wayne_new_eq_pick", meta.id),
                                            false,
                                            None,
                                            |ui| {
                                                ui.horizontal(|ui| {
                                                    ui.label(
                                                        RichText::new(meta.display_name).strong(),
                                                    );
                                                    badge(
                                                        ui,
                                                        meta.category.label(),
                                                        BG_DARKEST,
                                                        ACCENT_GREEN,
                                                    );
                                                    ui.label(
                                                        RichText::new(meta.description)
                                                            .color(TEXT_MUTED)
                                                            .small(),
                                                    );
                                                });
                                            },
                                        )
                                        .clicked()
                                        {
                                            if let Some(eq_val) =
                                                save.allocate_equipment_from_item(meta.id)
                                            {
                                                save.special_storage[idx] = eq_val;
                                            }
                                        }
                                        ui.add_space(2.0);
                                    }
                                });
                        }
                    }
                    WayneSubTab::RegularItems => {
                        let idx = self.selected_wayne_regular_slot.min(99);
                        let val = save.storage[idx];
                        let (_, title, _, _) = slot_value_icon_and_title(save, val);

                        ui.horizontal_wrapped(|ui| {
                            ui.heading(format!("Wayne Item #{:02} — {}", idx + 1, title));
                            if val != 0 && ui.button("🎒 Move to Aya's Bag").clicked() {
                                if let Some(free_s) =
                                    save.inventory_items.iter().position(|&v| v == 0)
                                {
                                    save.inventory_items[free_s] = val;
                                    save.storage[idx] = 0;
                                }
                            }
                            if val != 0 && ui.button("🗑 Clear").clicked() {
                                save.storage[idx] = 0;
                            }
                        });
                        ui.separator();
                        let mut dummy_replace = false;
                        render_catalog_picker_for_slot(
                            ui,
                            save,
                            idx,
                            true,
                            &mut self.picker_category,
                            &mut self.picker_search,
                            &mut dummy_replace,
                        );
                    }
                }
            });

        egui::CentralPanel::default()
            .frame(
                egui::Frame::NONE
                    .fill(BG_PANEL)
                    .inner_margin(Margin::symmetric(14, 10)),
            )
            .show(ctx, |ui| {
                let save = self.container.as_mut().unwrap().slots[self.selected_slot]
                    .as_mut()
                    .unwrap();

                ui.horizontal(|ui| {
                    ui.selectable_value(
                        &mut self.wayne_subtab,
                        WayneSubTab::EquipmentVault,
                        "🔫 Wayne's Equipment Vault (82 Slots)",
                    );
                    ui.selectable_value(
                        &mut self.wayne_subtab,
                        WayneSubTab::RegularItems,
                        "💊 Wayne's Regular Item Storage (100 Slots)",
                    );
                    ui.separator();
                    if ui
                        .button("⇅ Sort Wayne's Storage")
                        .on_hover_text(SORT_HELP)
                        .clicked()
                    {
                        save.sort_wayne_storage();
                    }
                });
                ui.add_space(4.0);

                match self.wayne_subtab {
                    WayneSubTab::EquipmentVault => {
                        egui::ScrollArea::vertical()
                            .id_salt("wayne_special_cards_scroll")
                            .auto_shrink([false, false])
                            .show(ui, |ui| {
                                for i in 0..82 {
                                    let val = save.special_storage[i];
                                    let (icon, title, sub, accent) =
                                        slot_value_icon_and_title(save, val);
                                    let resp = interactive_card(
                                        ui,
                                        ("wayne_spec_card", i),
                                        self.selected_wayne_special_slot == i,
                                        None,
                                        |ui| {
                                            ui.horizontal(|ui| {
                                                ui.label(
                                                    RichText::new(format!("Vault #{:02}", i + 1))
                                                        .monospace()
                                                        .small()
                                                        .color(TEXT_MUTED),
                                                );
                                                ui.label(RichText::new(icon));
                                                ui.label(
                                                    RichText::new(title).strong().color(
                                                        if val <= 0 {
                                                            TEXT_MUTED
                                                        } else {
                                                            Color32::WHITE
                                                        },
                                                    ),
                                                );
                                            });
                                            if val > 0 {
                                                ui.label(
                                                    RichText::new(sub).color(accent).small(),
                                                );
                                            }
                                        },
                                    );
                                    if resp.clicked() {
                                        self.selected_wayne_special_slot = i;
                                    }
                                    ui.add_space(2.0);
                                }
                            });
                    }
                    WayneSubTab::RegularItems => {
                        egui::ScrollArea::vertical()
                            .id_salt("wayne_reg_cards_scroll")
                            .auto_shrink([false, false])
                            .show(ui, |ui| {
                                let use_two_cols = ui.available_width() >= 460.0;
                                let step = if use_two_cols { 2 } else { 1 };
                                for row_start in (0..100).step_by(step) {
                                    ui.columns(step, |scols| {
                                        for off in 0..step {
                                            let i = row_start + off;
                                            if i >= 100 {
                                                continue;
                                            }
                                            let val = save.storage[i];
                                            let (icon, title, _, _) =
                                                slot_value_icon_and_title(save, val);
                                            if interactive_card(
                                                &mut scols[off],
                                                ("wayne_reg_card", i),
                                                self.selected_wayne_regular_slot == i,
                                                None,
                                                |ui| {
                                                    ui.horizontal(|ui| {
                                                        ui.label(
                                                            RichText::new(format!(
                                                                "#{:02}",
                                                                i + 1
                                                            ))
                                                            .monospace()
                                                            .small()
                                                            .color(TEXT_MUTED),
                                                        );
                                                        ui.label(RichText::new(icon));
                                                        ui.label(
                                                            RichText::new(title).strong().color(
                                                                if val <= 0 {
                                                                    TEXT_MUTED
                                                                } else {
                                                                    Color32::WHITE
                                                                },
                                                            ),
                                                        );
                                                    });
                                                },
                                            )
                                            .clicked()
                                            {
                                                self.selected_wayne_regular_slot = i;
                                            }
                                        }
                                    });
                                    ui.add_space(2.0);
                                }
                            });
                    }
                }
            });
    }

    // =========================================================================
    // TAB 6: STORY & TELEPORT (Right Teleport Cards + Central Story Controls)
    // =========================================================================
    fn show_tab_story_and_teleport(&mut self, ctx: &egui::Context) {
        egui::SidePanel::right("story_teleport_panel")
            .resizable(true)
            .default_width(470.0)
            .min_width(360.0)
            .max_width(640.0)
            .frame(
                egui::Frame::NONE
                    .fill(BG_DARKEST)
                    .inner_margin(Margin::symmetric(14, 12))
                    .stroke(Stroke::new(1.0_f32, BORDER_SUBTLE)),
            )
            .show(ctx, |ui| {
                let save = self.container.as_mut().unwrap().slots[self.selected_slot]
                    .as_mut()
                    .unwrap();

                ui.label(
                    RichText::new("CLICK ANY LOCATION CARD TO TELEPORT AYA")
                        .color(TEXT_MUTED)
                        .small()
                        .strong(),
                );
                ui.add_space(4.0);

                egui::ScrollArea::vertical()
                    .id_salt("teleport_cards_scroll")
                    .auto_shrink([false, false])
                    .show(ui, |ui| {
                        for &(map_id, day_id, desc) in MAP_TELEPORT_PRESETS.iter() {
                            let is_current = save.current_map_number == map_id;
                            let resp = interactive_card(
                                ui,
                                ("teleport_preset_card", map_id),
                                is_current,
                                if is_current {
                                    Some(ACCENT_GREEN)
                                } else {
                                    None
                                },
                                |ui| {
                                    ui.horizontal(|ui| {
                                        badge(
                                            ui,
                                            &format!("DAY {}", day_id),
                                            BG_DARKEST,
                                            ACCENT_GREEN,
                                        );
                                        badge(
                                            ui,
                                            &format!("Map {:03}", map_id),
                                            BG_DARKEST,
                                            ACCENT_CYAN,
                                        );
                                        ui.label(
                                            RichText::new(desc).strong().color(Color32::WHITE),
                                        );
                                    });
                                },
                            );

                            if resp.clicked() {
                                save.current_map_number = map_id;
                                save.play_time_class = day_id;
                                save.current_story_day = day_id as i8;
                            }
                            ui.add_space(3.0);
                        }
                    });
            });

        egui::CentralPanel::default()
            .frame(
                egui::Frame::NONE
                    .fill(BG_PANEL)
                    .inner_margin(Margin::symmetric(14, 10)),
            )
            .show(ctx, |ui| {
                let save = self.container.as_mut().unwrap().slots[self.selected_slot]
                    .as_mut()
                    .unwrap();

                egui::ScrollArea::vertical()
                    .id_salt("story_controls_scroll")
                    .auto_shrink([false, false])
                    .show(ui, |ui| {
                        egui::Frame::NONE
                            .fill(BG_DARKEST)
                            .stroke(Stroke::new(1.0_f32, BORDER_SUBTLE))
                            .corner_radius(CornerRadius::same(8))
                            .inner_margin(Margin::symmetric(14, 12))
                            .show(ui, |ui| {
                                ui.label(
                                    RichText::new("CURRENT STORY DAY & MAP NUMBER")
                                        .color(TEXT_MUTED)
                                        .small()
                                        .strong(),
                                );
                                ui.separator();

                                egui::Grid::new("story_day_map_grid")
                                    .num_columns(2)
                                    .spacing([14.0, 10.0])
                                    .show(ui, |ui| {
                                        ui.label("Save Header Day (1..6):");
                                        ui.add(
                                            egui::Slider::new(&mut save.play_time_class, 1..=6),
                                        );
                                        ui.end_row();

                                        ui.label("Engine Story Chapter (1..8):");
                                        ui.add(
                                            egui::Slider::new(&mut save.current_story_day, 1..=8),
                                        );
                                        ui.end_row();

                                        ui.label("Room / Map ID (1..437):");
                                        ui.add(
                                            egui::Slider::new(
                                                &mut save.current_map_number,
                                                1..=437,
                                            ),
                                        );
                                        ui.end_row();

                                        ui.label("Location Name:");
                                        ui.label(
                                            RichText::new(map_location_descriptive(
                                                save.current_map_number,
                                            ))
                                            .color(ACCENT_GREEN)
                                            .strong(),
                                        );
                                        ui.end_row();
                                    });
                            });

                        ui.add_space(8.0);

                        egui::Frame::NONE
                            .fill(BG_DARKEST)
                            .stroke(Stroke::new(1.0_f32, BORDER_SUBTLE))
                            .corner_radius(CornerRadius::same(8))
                            .inner_margin(Margin::symmetric(14, 12))
                            .show(ui, |ui| {
                                ui.horizontal_wrapped(|ui| {
                                    ui.label(
                                        RichText::new(
                                            "ADVANCED: SCRIPT VARIABLE BANK & CRC-16",
                                        )
                                        .color(TEXT_MUTED)
                                        .small()
                                        .strong(),
                                    );
                                    ui.checkbox(
                                        &mut self.show_script_hex_inspector,
                                        "Show Hex Inspector",
                                    );
                                });
                                ui.separator();
                                ui.label(
                                    RichText::new(format!(
                                        "CRC-16 Checksum at 0x1C8C: Stored 0x{:04X} | Calculated 0x{:04X}",
                                        save.stored_crc, save.calculated_crc
                                    ))
                                    .color(ACCENT_GREEN),
                                );

                                if self.show_script_hex_inspector {
                                    ui.add_space(6.0);
                                    ui.horizontal(|ui| {
                                        ui.label("Script Bank Offset (0x000..0x7F0):");
                                        ui.add(
                                            egui::Slider::new(
                                                &mut self.script_inspector_offset,
                                                0..=(SCRIPT_VAR_SIZE - 16),
                                            )
                                            .hexadecimal(3, false, true),
                                        );
                                    });
                                    let base_off = (self.script_inspector_offset / 8) * 8;
                                    egui::Grid::new("script_hex_grid")
                                        .striped(true)
                                        .num_columns(9)
                                        .spacing([6.0, 4.0])
                                        .show(ui, |ui| {
                                            for row in 0..8 {
                                                let row_off = base_off + row * 8;
                                                if row_off >= SCRIPT_VAR_SIZE {
                                                    break;
                                                }
                                                ui.label(
                                                    RichText::new(format!("0x{:03X}", row_off))
                                                        .monospace()
                                                        .color(TEXT_MUTED),
                                                );
                                                for col in 0..8 {
                                                    let off = row_off + col;
                                                    let mut b = save.read_script_u8(off);
                                                    if ui
                                                        .add(
                                                            egui::DragValue::new(&mut b)
                                                                .range(0..=255)
                                                                .hexadecimal(2, false, false),
                                                        )
                                                        .changed()
                                                    {
                                                        save.write_script_u8(off, b);
                                                    }
                                                }
                                                ui.end_row();
                                            }
                                        });
                                }
                            });
                    });
            });
    }
}

/// Clean, non-overlapping Equipment Record Editor (Stats Grid + 10-Slot Mod Grid)
fn render_equipment_record_editor(
    ui: &mut egui::Ui,
    rec: &mut EquipmentRecord,
    id_salt: usize,
) {
    let is_armor = rec.is_armor();
    let cur_meta = item_metadata(rec.item_id);

    ui.horizontal_wrapped(|ui| {
        ui.label(RichText::new("Base Model:").color(TEXT_MUTED));
        egui::ComboBox::from_id_salt(("eq_model_combo", id_salt, rec.index))
            .width(210.0)
            .selected_text(format!(
                "{} ({})",
                cur_meta.display_name,
                cur_meta.category.label()
            ))
            .show_ui(ui, |ui| {
                for meta in ITEM_TABLE.iter().filter(|m| m.category.is_equipment()) {
                    if ui
                        .selectable_label(
                            rec.item_id == meta.id,
                            format!("{} [{}]", meta.display_name, meta.category.label()),
                        )
                        .clicked()
                    {
                        rec.item_id = meta.id;
                        (rec.icon_id, rec.kind) = EquipmentRecord::icon_and_kind_for(meta.id);
                    }
                }
            });

        if ui.button("✨ Max +999 & 10 Slots").clicked() {
            rec.bonus_stats = [999, 999, 999];
            rec.tail_count = 10;
            rec.tail_capacity = 10;
            rec.set_engraved(true);
            if !is_armor {
                rec.ammo = 999;
            }
        }
    });

    let mut engraved = rec.is_engraved();
    if ui
        .checkbox(
            &mut engraved,
            "★ Engraved with Wayne's Name (Carries over into EX Game)",
        )
        .changed()
    {
        rec.set_engraved(engraved);
    }

    ui.add_space(4.0);

    // Clean 4-column table for the 3 stat axes (never overlaps horizontally!)
    let axes = if is_armor {
        [
            ("🛡 Defense", ACCENT_CYAN),
            ("🧬 P.Energy", ACCENT_GREEN),
            ("⚡ Crit Evade", ACCENT_AMBER),
        ]
    } else {
        [
            ("💥 Attack", ACCENT_GREEN),
            ("🎯 Range", ACCENT_CYAN),
            ("弹 Bullets Cap", ACCENT_AMBER),
        ]
    };

    egui::Frame::NONE
        .fill(BG_CARD)
        .stroke(Stroke::new(1.0_f32, BORDER_SUBTLE))
        .corner_radius(CornerRadius::same(6))
        .inner_margin(Margin::symmetric(10, 8))
        .show(ui, |ui| {
            egui::Grid::new(("eq_stat_axes_grid", id_salt, rec.index))
                .num_columns(4)
                .spacing([18.0, 6.0])
                .show(ui, |ui| {
                    ui.label(RichText::new("Parameter").color(TEXT_MUTED).small().strong());
                    ui.label(RichText::new("Base (0..255)").color(TEXT_MUTED).small().strong());
                    ui.label(RichText::new("Bonus (+N)").color(TEXT_MUTED).small().strong());
                    ui.label(RichText::new("Total").color(TEXT_MUTED).small().strong());
                    ui.end_row();

                    for axis in 0..3 {
                        let (label, color) = axes[axis];
                        let total =
                            rec.base_stats[axis] as i32 + rec.bonus_stats[axis] as i32;
                        ui.label(RichText::new(label).strong());
                        ui.add(egui::DragValue::new(&mut rec.base_stats[axis]).range(0..=255));
                        ui.add(
                            egui::DragValue::new(&mut rec.bonus_stats[axis])
                                .range(-999..=999)
                                .prefix("+"),
                        );
                        ui.label(
                            RichText::new(format!("{}", total))
                                .color(color)
                                .size(14.5)
                                .strong(),
                        );
                        ui.end_row();
                    }
                });

            if !is_armor {
                ui.separator();
                ui.horizontal_wrapped(|ui| {
                    ui.label(RichText::new("Loaded Ammo:").color(TEXT_MUTED));
                    ui.add(egui::DragValue::new(&mut rec.ammo).range(0..=999));
                    let cap = (rec.base_stats[2] as i32 + rec.bonus_stats[2] as i32)
                        .clamp(1, 999) as u16;
                    if ui.small_button(format!("Fill Magazine ({})", cap)).clicked() {
                        rec.ammo = cap;
                    }
                });
            }
        });

    ui.add_space(6.0);

    // Modifications Section
    ui.horizontal_wrapped(|ui| {
        ui.label(
            RichText::new("MODIFICATIONS (UP TO 10 SLOTS)")
                .color(TEXT_MUTED)
                .small()
                .strong(),
        );
        ui.separator();
        ui.label("Active Slots:");
        ui.add(egui::Slider::new(&mut rec.tail_count, 0..=10));
    });
    if rec.tail_capacity < rec.tail_count {
        rec.tail_capacity = rec.tail_count;
    }

    let mod_slice = if is_armor {
        &ARMOR_MOD_TABLE[..]
    } else {
        &WEAPON_MOD_TABLE[..]
    };

    egui::Grid::new(("eq_mods_table_grid", id_salt, rec.index))
        .striped(true)
        .num_columns(3)
        .spacing([10.0, 5.0])
        .show(ui, |ui| {
            for slot_i in 0..10 {
                let is_unlocked = (slot_i as u8) < rec.tail_count;
                let cur_mod_id = rec.mod_id_at(slot_i);
                let cur_mod_meta = mod_metadata(cur_mod_id, is_armor);

                ui.label(
                    RichText::new(format!("Mod #{:02}", slot_i + 1))
                        .color(if is_unlocked {
                            ACCENT_GREEN
                        } else {
                            TEXT_MUTED
                        })
                        .monospace()
                        .small(),
                );

                egui::ComboBox::from_id_salt(("eq_mod_slot_combo", id_salt, rec.index, slot_i))
                    .width(260.0)
                    .selected_text(if cur_mod_id == 0 {
                        RichText::new("— Empty Slot —").color(TEXT_MUTED)
                    } else if cur_mod_meta.id != cur_mod_id {
                        RichText::new(format!("Unknown Effect #{} (kept as it is)", cur_mod_id))
                            .color(ACCENT_AMBER)
                    } else {
                        RichText::new(format!(
                            "{} ({})",
                            cur_mod_meta.name, cur_mod_meta.description
                        ))
                        .color(Color32::WHITE)
                    })
                    .show_ui(ui, |ui| {
                        for m in mod_slice.iter() {
                            if ui
                                .selectable_label(
                                    cur_mod_id == m.id,
                                    format!("{} — {}", m.name, m.description),
                                )
                                .clicked()
                            {
                                rec.set_mod_at(slot_i, m.id);
                                if (slot_i as u8) >= rec.tail_count && m.id != 0 {
                                    rec.tail_count = (slot_i as u8) + 1;
                                    rec.tail_capacity = rec.tail_capacity.max(rec.tail_count);
                                }
                            }
                        }
                    });

                if cur_mod_id != 0 {
                    if ui.small_button("✖").on_hover_text("Remove mod").clicked() {
                        rec.set_mod_at(slot_i, 0);
                    }
                } else {
                    ui.label("");
                }
                ui.end_row();
            }
        });
}

fn render_catalog_picker_for_slot(
    ui: &mut egui::Ui,
    save: &mut PeSaveSlot,
    target_slot_idx: usize,
    is_wayne_regular_storage: bool,
    picker_cat: &mut ItemPickerFilter,
    picker_search: &mut String,
    replace_mode: &mut bool,
) {
    ui.label(
        RichText::new("CLICK ANY ITEM BELOW TO PLACE IN THIS SLOT")
            .color(TEXT_MUTED)
            .small()
            .strong(),
    );

    ui.horizontal_wrapped(|ui| {
        for cat in [
            ItemPickerFilter::All,
            ItemPickerFilter::Consumables,
            ItemPickerFilter::Ammo,
            ItemPickerFilter::UpgradesAndTools,
            ItemPickerFilter::Weapons,
            ItemPickerFilter::Armor,
            ItemPickerFilter::KeysAndCards,
        ] {
            if ui
                .selectable_label(*picker_cat == cat, cat.label())
                .clicked()
            {
                *picker_cat = cat;
            }
        }
    });

    ui.horizontal(|ui| {
        ui.label("🔍 Search:");
        ui.text_edit_singleline(picker_search);
        if !picker_search.is_empty() && ui.small_button("Clear").clicked() {
            picker_search.clear();
        }
    });
    ui.add_space(4.0);

    let needle = picker_search.to_lowercase();

    egui::ScrollArea::vertical()
        .id_salt(("catalog_picker_scroll", is_wayne_regular_storage))
        .auto_shrink([false, false])
        .show(ui, |ui| {
            for meta in ITEM_TABLE.iter().skip(1) {
                if !picker_cat.matches(meta.category) {
                    continue;
                }
                if !needle.is_empty()
                    && !meta.display_name.to_lowercase().contains(&needle)
                    && !meta.description.to_lowercase().contains(&needle)
                    && !meta.category.label().to_lowercase().contains(&needle)
                {
                    continue;
                }

                let resp = interactive_card(
                    ui,
                    ("catalog_item_row", meta.id, is_wayne_regular_storage),
                    false,
                    None,
                    |ui| {
                        ui.horizontal(|ui| {
                            ui.label(
                                RichText::new(meta.display_name)
                                    .strong()
                                    .color(Color32::WHITE),
                            );
                            badge(ui, meta.category.label(), BG_DARKEST, ACCENT_GREEN);
                        });
                        ui.label(RichText::new(meta.description).color(TEXT_MUTED).small());
                    },
                );

                if resp.clicked() {
                    let assigned_val = if meta.category.is_equipment() {
                        save.allocate_equipment_from_item(meta.id)
                            .unwrap_or(meta.id as i16)
                    } else {
                        meta.id as i16
                    };

                    if is_wayne_regular_storage {
                        if target_slot_idx < save.storage.len() {
                            save.storage[target_slot_idx] = assigned_val;
                        }
                    } else if target_slot_idx < save.inventory_items.len() {
                        save.inventory_items[target_slot_idx] = assigned_val;
                    }
                    *replace_mode = false;
                }
                ui.add_space(2.0);
            }
        });
}

fn render_tab_character_and_stats(ui: &mut egui::Ui, save: &mut PeSaveSlot) {
    ui.columns(2, |cols| {
        cols[0].vertical(|ui| {
            egui::Frame::NONE
                .fill(BG_DARKEST)
                .stroke(Stroke::new(1.0_f32, BORDER_SUBTLE))
                .corner_radius(CornerRadius::same(8))
                .inner_margin(Margin::symmetric(14, 12))
                .show(ui, |ui| {
                    ui.label(
                        RichText::new("CHARACTER IDENTITY & VITAL ATTRIBUTES")
                            .color(TEXT_MUTED)
                            .small()
                            .strong(),
                    );
                    ui.separator();

                    egui::Grid::new("char_vital_grid")
                        .num_columns(2)
                        .spacing([16.0, 10.0])
                        .show(ui, |ui| {
                            ui.label("Character Name:");
                            ui.text_edit_singleline(&mut save.aya_name);
                            ui.end_row();

                            ui.label("Level (1..99):");
                            let mut lvl = save.display_level();
                            if ui.add(egui::Slider::new(&mut lvl, 1..=99)).changed() {
                                save.set_display_level(lvl);
                            }
                            ui.end_row();

                            ui.label("Current HP:");
                            ui.add(egui::Slider::new(&mut save.current_hp, 1..=999));
                            ui.end_row();

                            ui.label("Maximum HP:");
                            ui.add(egui::Slider::new(&mut save.max_hp, 1..=999))
                                .on_hover_text(MAX_HP_HELP);
                            ui.end_row();

                            ui.label("Total Experience (EXP):");
                            ui.add(
                                egui::DragValue::new(&mut save.total_exp)
                                    .range(0..=99_999_999)
                                    .speed(250),
                            );
                            ui.end_row();

                            ui.label("Bonus Points (BP):");
                            ui.add(
                                egui::DragValue::new(&mut save.bonus_points)
                                    .range(0..=99_999)
                                    .speed(50),
                            );
                            ui.end_row();

                            ui.label("Base Bag Capacity (armor can add up to 4):");
                            ui.add(
                                egui::Slider::new(
                                    &mut save.inventory_slot_count,
                                    10..=crate::disc::MAX_STAT_BAG_SLOTS,
                                )
                                .suffix(" slots"),
                            )
                            .on_hover_text(BAG_SIZE_HELP);
                            ui.end_row();

                            ui.label("PE Energy Bonus Pool:");
                            ui.add(egui::Slider::new(&mut save.pe_bonus_pool, 0..=999));
                            ui.end_row();
                        });
                });

            ui.add_space(8.0);

            egui::Frame::NONE
                .fill(BG_DARKEST)
                .stroke(Stroke::new(1.0_f32, BORDER_SUBTLE))
                .corner_radius(CornerRadius::same(8))
                .inner_margin(Margin::symmetric(14, 12))
                .show(ui, |ui| {
                    ui.label(
                        RichText::new("PLAY TIME, EX ROUND & UI WINDOW THEME")
                            .color(TEXT_MUTED)
                            .small()
                            .strong(),
                    );
                    ui.separator();

                    let total_s = save.play_time_seconds.max(0);
                    let mut hrs = total_s / 3600;
                    let mut mins = (total_s % 3600) / 60;
                    let mut secs = total_s % 60;
                    ui.horizontal_wrapped(|ui| {
                        ui.label("Play Time:");
                        let mut changed = false;
                        changed |= ui
                            .add(egui::DragValue::new(&mut hrs).range(0..=999).suffix(" h"))
                            .changed();
                        changed |= ui
                            .add(egui::DragValue::new(&mut mins).range(0..=59).suffix(" m"))
                            .changed();
                        changed |= ui
                            .add(egui::DragValue::new(&mut secs).range(0..=59).suffix(" s"))
                            .changed();
                        if changed {
                            save.set_play_time_hms(hrs, mins, secs);
                        }
                    });

                    ui.horizontal_wrapped(|ui| {
                        ui.label("EX Playthrough Round (0 = 1st Game):");
                        ui.add(egui::Slider::new(&mut save.new_game_count, 0..=98));
                    });

                    ui.checkbox(
                        &mut save.is_clear_data,
                        "Mark as Clear Data / EX Game Save (Starts New EX Round on Load)",
                    );

                    let mut rgb = [
                        (save.draw_blend_color & 0xFF) as u8,
                        ((save.draw_blend_color >> 8) & 0xFF) as u8,
                        ((save.draw_blend_color >> 16) & 0xFF) as u8,
                    ];
                    ui.horizontal(|ui| {
                        ui.label("In-Game Menu Window Tint:");
                        if ui.color_edit_button_srgb(&mut rgb).changed() {
                            save.draw_blend_color = (rgb[0] as u32)
                                | ((rgb[1] as u32) << 8)
                                | ((rgb[2] as u32) << 16);
                        }
                    });
                });
        });

        cols[1].vertical(|ui| {
            egui::Frame::NONE
                .fill(BG_DARKEST)
                .stroke(Stroke::new(1.0_f32, BORDER_SUBTLE))
                .corner_radius(CornerRadius::same(8))
                .inner_margin(Margin::symmetric(14, 12))
                .show(ui, |ui| {
                    ui.horizontal_wrapped(|ui| {
                        ui.label(
                            RichText::new("STAT GROWTH ALLOCATIONS (0..1000)")
                                .color(TEXT_MUTED)
                                .small()
                                .strong(),
                        );
                        if ui.small_button("Max All to 1000").clicked() {
                            save.stats_levels = [1000; 7];
                        }
                    });
                    ui.separator();
                    ui.label(
                        RichText::new("Combat stats (Offense, Defense, PE, ATB Speed, Recovery) are computed from these 7 growth levels (base = Level × 10, max = 1000).")
                            .color(TEXT_MUTED)
                            .small(),
                    );
                    ui.add_space(8.0);

                    egui::Grid::new("stat_growth_sliders_grid")
                        .striped(true)
                        .num_columns(2)
                        .spacing([14.0, 8.0])
                        .show(ui, |ui| {
                            for i in 0..7 {
                                ui.label(RichText::new(STAT_GROWTH_NAMES[i]).strong());
                                ui.add(
                                    egui::Slider::new(&mut save.stats_levels[i], 0..=1000)
                                        .show_value(true),
                                );
                                ui.end_row();
                            }
                        });
                });
        });
    });
}

fn render_tab_parasite_energy_cards(
    ui: &mut egui::Ui,
    save: &mut PeSaveSlot,
    show_hidden: &mut bool,
) {
    ui.horizontal_wrapped(|ui| {
        ui.label(
            RichText::new("CLICK ANY SPELL CARD TO UNLOCK OR LOCK IT")
                .color(TEXT_MUTED)
                .small()
                .strong(),
        );
        ui.separator();
        if ui.button("✨ Unlock All Playable Spells").clicked() {
            save.apply_cheat_unlock_all_pe();
        }
        if ui.button("Clear All").clicked() {
            save.parasite_spell_flags = 0;
        }
        ui.checkbox(show_hidden, "Show Cut / Dummy Slots (Mito14..Mito18)");
    });
    ui.add_space(6.0);

    let visible_spells: Vec<_> = PARASITE_SPELLS
        .iter()
        .filter(|sp| *show_hidden || !sp.category.contains("Dummy"))
        .collect();

    egui::ScrollArea::vertical()
        .id_salt("pe_spell_cards_scroll")
        .auto_shrink([false, false])
        .show(ui, |ui| {
            let num_cols = if ui.available_width() >= 760.0 { 3 } else { 2 };
            for row_chunk in visible_spells.chunks(num_cols) {
                ui.columns(num_cols, |cols| {
                    for (col_idx, spell) in row_chunk.iter().enumerate() {
                        let unlocked = save.has_spell(spell.bit_index);
                        let resp = interactive_card(
                            &mut cols[col_idx],
                            ("pe_spell_tile", spell.bit_index),
                            unlocked,
                            if unlocked {
                                Some(ACCENT_GREEN)
                            } else {
                                None
                            },
                            |ui| {
                                ui.horizontal(|ui| {
                                    ui.label(
                                        RichText::new(if unlocked { "🧬" } else { "🔒" })
                                            .size(15.0),
                                    );
                                    if unlocked {
                                        badge(
                                            ui,
                                            "UNLOCKED",
                                            Color32::from_rgb(24, 65, 42),
                                            ACCENT_GREEN,
                                        );
                                    } else {
                                        badge(ui, "LOCKED", BG_DARKEST, TEXT_MUTED);
                                    }
                                    ui.label(
                                        RichText::new(spell.name)
                                            .size(14.0)
                                            .strong()
                                            .color(if unlocked {
                                                Color32::WHITE
                                            } else {
                                                TEXT_MUTED
                                            }),
                                    );
                                });
                                ui.label(
                                    RichText::new(format!(
                                        "{}  •  {}",
                                        spell.category, spell.pe_cost_note
                                    ))
                                    .color(TEXT_MUTED)
                                    .small(),
                                );
                            },
                        );

                        if resp.clicked() {
                            save.set_spell(spell.bit_index, !unlocked);
                        }
                    }
                });
                ui.add_space(4.0);
            }
        });
}

pub fn run_gui(initial_path: Option<PathBuf>) -> anyhow::Result<()> {
    let native_options = eframe::NativeOptions {
        viewport: egui::ViewportBuilder::default()
            .with_title("Parasite Eve (SLUS-006.62 / PsyZ PC) Save Editor")
            .with_inner_size([1340.0, 860.0])
            .with_min_inner_size([1024.0, 680.0]),
        ..Default::default()
    };

    eframe::run_native(
        "Parasite Eve Save Editor",
        native_options,
        Box::new(move |cc| Ok(Box::new(SaveEditorApp::new(cc, initial_path)))),
    )
    .map_err(|e| anyhow::anyhow!("GUI error: {}", e))
}

pub fn default_psyz_bu00_dir(workspace_root: &Path) -> PathBuf {
    workspace_root.join("bu00")
}
