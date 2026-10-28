# Mod host and mods, built only with -DPE_MODS=ON. See mods/README.md.
# Each mod is a folder here holding .c files and defining `const PeMod pe_mod_<folder>`.
# pe_hello is the example mod; add it to the list to build it.
set(PE_MOD_LIST pe_qol pe_rebalance pe_economy pe_collection pe_tools CACHE STRING "Mods built into the game (folder names)")

set(pe_host_sources
    host/pe_mod_host.c      # registry, hooks, services, events, logs
    host/pe_mod_menu.c      # Mods panel rows and pe_mods.ini
    host/pe_mod_panel.c     # the F1 panel: navigation and input
    host/pe_pages.c         # PE Enhancements page layout
    host/pe_game.c          # game access helpers
    host/pe_game_facts.c    # game facts as tables: effect names and groups, power tags
    host/pe_main_menu.c     # extra rows in Aya's main menu, screens from game widgets
    host/pe_equip_sync.c    # edited equipment reaches the next battle
    host/pe_save.c          # mods' per-save data: records the host saves, loads and carries
    host/pe_present.c       # image presenter: pictures pasted into the finished frame
    host/pe_disc_assets.c   # read-only game data from the player's disc (rooms)
    host/pe_model.c         # decoded game models: geometry, textures, idle animation
    host/pe_model_view.c    # a model posed, turned and drawn into a picture
    host/pe_model_stage.c   # a model on show: turning, idle animation, box or full screen
    host/ui/pe_ui_raster.c  # drawing: boxes and text
    host/ui/pe_ui_gpu.c     # the overlay quad
    host/ui/pe_toast.c)     # one-line messages

