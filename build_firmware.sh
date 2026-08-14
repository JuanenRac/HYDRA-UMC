#!/usr/bin/env bash
# =============================================================================
# build_firmware.sh - Install tools, verify everything, compile HYDRA-UMC's
# own MCU firmware binaries from a clean checkout.
#
# PROJECT: HYDRA-UMC
# AUTHOR: JuanenRac (Electro Hobby 3D) - electrohobby3d@gmail.com
# LICENSE: GPL-3.0 (same as the firmware this builds - see LICENSE at repo root)
#
# Modeled directly on the sibling URTC repo's own build_firmware.sh (same
# pinned-vendor-via-git approach, same pass/warn/fail step reporting) - see
# that script and its own docs/COMPILE_STM32F303.TXT for the pattern this
# follows. Full reasoning for HYDRA-UMC's own two targets is in
# docs/COMPILE_STM32G474.TXT and docs/COMPILE_STM32H745.TXT - read those
# first if anything here needs adjusting; this script automates them, it
# doesn't replace them.
#
# Usage:
#   ./build_firmware.sh              build every target below
#   ./build_firmware.sh --clean      wipe the local build/ cache first
#   ./build_firmware.sh g474         build only the Robot Controller Board (STM32G474RET6)
#   ./build_firmware.sh h745         build only the Kinematic Brain (STM32H745ZIT6, both cores)
#
# STATUS: g474 is a real, verified-compiling smoke test (GPIO toggle) proving
# the toolchain/vendoring pipeline itself works end to end - not yet the real
# CAN-OTA/motion firmware (see firmware/mcu_stm32g474/README.md). h745 is not
# implemented in this script yet - see docs/COMPILE_STM32H745.TXT for the
# manual steps until it is.
# =============================================================================
set -e

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD="$ROOT/build"
FIRMWARE_OUT="$ROOT/firmware_out"

# Pinned versions - see docs/COMPILE_STM32G474.TXT section 3 for why pinned
# rather than tracking each repo's own latest master.
G4_HAL_REPO="https://github.com/STMicroelectronics/stm32g4xx_hal_driver.git"
G4_HAL_TAG="v1.2.7"
G4_CMSIS_DEVICE_REPO="https://github.com/STMicroelectronics/cmsis_device_g4.git"
G4_CMSIS_DEVICE_TAG="v1.2.6"
CMSIS_CORE_REPO="https://github.com/STMicroelectronics/cmsis_core.git"
CMSIS_CORE_TAG="v5.9.0_20250520"
H7_HAL_REPO="https://github.com/STMicroelectronics/stm32h7xx_hal_driver.git"
H7_HAL_TAG="v1.11.6"
H7_CMSIS_DEVICE_REPO="https://github.com/STMicroelectronics/cmsis_device_h7.git"
H7_CMSIS_DEVICE_TAG="v1.10.7"

PASS=0; WARN=0; FAIL=0
pass() { echo "  OK   $1"; PASS=$((PASS+1)); }
warn() { echo "  WARN $1"; WARN=$((WARN+1)); }
fail() { echo "  FAIL $1"; FAIL=$((FAIL+1)); }
step() { echo ""; echo "=== $1 ==="; }

TARGET="${1:-all}"
if [ "$1" = "--clean" ]; then
    echo "Removing $BUILD ..."
    rm -rf "$BUILD"
    TARGET="${2:-all}"
fi

# -----------------------------------------------------------------------
step "1. Toolchain"
# -----------------------------------------------------------------------
if ! command -v arm-none-eabi-gcc >/dev/null 2>&1; then
    echo "arm-none-eabi-gcc not found - attempting install via apt..."
    if command -v apt >/dev/null 2>&1; then
        sudo apt update
        sudo apt install -y gcc-arm-none-eabi binutils-arm-none-eabi \
            libnewlib-arm-none-eabi libstdc++-arm-none-eabi-newlib \
            libstdc++-arm-none-eabi-dev git
    else
        fail "no apt available and arm-none-eabi-gcc is missing - install the ARM GNU Toolchain manually, then re-run this script"
        echo ""; echo "$PASS passed, $WARN warnings, $FAIL failed"; exit 1
    fi
