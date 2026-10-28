# Parasite Eve

[![Discord](https://img.shields.io/badge/Discord-Join%20the%20community-5865F2?logo=discord&logoColor=white)](https://discord.gg/bQkCVvrkem)

A native PC port of Parasite Eve, built from the original game code with
[our PsyZ fork](https://github.com/khasinski/parasite-eve-psyz).

![Parasite Eve running in the native port](docs/images/gameplay.png)

## Play

Download the game from [GitHub Releases](https://github.com/khasinski/parasite-eve-pc/releases).
This is an early alpha; see the release notes for available platforms and known issues.
See the [changelog](CHANGELOG.md) for changes.

You need your own **North American CUE/BIN images**. Game data is not included.
Keep each CUE file next to its BIN file.

1. Launch Parasite Eve and select your disc images. Selecting both discs enables automatic switching.
2. You can start with either disc. If the other is needed, the game asks for it by name.
3. Press **Escape** for graphics, controls, audio and disc settings. You can add the other disc there later.

**Enter** is START and skips movies; in a story movie, START pauses it and asks whether to skip. View or change all bindings in **Escape → Controls**. A **speed** button can be bound there too: the game runs at 2x while it is held, or 3x or 4x with the arrows either side of the number (Left/Right on that row). It is unbound until you choose a key or button, and keeps your VSync setting.
The left stick provides analog movement. **Escape → Controls → Field run threshold** sets how far to push it before Aya runs (85% by default).
Settings and selected disc paths are saved automatically. Memory cards are kept in `memory-cards` beside your user preferences. Older `bu00`/`bu10` cards from the launch or executable folder are imported once, keeping the original files.
**Escape → Graphics** includes geometry correction, antialiasing, model texture filtering and an optional **Widescreen** aspect ratio. Widescreen shows more of the original background where available; narrow scenes and movies stay in 4:3.

If the game crashes, it saves a report and shows where to find it. Attach that file when reporting the problem on Discord.

## Built-in mods: PE Enhancements

This source tree includes four modular enhancement packs. Build with **`PE_MODS=ON`**
to include them; a standard build leaves the mod host disabled. Downloaded releases
may differ from this checkout. Open **F1 → PE Enhancements** to configure the mods.
Preferences are saved automatically in `pe_mods.ini`.

### Movement and inventory (`pe_qol`)

- **Always Run** and configurable walk, run, combat movement and battle-entry speeds.
  Walking and running default to **1.75×**.
- **Encounter controls:** disable random battles or choose Normal, 2×, 4× or Every
  Check frequency. These change how often encounters occur, not enemies per fight;
  story and room restrictions still apply.
- **Key-item pocket:** story keys and Chrysler elevator keys stay outside the normal
  bag while remaining available to story checks. Browse them through **Key Items**
  in Aya's main menu or the F1 Inventory page.
- **Remembered sorting:** Auto remembers the native sort settings; manually moving
  items switches back to Manual. Press **Square in Aya's Use Item inventory** to
  open sorting. The separate main-menu Sort row is hidden.
- **Optional Debug controls:** God Mode, reserve-ammo refill, never reload, a minimum
  bag size and a minimum unspent bonus-point balance. Cheats and floors default Off.

### Combat and Parasite Energy (`pe_rebalance`)

- **Vanilla, Rebalanced and Custom rules** for weapon effects and Parasite Energy.
- **Weapon and armor effect tuning:** Heat/Frost, Rate of Fire damage and volley
  speed, gun progression bonuses, Attack All crowd damage, status chances, poison,
  Critical Up, Quickdraw and defensive armor tradeoffs.
- **Weapon power estimates:** DPS information in weapon help text.
- **PE adjustments:** healing amounts, selected power costs and Scan information.
- **Half-bar turns:** configurable items, Quick PE powers and weapon changes, with
  a gauge indicator and cancellation handling. Attacks and full-cost powers still
  require a full turn; armor changes and escape remain full-bar by default.
- **Medicine combining:** two Medicine 1/2/3 become the next tier. Combine from a
  medicine's native item action menu or through F1.
- **Panacea:** combine Medicine 4 and Full Cure for an item that restores 400 HP and
  cures ailments.

### Junk, equipment and Workshop (`pe_economy`)

- **Junk economy:** a shared counter, battle and steal rewards, Super/Duper Junk
  conversion, difficulty presets, rare-drop cascades and soft pity.
- **Economy armor effects:** Bargain, Scavenger, Lucky Find and Salvager, with custom
  effect badges and help names in equipment menus.
- **Mod Catalog:** dismantle equipment to unlock effects, then install, remove or
  replace them on equipped gear. Available in Aya's main menu and F1.
- **Dismantle and Rebuild:** salvage spare equipment and buy recorded gear back,
  with protection against repeated salvage/buy-back Junk farming. The optional
  **Discard dismantles** setting integrates salvage into equipment action menus.
- **Requisition:** buy medicine, cures, ammo, Smoke Grenades, Revive, Tools and
  economy-effect unlocks. Includes increasing Tool prices capped at 100 Junk,
  Tool Kit handling, EX Game Revive pricing, and rescue ammo/Spare Club when no
  weapon in the bag or storage can attack.
- **Wayne's Workshop:** native dialogue access to Requisition and Rebuild, Mod Permit
  Cards for an extra equipment slot, and Wardrobe when Collection is included.
  F1 Economy pages remain available.
- **Warner's services:** storage and Workshop access where he stands guard on Day 4,
  controlled by **Economy → Warner helps out**.
- **Smoke Grenades:** guaranteed escape from eligible battles; enemies that forbid
  escape still block them.
- **Inventory Junk counter:** a saved toggle under **Inventory → Junk counter**,
  displaying Junk beside Quantity.
- **Wayne failed-craft refund:** available but **disabled by default**; Super/Duper
  Junk retain their existing partial-refund role.

### Bestiary and Wardrobe (`pe_collection`)

- **Bestiary:** 75 creatures covering the story and Chrysler Building. Kills, Scan
  and Junk purchases reveal creature details, weaknesses and loot information.
- **Wardrobe:** unlock and wear supported Aya outfits, through F1 Collection or
  Wayne's Workshop. Character replacement skins remain listed as unavailable
  until their native loader is ported.
- **Persistent Collection:** unlocks are kept across new games and older saves in
  `pe_collection.ini`; the recomp Collection file can be imported once.
- **Extensible packs:** creature and costume registries let additional mods supply
  their own entries without changing the Collection implementation.

Most options are grouped under **Movement**, **Combat**, **Economy**, **Inventory**,
**Collection** and **Debug** in F1. Some features also have the native menu entry
points described above. Mod builds remain experimental; a successful build or
regression test does not establish complete in-game compatibility.

See [the mod guide](mods/README.md) for module organization, settings, save formats
and focused regression checks. [Deferred custom-content ideas](mods/DEFERRED_CUSTOM_CONTENT.md)
are notes only, not implemented features.


## Build

Requires Git, CMake 3.21 or newer and a C11 compiler.

```sh
git clone https://github.com/khasinski/parasite-eve-pc
cd parasite-eve-pc
git submodule update --init third_party/psyz
git -C third_party/psyz submodule update --init external/SDL
cmake -S . -B build -DBUILD_TESTING=OFF -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --parallel
./build/parasite-eve-pc
```

### Build with all enhancement packs

After the submodule setup above, configure a separate mod build:

```sh
cmake -S . -B build-mods -DBUILD_TESTING=OFF -DCMAKE_BUILD_TYPE=RelWithDebInfo -DPE_MODS=ON "-DPE_MOD_LIST=pe_qol;pe_rebalance;pe_economy;pe_collection"
cmake --build build-mods --parallel
./build-mods/parasite-eve-pc
```

On Windows the executable is `build-mods/parasite-eve-pc.exe`. `PE_MOD_LIST` selects
which packs are compiled in; the command above includes all four, including
Collection in previously configured build directories. In-game settings control
individual features. Use `-DPE_MODS=OFF` for a build without the mod host.

For development details, see [code organization](docs/CODE_ORGANIZATION.md).