set(pe_mod_sources ${pe_host_sources})
set(pe_mod_decls "")
set(pe_mod_refs "")
foreach(mod IN LISTS PE_MOD_LIST)
    file(GLOB mod_files CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${mod}/*.c)
    list(APPEND pe_mod_sources ${mod_files})
    string(APPEND pe_mod_decls "extern const PeMod pe_mod_${mod};\n")
    string(APPEND pe_mod_refs "    &pe_mod_${mod},\n")
endforeach()
configure_file(pe_mod_list.c.in ${CMAKE_CURRENT_BINARY_DIR}/pe_mod_list.c @ONLY)

add_library(pe-mods OBJECT ${pe_mod_sources} ${CMAKE_CURRENT_BINARY_DIR}/pe_mod_list.c)
target_include_directories(pe-mods PRIVATE ${PROJECT_SOURCE_DIR}/include host
    "$<TARGET_PROPERTY:SDL3::SDL3,INTERFACE_INCLUDE_DIRECTORIES>")
target_compile_definitions(pe-mods PRIVATE __psyz)
target_link_libraries(pe-mods PRIVATE psyz)

if(BUILD_TESTING AND "pe_qol" IN_LIST PE_MOD_LIST)
    add_executable(pe-mod-frequency
        ${PROJECT_SOURCE_DIR}/tests/pe_mod_frequency_test.c
        pe_qol/pe_battle_frequency.c
        pe_qol/pe_move.c)
    target_include_directories(pe-mod-frequency PRIVATE ${PROJECT_SOURCE_DIR}/include host pe_qol
        "$<TARGET_PROPERTY:SDL3::SDL3,INTERFACE_INCLUDE_DIRECTORIES>")
    target_compile_definitions(pe-mod-frequency PRIVATE __psyz PE_MODS=1)
    target_link_libraries(pe-mod-frequency PRIVATE psyz)
    add_test(NAME pe-mod-frequency COMMAND pe-mod-frequency)
endif()

if(BUILD_TESTING)
    add_executable(test-mod-settings ${PROJECT_SOURCE_DIR}/tests/original_input_rules.c)
    target_include_directories(test-mod-settings PRIVATE ${PROJECT_SOURCE_DIR}/include host)
    target_compile_definitions(test-mod-settings PRIVATE PE_MOD_SETTINGS_TEST=1 __psyz)
    target_link_libraries(test-mod-settings PRIVATE psyz)
    add_test(NAME mod-settings-save-safety COMMAND test-mod-settings "${CMAKE_CURRENT_BINARY_DIR}/settings-evidence")

    add_executable(test-mod-crash-tail ${PROJECT_SOURCE_DIR}/tests/pe_mod_crash_tail_test.c host/pe_mod_host.c host/pe_save.c)
    target_include_directories(test-mod-crash-tail PRIVATE ${PROJECT_SOURCE_DIR}/include host
        "$<TARGET_PROPERTY:SDL3::SDL3,INTERFACE_INCLUDE_DIRECTORIES>")
    target_compile_definitions(test-mod-crash-tail PRIVATE __psyz PE_MODS=1)
    target_link_libraries(test-mod-crash-tail PRIVATE psyz)
    add_test(NAME mod-crash-save-tail COMMAND test-mod-crash-tail)
endif()

if(BUILD_TESTING AND "pe_qol" IN_LIST PE_MOD_LIST)
    add_executable(test-mod-inventory ${PROJECT_SOURCE_DIR}/tests/pe_mod_inventory_test.c
        pe_qol/pe_key_items.c pe_qol/pe_bag_sort.c host/pe_save.c ${PROJECT_SOURCE_DIR}/src/main/item/armor_capacity.c)
    add_executable(test-mod-inventory-menu ${PROJECT_SOURCE_DIR}/tests/pe_mod_inventory_menu_test.c
        pe_qol/pe_inventory_menu.c)
    foreach(target test-mod-inventory test-mod-inventory-menu)
        target_include_directories(${target} PRIVATE ${PROJECT_SOURCE_DIR}/include host pe_qol)
        target_compile_definitions(${target} PRIVATE __psyz PE_MODS=1)
        target_link_libraries(${target} PRIVATE psyz)
        if(MSVC)
            target_compile_options(${target} PRIVATE /UNDEBUG)
        else()
            target_compile_options(${target} PRIVATE -UNDEBUG)
        endif()
    endforeach()
    add_test(NAME mod-inventory COMMAND test-mod-inventory)
    add_test(NAME mod-inventory-menu COMMAND test-mod-inventory-menu)
endif()

if(BUILD_TESTING)
    add_executable(test-mod-save ${PROJECT_SOURCE_DIR}/tests/pe_mod_save_test.c host/pe_save.c)
    target_include_directories(test-mod-save PRIVATE ${PROJECT_SOURCE_DIR}/include host)
    target_compile_definitions(test-mod-save PRIVATE __psyz PE_MODS=1)
    target_link_libraries(test-mod-save PRIVATE psyz)
    add_test(NAME mod-save-records COMMAND test-mod-save)
endif()

if(BUILD_TESTING AND "pe_rebalance" IN_LIST PE_MOD_LIST)
    add_executable(test-mod-rebalance ${PROJECT_SOURCE_DIR}/tests/pe_mod_rebalance_test.c
        pe_rebalance/pe_mod_effects.c ${PROJECT_SOURCE_DIR}/src/main/battle/enemy_movement.c)
    target_include_directories(test-mod-rebalance PRIVATE ${PROJECT_SOURCE_DIR}/include host)
    target_compile_definitions(test-mod-rebalance PRIVATE __psyz PE_MODS=1)
    target_link_libraries(test-mod-rebalance PRIVATE psyz)
    if(MINGW)
        # COFF unwind tables retain unused entry points from the whole original module.
        # This fixture tests damage only; the game keeps its normal unwind metadata.
        target_compile_options(test-mod-rebalance PRIVATE -fno-asynchronous-unwind-tables)
    endif()
    add_test(NAME mod-rebalance-vanilla COMMAND test-mod-rebalance --vanilla)
    add_test(NAME mod-rebalance-saved-vanilla COMMAND test-mod-rebalance --saved-vanilla)
    add_test(NAME mod-rebalance-scoped COMMAND test-mod-rebalance --scoped)
    add_test(NAME mod-rebalance-poison COMMAND test-mod-rebalance --poison)
endif()
if(BUILD_TESTING)
    add_test(NAME mod-preserve-save-tail COMMAND test-mod-save --opaque)
    add_test(NAME mod-save-registration-bounds COMMAND test-mod-save --overflow)
endif()

if(BUILD_TESTING AND "pe_rebalance" IN_LIST PE_MOD_LIST)
    add_executable(test-mod-atb ${PROJECT_SOURCE_DIR}/tests/pe_mod_atb_test.c
        pe_rebalance/pe_atb_items.c pe_rebalance/pe_pe_balance.c host/pe_game_facts.c)
    add_executable(test-mod-combine ${PROJECT_SOURCE_DIR}/tests/pe_mod_combine_colour_test.c
        pe_rebalance/pe_combine.c pe_rebalance/pe_panacea.c)
    add_executable(test-mod-pe ${PROJECT_SOURCE_DIR}/tests/pe_mod_pe_test.c pe_rebalance/pe_pe_balance.c
        ${PROJECT_SOURCE_DIR}/src/main/item/actions.c)
    foreach(target test-mod-atb test-mod-combine test-mod-pe)
        target_include_directories(${target} PRIVATE ${PROJECT_SOURCE_DIR}/include host pe_rebalance)
        target_compile_definitions(${target} PRIVATE __psyz PE_MODS=1)
        target_link_libraries(${target} PRIVATE psyz)
    endforeach()
    if(MINGW)
        # Allow unused original item-module entry points to be discarded in this fixture.
        target_compile_options(test-mod-pe PRIVATE -fno-asynchronous-unwind-tables)
    endif()
    add_test(NAME mod-atb COMMAND test-mod-atb)
    add_test(NAME mod-combine COMMAND test-mod-combine)
    add_test(NAME mod-panacea-off COMMAND test-mod-combine --panacea-off)
    add_test(NAME mod-pe-vanilla COMMAND test-mod-pe --vanilla)
    add_test(NAME mod-pe-saved-vanilla COMMAND test-mod-pe --saved-vanilla)
    add_test(NAME mod-pe-cost COMMAND test-mod-pe --cost)
    add_test(NAME mod-pe-actor COMMAND test-mod-pe --actor)
endif()