fi
if command -v arm-none-eabi-gcc >/dev/null 2>&1; then
    pass "arm-none-eabi-gcc found: $(arm-none-eabi-gcc --version | head -1)"
else
    fail "arm-none-eabi-gcc still not found after install attempt"
    echo ""; echo "$PASS passed, $WARN warnings, $FAIL failed"; exit 1
fi
for tool in arm-none-eabi-objcopy arm-none-eabi-size arm-none-eabi-nm; do
    if command -v $tool >/dev/null 2>&1; then
        pass "$tool found"
    else
        fail "$tool not found - the gcc-arm-none-eabi package should provide this; check your install"
    fi
done
if command -v git >/dev/null 2>&1; then
    pass "git found: $(git --version)"
else
    fail "git not found - needed to fetch ST's own HAL/CMSIS sources."
    echo ""; echo "$PASS passed, $WARN warnings, $FAIL failed"; exit 1
fi

mkdir -p "$FIRMWARE_OUT"

build_bin_hex() {
    local elf="$1"
    arm-none-eabi-objcopy -O binary "$elf" "${elf%.elf}.bin"
    arm-none-eabi-objcopy -O ihex "$elf" "${elf%.elf}.hex"
}

# =========================================================================
if [ "$TARGET" = "all" ] || [ "$TARGET" = "g474" ]; then
# =========================================================================
step "2. Robot Controller Board (STM32G474RET6) - ST HAL/CMSIS sources"
# -----------------------------------------------------------------------
G4="$BUILD/g474"
mkdir -p "$G4/vendor" "$G4/common/HAL_Include" "$G4/common/CMSIS_Include" "$G4/hal_src" "$G4/hal_obj" "$G4/app" "$G4/boot_obj" "$G4/app_obj"

if [ ! -d "$G4/vendor/hal" ]; then
    echo "Fetching STM32G4xx HAL driver ($G4_HAL_TAG)..."
    git -c safe.directory='*' clone --depth 1 --branch "$G4_HAL_TAG" -q "$G4_HAL_REPO" "$G4/vendor/hal"
    pass "HAL driver fetched"
else
    pass "HAL driver already cached at build/g474/vendor/hal"
fi

if [ ! -d "$G4/vendor/cmsis_device_g4" ]; then
    echo "Fetching CMSIS device headers for G4 ($G4_CMSIS_DEVICE_TAG)..."
    git -c safe.directory='*' clone --depth 1 --branch "$G4_CMSIS_DEVICE_TAG" -q "$G4_CMSIS_DEVICE_REPO" "$G4/vendor/cmsis_device_g4"
    pass "CMSIS device (G4) fetched"
else
    pass "CMSIS device (G4) already cached at build/g474/vendor/cmsis_device_g4"
fi

if [ ! -d "$G4/vendor/cmsis_core" ]; then
    echo "Fetching generic ARM CMSIS Core headers (Include/ only, sparse)..."
    git -c safe.directory='*' clone --depth 1 --filter=blob:none --no-checkout -q --branch "$CMSIS_CORE_TAG" "$CMSIS_CORE_REPO" "$G4/vendor/cmsis_core"
    (cd "$G4/vendor/cmsis_core" && git -c safe.directory='*' sparse-checkout init --no-cone \
        && echo "/Core/Include/**" > .git/info/sparse-checkout \
        && git -c safe.directory='*' checkout -q)
    pass "CMSIS core fetched (Include/ only)"
else
    pass "CMSIS core already cached at build/g474/vendor/cmsis_core"
fi

