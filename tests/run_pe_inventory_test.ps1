param([string]$BuildDirectory)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if (!$BuildDirectory) { $BuildDirectory = Join-Path $root 'build-mods' }
$cache = [IO.File]::ReadAllText((Join-Path $BuildDirectory 'CMakeCache.txt'))
$match = [regex]::Match($cache, '(?m)^CMAKE_C_COMPILER:(?:STRING|FILEPATH)=(.+)\r?$')
if (!$match.Success) { throw 'Configure the native CMake build first.' }
$compiler = $match.Groups[1].Value.Trim()
$output = Join-Path $BuildDirectory 'test-pe-mod-inventory.exe'
$compileArgs = @('-D__psyz', '-DPE_MODS=1', '-std=c11', '-O1', '-fno-strict-aliasing', '-Wno-pointer-sign')
foreach ($path in @('include', 'mods/host', 'mods/pe_qol', 'third_party/psyz/psyz/include', 'third_party/psyz/external/SDL/include')) {
    $compileArgs += '-I'; $compileArgs += Join-Path $root $path
}
foreach ($path in @('tests/pe_mod_inventory_test.c', 'mods/pe_qol/pe_key_items.c', 'mods/pe_qol/pe_bag_sort.c', 'mods/host/pe_save.c', 'src/main/item/armor_capacity.c')) {
    $compileArgs += Join-Path $root $path
}
$compileArgs += '-o'; $compileArgs += $output
& $compiler @compileArgs
if ($LASTEXITCODE -ne 0) { throw 'Inventory test compilation failed.' }
& $output
if ($LASTEXITCODE -ne 0) { throw 'Inventory regression test failed.' }

$menuOutput = Join-Path $BuildDirectory 'test-pe-mod-inventory-menu.exe'
$menuArgs = @('-D__psyz', '-DPE_MODS=1', '-std=c11', '-O1', '-fno-strict-aliasing', '-Wno-pointer-sign')
foreach ($path in @('include', 'mods/host', 'mods/pe_qol', 'third_party/psyz/psyz/include', 'third_party/psyz/external/SDL/include')) {
    $menuArgs += '-I'; $menuArgs += Join-Path $root $path
}
foreach ($path in @('tests/pe_mod_inventory_menu_test.c', 'mods/pe_qol/pe_inventory_menu.c')) {
    $menuArgs += Join-Path $root $path
}
$menuArgs += '-o'; $menuArgs += $menuOutput
& $compiler @menuArgs
if ($LASTEXITCODE -ne 0) { throw 'Inventory menu test compilation failed.' }
& $menuOutput
if ($LASTEXITCODE -ne 0) { throw 'Inventory menu regression test failed.' }