cp "$G4/vendor/hal/Inc/"*.h "$G4/common/HAL_Include/" 2>/dev/null || true
cp -r "$G4/vendor/hal/Inc/Legacy" "$G4/common/HAL_Include/" 2>/dev/null || true
cp "$G4/common/HAL_Include/stm32g4xx_hal_conf_template.h" "$G4/common/HAL_Include/stm32g4xx_hal_conf.h"
cp "$G4/vendor/hal/Src/"*.c "$G4/hal_src/"
cp "$G4/vendor/cmsis_device_g4/Include/"*.h "$G4/common/CMSIS_Include/"
cp -r "$G4/vendor/cmsis_core/Core/Include/"* "$G4/common/CMSIS_Include/"
if [ -f "$G4/common/CMSIS_Include/core_cm4.h" ] && [ -f "$G4/common/HAL_Include/stm32g4xx_hal.h" ]; then
    pass "HAL/CMSIS include tree assembled"
else
    fail "HAL/CMSIS include tree incomplete - check build/g474/common/ manually"
fi

# -----------------------------------------------------------------------
step "3. Robot Controller Board - common compiler flags and shared HAL objects"
# -----------------------------------------------------------------------
CFLAGS_G4="-mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard -DSTM32G474xx -DUSE_HAL_DRIVER -I$G4/common/CMSIS_Include -I$G4/common/HAL_Include -O2 -Wall -ffunction-sections -fdata-sections"
LDCOMMON_G4="-mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard -specs=nano.specs -specs=nosys.specs -Wl,--gc-sections"

# Only the modules this skeleton's own GPIO-toggle smoke test needs today
# (HAL core + RCC + GPIO + Cortex + PWR + FLASH) - extend this list as real
# FDCAN/motion/timer code lands, same "only what's used" reasoning URTC's
# own build script documents.
HAL_MODULES_G4="stm32g4xx_hal stm32g4xx_hal_cortex stm32g4xx_hal_gpio stm32g4xx_hal_rcc stm32g4xx_hal_rcc_ex stm32g4xx_hal_pwr stm32g4xx_hal_pwr_ex stm32g4xx_hal_flash stm32g4xx_hal_flash_ex stm32g4xx_hal_exti"

HAL_OK=1
for f in $HAL_MODULES_G4; do
    if [ ! -f "$G4/hal_obj/$f.o" ] || [ "$G4/hal_src/$f.c" -nt "$G4/hal_obj/$f.o" ]; then
        arm-none-eabi-gcc $CFLAGS_G4 -x c -c "$G4/hal_src/$f.c" -o "$G4/hal_obj/$f.o" || HAL_OK=0
    fi
done
COUNT=$(ls "$G4/hal_obj"/*.o 2>/dev/null | wc -l)
if [ "$HAL_OK" = "1" ] && [ "$COUNT" -ge 1 ]; then
    pass "$COUNT HAL modules compiled"
else
    fail "one or more HAL modules failed to compile - see errors above"
    echo ""; echo "$PASS passed, $WARN warnings, $FAIL failed"; exit 1
fi

cp "$G4/vendor/cmsis_device_g4/Source/Templates/gcc/startup_stm32g474xx.s" "$G4/app/"
cp "$G4/vendor/cmsis_device_g4/Source/Templates/system_stm32g4xx.c" "$G4/app/"
arm-none-eabi-gcc $CFLAGS_G4 -x assembler-with-cpp -c "$G4/app/startup_stm32g474xx.s" -o "$G4/app/startup.o"
arm-none-eabi-gcc $CFLAGS_G4 -x c -c "$G4/app/system_stm32g4xx.c" -o "$G4/app/system_stm32g4xx.o"
pass "startup + system files compiled"

# -----------------------------------------------------------------------
step "4. Robot Controller Board bootloader (firmware/mcu_stm32g474/boot/)"
# -----------------------------------------------------------------------
SRC="$ROOT/firmware/mcu_stm32g474/boot"
rm -f "$G4/boot_obj"/*.o
arm-none-eabi-gcc $CFLAGS_G4 -I"$SRC" -x c -c "$SRC/bootloader_main.c" -o "$G4/boot_obj/bootloader_main.o"
arm-none-eabi-gcc $LDCOMMON_G4 -T"$SRC/STM32G474RETx_BOOTLOADER.ld" \
    "$G4/app/startup.o" "$G4/app/system_stm32g4xx.o" \
    "$G4/boot_obj"/*.o "$G4/hal_obj"/*.o -o "$G4/boot_obj/HYDRA_RCB_BOOTLOADER.elf" \
    2>&1 | grep -v "not implemented\|note: the message\|in function \`_" || true
build_bin_hex "$G4/boot_obj/HYDRA_RCB_BOOTLOADER.elf"
cp "$G4/boot_obj/HYDRA_RCB_BOOTLOADER."{elf,bin,hex} "$FIRMWARE_OUT/"
pass "HYDRA_RCB_BOOTLOADER.bin/.hex/.elf built ($(arm-none-eabi-size "$G4/boot_obj/HYDRA_RCB_BOOTLOADER.elf" | tail -1 | awk '{print $1}') bytes text)"

# -----------------------------------------------------------------------
step "5. Robot Controller Board application (firmware/mcu_stm32g474/)"
# -----------------------------------------------------------------------
SRC="$ROOT/firmware/mcu_stm32g474"
rm -f "$G4/app_obj"/*.o
arm-none-eabi-gcc $CFLAGS_G4 -I"$SRC" -x c -c "$SRC/STM32G474RE_main.c" -o "$G4/app_obj/STM32G474RE_main.o"
arm-none-eabi-gcc $LDCOMMON_G4 -T"$SRC/STM32G474RETx_APP.ld" \
    "$G4/app/startup.o" "$G4/app/system_stm32g4xx.o" \
    "$G4/app_obj"/*.o "$G4/hal_obj"/*.o -o "$G4/app_obj/HYDRA_RCB_APP.elf" \
    2>&1 | grep -v "not implemented\|note: the message\|in function \`_" || true
build_bin_hex "$G4/app_obj/HYDRA_RCB_APP.elf"
cp "$G4/app_obj/HYDRA_RCB_APP."{elf,bin,hex} "$FIRMWARE_OUT/"
pass "HYDRA_RCB_APP.bin/.hex/.elf built ($(arm-none-eabi-size "$G4/app_obj/HYDRA_RCB_APP.elf" | tail -1 | awk '{print $1}') bytes text) - GPIO-toggle smoke test only, see firmware/mcu_stm32g474/README.md"

fi

# =========================================================================
if [ "$TARGET" = "all" ] || [ "$TARGET" = "h745" ]; then
# =========================================================================
step "6. Kinematic Brain (STM32H745ZIT6) - ST HAL/CMSIS sources"
# -----------------------------------------------------------------------
H7="$BUILD/h745"
mkdir -p "$H7/vendor" "$H7/common/HAL_Include" "$H7/common/CMSIS_Include" "$H7/hal_src" "$H7/hal_obj" "$H7/cm7" "$H7/cm4" "$H7/cm7_boot_obj" "$H7/cm7_app_obj" "$H7/cm4_boot_obj" "$H7/cm4_app_obj"

if [ ! -d "$H7/vendor/hal" ]; then
    echo "Fetching STM32H7xx HAL driver ($H7_HAL_TAG)..."
    git -c safe.directory='*' clone --depth 1 --branch "$H7_HAL_TAG" -q "$H7_HAL_REPO" "$H7/vendor/hal"
    pass "HAL driver fetched"
else
    pass "HAL driver already cached at build/h745/vendor/hal"
fi

if [ ! -d "$H7/vendor/cmsis_device_h7" ]; then
    echo "Fetching CMSIS device headers for H7 ($H7_CMSIS_DEVICE_TAG)..."
    git -c safe.directory='*' clone --depth 1 --branch "$H7_CMSIS_DEVICE_TAG" -q "$H7_CMSIS_DEVICE_REPO" "$H7/vendor/cmsis_device_h7"
    pass "CMSIS device (H7) fetched"
else
    pass "CMSIS device (H7) already cached at build/h745/vendor/cmsis_device_h7"
fi

if [ ! -d "$H7/vendor/cmsis_core" ]; then
    echo "Fetching generic ARM CMSIS Core headers (Include/ only, sparse)..."
    git -c safe.directory='*' clone --depth 1 --filter=blob:none --no-checkout -q --branch "$CMSIS_CORE_TAG" "$CMSIS_CORE_REPO" "$H7/vendor/cmsis_core"
    git -C "$H7/vendor/cmsis_core" -c safe.directory='*' sparse-checkout init --no-cone
    echo "/Core/Include/**" > "$H7/vendor/cmsis_core/.git/info/sparse-checkout"
    git -C "$H7/vendor/cmsis_core" -c safe.directory='*' checkout -q
    pass "CMSIS core fetched (Include/ only)"
else
    pass "CMSIS core already cached at build/h745/vendor/cmsis_core"
fi

cp "$H7/vendor/hal/Inc/"*.h "$H7/common/HAL_Include/" 2>/dev/null || true
cp -r "$H7/vendor/hal/Inc/Legacy" "$H7/common/HAL_Include/" 2>/dev/null || true
cp "$H7/common/HAL_Include/stm32h7xx_hal_conf_template.h" "$H7/common/HAL_Include/stm32h7xx_hal_conf.h"
cp "$H7/vendor/hal/Src/"*.c "$H7/hal_src/"
cp "$H7/vendor/cmsis_device_h7/Include/"*.h "$H7/common/CMSIS_Include/"
cp -r "$H7/vendor/cmsis_core/Core/Include/"* "$H7/common/CMSIS_Include/"
if [ -f "$H7/common/CMSIS_Include/core_cm7.h" ] && [ -f "$H7/common/HAL_Include/stm32h7xx_hal.h" ]; then
    pass "HAL/CMSIS include tree assembled"
else
    fail "HAL/CMSIS include tree incomplete - check build/h745/common/ manually"
fi

# -----------------------------------------------------------------------
step "7. Kinematic Brain - common compiler flags and shared HAL objects (per core)"
# -----------------------------------------------------------------------
# Only what this skeleton's own GPIO-toggle smoke test needs today - extend
# as real FDCAN/SPI/timer/motion code lands, same reasoning as G474's own list.
HAL_MODULES_H7="stm32h7xx_hal stm32h7xx_hal_cortex stm32h7xx_hal_gpio stm32h7xx_hal_rcc stm32h7xx_hal_rcc_ex stm32h7xx_hal_pwr stm32h7xx_hal_pwr_ex stm32h7xx_hal_flash stm32h7xx_hal_flash_ex stm32h7xx_hal_exti stm32h7xx_hal_mdma"

for CORE in CM7 CM4; do
    CFLAGS_H7="-mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard -DSTM32H745xx -DCORE_CM4 -I$H7/common/CMSIS_Include -I$H7/common/HAL_Include -O2 -Wall -ffunction-sections -fdata-sections"
    if [ "$CORE" = "CM7" ]; then
        CFLAGS_H7="-mcpu=cortex-m7 -mthumb -mfpu=fpv5-d16 -mfloat-abi=hard -DSTM32H745xx -DCORE_CM7 -I$H7/common/CMSIS_Include -I$H7/common/HAL_Include -O2 -Wall -ffunction-sections -fdata-sections"
    fi
    LOWER=$(echo "$CORE" | tr 'A-Z' 'a-z')
    OBJDIR="$H7/hal_obj_$LOWER"
    mkdir -p "$OBJDIR"
    HAL_OK=1
    for f in $HAL_MODULES_H7; do
        if [ ! -f "$OBJDIR/$f.o" ] || [ "$H7/hal_src/$f.c" -nt "$OBJDIR/$f.o" ]; then
            arm-none-eabi-gcc $CFLAGS_H7 -x c -c "$H7/hal_src/$f.c" -o "$OBJDIR/$f.o" || HAL_OK=0
        fi
    done
    COUNT=$(ls "$OBJDIR"/*.o 2>/dev/null | wc -l)
    if [ "$HAL_OK" = "1" ] && [ "$COUNT" -ge 1 ]; then
        pass "$CORE: $COUNT HAL modules compiled"
    else
        fail "$CORE: one or more HAL modules failed to compile - see errors above"
        echo ""; echo "$PASS passed, $WARN warnings, $FAIL failed"; exit 1
    fi

    CORE_DIR="$H7/$LOWER"
    cp "$H7/vendor/cmsis_device_h7/Source/Templates/gcc/startup_stm32h745xx.s" "$CORE_DIR/"
    cp "$H7/vendor/cmsis_device_h7/Source/Templates/system_stm32h7xx_dualcore_boot_cm4_cm7.c" "$CORE_DIR/system_stm32h7xx.c"
    arm-none-eabi-gcc $CFLAGS_H7 -x assembler-with-cpp -c "$CORE_DIR/startup_stm32h745xx.s" -o "$CORE_DIR/startup.o"
    arm-none-eabi-gcc $CFLAGS_H7 -x c -c "$CORE_DIR/system_stm32h7xx.c" -o "$CORE_DIR/system_stm32h7xx.o"
    pass "$CORE: startup + system files compiled"
done

CFLAGS_CM7="-mcpu=cortex-m7 -mthumb -mfpu=fpv5-d16 -mfloat-abi=hard -DSTM32H745xx -DCORE_CM7 -I$H7/common/CMSIS_Include -I$H7/common/HAL_Include -O2 -Wall -ffunction-sections -fdata-sections"
LDCOMMON_CM7="-mcpu=cortex-m7 -mthumb -mfpu=fpv5-d16 -mfloat-abi=hard -specs=nano.specs -specs=nosys.specs -Wl,--gc-sections"
CFLAGS_CM4="-mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard -DSTM32H745xx -DCORE_CM4 -I$H7/common/CMSIS_Include -I$H7/common/HAL_Include -O2 -Wall -ffunction-sections -fdata-sections"
LDCOMMON_CM4="-mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard -specs=nano.specs -specs=nosys.specs -Wl,--gc-sections"

# -----------------------------------------------------------------------
step "8. Kinematic Brain CM7 bootloader + application (firmware/mcu_stm32h745/CM7/)"
# -----------------------------------------------------------------------
SRC="$ROOT/firmware/mcu_stm32h745/CM7"
arm-none-eabi-gcc $CFLAGS_CM7 -I"$SRC/boot" -x c -c "$SRC/boot/bootloader_main.c" -o "$H7/cm7_boot_obj/bootloader_main.o"
arm-none-eabi-gcc $LDCOMMON_CM7 -T"$SRC/boot/STM32H745ZITx_CM7_BOOTLOADER.ld" \
    "$H7/cm7/startup.o" "$H7/cm7/system_stm32h7xx.o" \
    "$H7/cm7_boot_obj"/*.o "$H7/hal_obj_cm7"/*.o -o "$H7/cm7_boot_obj/HYDRA_KB_CM7_BOOTLOADER.elf" \
    2>&1 | grep -v "not implemented\|note: the message\|in function \`_" || true
build_bin_hex "$H7/cm7_boot_obj/HYDRA_KB_CM7_BOOTLOADER.elf"
cp "$H7/cm7_boot_obj/HYDRA_KB_CM7_BOOTLOADER."{elf,bin,hex} "$FIRMWARE_OUT/"
pass "HYDRA_KB_CM7_BOOTLOADER.bin/.hex/.elf built"

arm-none-eabi-gcc $CFLAGS_CM7 -I"$SRC" -x c -c "$SRC/STM32H745ZI_CM7_main.c" -o "$H7/cm7_app_obj/STM32H745ZI_CM7_main.o"
arm-none-eabi-gcc $LDCOMMON_CM7 -T"$SRC/STM32H745ZITx_CM7_APP.ld" \
    "$H7/cm7/startup.o" "$H7/cm7/system_stm32h7xx.o" \
    "$H7/cm7_app_obj"/*.o "$H7/hal_obj_cm7"/*.o -o "$H7/cm7_app_obj/HYDRA_KB_CM7_APP.elf" \
    2>&1 | grep -v "not implemented\|note: the message\|in function \`_" || true
build_bin_hex "$H7/cm7_app_obj/HYDRA_KB_CM7_APP.elf"
cp "$H7/cm7_app_obj/HYDRA_KB_CM7_APP."{elf,bin,hex} "$FIRMWARE_OUT/"
pass "HYDRA_KB_CM7_APP.bin/.hex/.elf built ($(arm-none-eabi-size "$H7/cm7_app_obj/HYDRA_KB_CM7_APP.elf" | tail -1 | awk '{print $1}') bytes text) - GPIO-toggle smoke test only"

# -----------------------------------------------------------------------
step "9. Kinematic Brain CM4 bootloader + application (firmware/mcu_stm32h745/CM4/)"
# -----------------------------------------------------------------------
SRC="$ROOT/firmware/mcu_stm32h745/CM4"
arm-none-eabi-gcc $CFLAGS_CM4 -I"$SRC/boot" -x c -c "$SRC/boot/bootloader_main.c" -o "$H7/cm4_boot_obj/bootloader_main.o"
arm-none-eabi-gcc $LDCOMMON_CM4 -T"$SRC/boot/STM32H745ZITx_CM4_BOOTLOADER.ld" \
    "$H7/cm4/startup.o" "$H7/cm4/system_stm32h7xx.o" \
    "$H7/cm4_boot_obj"/*.o "$H7/hal_obj_cm4"/*.o -o "$H7/cm4_boot_obj/HYDRA_KB_CM4_BOOTLOADER.elf" \
    2>&1 | grep -v "not implemented\|note: the message\|in function \`_" || true
build_bin_hex "$H7/cm4_boot_obj/HYDRA_KB_CM4_BOOTLOADER.elf"
cp "$H7/cm4_boot_obj/HYDRA_KB_CM4_BOOTLOADER."{elf,bin,hex} "$FIRMWARE_OUT/"
pass "HYDRA_KB_CM4_BOOTLOADER.bin/.hex/.elf built"

arm-none-eabi-gcc $CFLAGS_CM4 -I"$SRC" -x c -c "$SRC/STM32H745ZI_CM4_main.c" -o "$H7/cm4_app_obj/STM32H745ZI_CM4_main.o"
arm-none-eabi-gcc $LDCOMMON_CM4 -T"$SRC/STM32H745ZITx_CM4_APP.ld" \
    "$H7/cm4/startup.o" "$H7/cm4/system_stm32h7xx.o" \
    "$H7/cm4_app_obj"/*.o "$H7/hal_obj_cm4"/*.o -o "$H7/cm4_app_obj/HYDRA_KB_CM4_APP.elf" \
    2>&1 | grep -v "not implemented\|note: the message\|in function \`_" || true
build_bin_hex "$H7/cm4_app_obj/HYDRA_KB_CM4_APP.elf"
cp "$H7/cm4_app_obj/HYDRA_KB_CM4_APP."{elf,bin,hex} "$FIRMWARE_OUT/"
pass "HYDRA_KB_CM4_APP.bin/.hex/.elf built ($(arm-none-eabi-size "$H7/cm4_app_obj/HYDRA_KB_CM4_APP.elf" | tail -1 | awk '{print $1}') bytes text) - GPIO-toggle smoke test only, no CM7<->CM4 sync yet"

fi

# -----------------------------------------------------------------------
step "Summary"
# -----------------------------------------------------------------------
echo "$PASS passed, $WARN warnings, $FAIL failed"
echo ""
echo "Output binaries are in: $FIRMWARE_OUT/"
echo ""
echo "Reminder: g474's app/bootloader above are GPIO-toggle smoke tests that"
echo "prove the toolchain/vendoring pipeline works end to end, NOT the real"
echo "CAN-OTA/motion firmware yet - see firmware/mcu_stm32g474/README.md and"
echo "HYDRA-UMC/docs/architecture.md for what's still real engineering work."

if [ "$FAIL" -gt 0 ]; then exit 1; fi
