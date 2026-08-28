@echo off
REM HYDRA_UMC_SCRIPT_STANDARD_HEADER_BEGIN
REM *****************************************************************************
REM Project   : HYDRA-UMC
REM Script    : build_firmware.bat
REM Purpose   : Incremental firmware build and versioned artifact packaging workflow.
REM Author    : JuanenRac (Electro Hobby 3D)
REM Email     : electrohobby3d@gmail.com
REM Copyright : (C) 2026 JuanenRac
REM License   : GPL-3.0 - see LICENSE
REM *****************************************************************************
REM HYDRA_UMC_SCRIPT_STANDARD_HEADER_END
REM HYDRA_UMC_SCRIPT_STANDARD_BANNER_BEGIN
echo.
echo *****************************************************************************
echo * HYDRA-UMC - build_firmware.bat
echo * Mode      : INCREMENTAL BUILD
echo * Author    : JuanenRac (Electro Hobby 3D)
echo * Email     : electrohobby3d@gmail.com
echo * Copyright : (C) 2026 JuanenRac
echo * License   : GPL-3.0 - see LICENSE
echo * ------------------------------------------------------------------------- *
echo * 1. Increment the project version and synchronise its manifest.
echo * 2. Run this project's declared build, verification and packaging commands.
echo * 3. Report the result and keep an interactive terminal open.
echo *****************************************************************************
echo.
REM HYDRA_UMC_SCRIPT_STANDARD_BANNER_END
setlocal enabledelayedexpansion
REM HYDRA_UMC_SCRIPT_STANDARD_VERSION_STEP
echo [1/3] Incrementing project version and synchronising its manifest...
REM HYDRA_UMC_SCRIPT_STANDARD_VERSION_CAPTURE_BEFORE
for /f "usebackq delims=" %%V in (`python -c "import json; print(json.load(open(r'%~dp0hydra-umc.project.json', encoding='utf-8'))['version'])"`) do set "HYDRA_UMC_VERSION_BEFORE=%%V"
REM The registry version is the G474 application version. Its component bump
REM occurs later, then --sync records that one authoritative bump.
echo.
set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"
set "BUILD=%ROOT%\build"
set "FIRMWARE_OUT=%ROOT%\firmware"
set "G4_HAL_REPO=https://github.com/STMicroelectronics/stm32g4xx_hal_driver.git"
set "G4_HAL_TAG=v1.2.7"
set "G4_CMSIS_DEVICE_REPO=https://github.com/STMicroelectronics/cmsis_device_g4.git"
set "G4_CMSIS_DEVICE_TAG=v1.2.6"
set "CMSIS_CORE_REPO=https://github.com/STMicroelectronics/cmsis_core.git"
set "CMSIS_CORE_TAG=v5.9.0_20250520"
set "H7_HAL_REPO=https://github.com/STMicroelectronics/stm32h7xx_hal_driver.git"
set "H7_HAL_TAG=v1.11.6"
set "H7_CMSIS_DEVICE_REPO=https://github.com/STMicroelectronics/cmsis_device_h7.git"
set "H7_CMSIS_DEVICE_TAG=v1.10.7"
set "FREERTOS_REPO=https://github.com/FreeRTOS/FreeRTOS-Kernel.git"
set "FREERTOS_TAG=V11.3.0"

set /a PASS=0
set /a WARN=0
set /a FAIL=0

set "TARGET=all"
if "%~1"=="--clean" (
    echo Removing %BUILD% ...
    rmdir /s /q "%BUILD%" 2>nul
    if "%~2" NEQ "" set "TARGET=%~2"
) else if "%~1" NEQ "" (
    set "TARGET=%~1"
)

REM -----------------------------------------------------------------------
echo.
echo === 1. Toolchain ===
REM -----------------------------------------------------------------------
where arm-none-eabi-gcc >nul 2>&1
if errorlevel 1 (
    echo arm-none-eabi-gcc not found on PATH.
    echo.
    echo Install the official Arm GNU Toolchain for Windows from:
    echo   https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads
    echo Pick the "arm-none-eabi" AArch32 bare-metal target, Windows installer.
    echo During install, check "Add path to environment variable" when offered.
    echo.
    echo If winget is available, you can also try:
    echo   winget install --id Arm.GnuArmEmbeddedToolchain
    echo.
    echo Re-run this script after installing.
    set /a FAIL+=1
    goto :summary
)
for /f "delims=" %%v in ('arm-none-eabi-gcc --version ^| findstr /r "^arm-none-eabi-gcc"') do echo   OK   arm-none-eabi-gcc found: %%v
set /a PASS+=1

for %%T in (arm-none-eabi-objcopy arm-none-eabi-size arm-none-eabi-nm) do (
    where %%T >nul 2>&1
    if errorlevel 1 (
        echo   FAIL %%T not found - the Arm GNU Toolchain install should provide this; check your install
        set /a FAIL+=1
    ) else (
        echo   OK   %%T found
        set /a PASS+=1
    )
)

where git >nul 2>&1
if errorlevel 1 (
    echo   FAIL git not found - needed to fetch ST's own HAL/CMSIS sources.
    echo        Install from https://git-scm.com/download/win and re-run.
    set /a FAIL+=1
    goto :summary
) else (
    for /f "delims=" %%v in ('git --version') do echo   OK   git found: %%v
    set /a PASS+=1
)

where python >nul 2>&1
if errorlevel 1 (
    echo   FAIL python not found - required to run bump_version.py ^(all 6 components are incremental, see this script's own version-bump steps below^) and generate_manifest.py.
    echo        Install from https://www.python.org/downloads/windows/ and re-run.
    set /a FAIL+=1
    goto :summary
) else (
    for /f "delims=" %%v in ('python --version') do echo   OK   python found: %%v
    set /a PASS+=1
)

if not exist "%FIRMWARE_OUT%" mkdir "%FIRMWARE_OUT%"

REM A firmware directory represents one build set. Remove only generated
REM HYDRA artifacts and its generated manifest; preserve any other files.
echo.
echo === Firmware output cleanup ===
del /q "%FIRMWARE_OUT%\HYDRA_*.bin" 2>nul
del /q "%FIRMWARE_OUT%\HYDRA_*.elf" 2>nul
del /q "%FIRMWARE_OUT%\HYDRA_*.hex" 2>nul
del /q "%FIRMWARE_OUT%\firmware_manifest.json" 2>nul
echo   OK   old generated firmware artifacts removed from firmware\

REM -----------------------------------------------------------------------
echo.
echo === 2. FreeRTOS kernel sources (shared by every application target - see docs\architecture.md) ===
REM -----------------------------------------------------------------------
set "FREERTOS_VENDOR=%BUILD%\vendor\freertos"
if not exist "%FREERTOS_VENDOR%" (
    echo Fetching FreeRTOS-Kernel ^(%FREERTOS_TAG%^)...
    git -c safe.directory=* clone --depth 1 --branch %FREERTOS_TAG% -q "%FREERTOS_REPO%" "%FREERTOS_VENDOR%"
    echo   OK   FreeRTOS-Kernel fetched
) else (
    echo   OK   FreeRTOS-Kernel already cached at build\vendor\freertos
)
set /a PASS+=1
set "FREERTOS_SRC=%FREERTOS_VENDOR%"
set "FREERTOS_COMMON_SOURCES=tasks.c queue.c list.c timers.c event_groups.c"

if "%TARGET%"=="all" set "DO_G474=1" & set "DO_H745=1"
if "%TARGET%"=="g474" set "DO_G474=1"
if "%TARGET%"=="h745" set "DO_H745=1"

REM =========================================================================
if not defined DO_G474 goto :SKIP_G474
REM =========================================================================
echo.
echo === 3. Robot Controller Board ^(STM32G474RET6^) - ST HAL/CMSIS sources ===
REM -----------------------------------------------------------------------
set "G4=%BUILD%\g474"
for %%D in ("!G4!\vendor" "!G4!\common\HAL_Include" "!G4!\common\CMSIS_Include" "!G4!\hal_src" "!G4!\hal_obj" "!G4!\app" "!G4!\boot_obj" "!G4!\app_obj" "!G4!\freertos_obj") do if not exist "%%~D" mkdir "%%~D"

if not exist "!G4!\vendor\hal" (
    echo Fetching STM32G4xx HAL driver ^(%G4_HAL_TAG%^)...
    git -c safe.directory=* clone --depth 1 --branch %G4_HAL_TAG% -q "%G4_HAL_REPO%" "!G4!\vendor\hal"
    echo   OK   HAL driver fetched
) else (
    echo   OK   HAL driver already cached at build\g474\vendor\hal
)
set /a PASS+=1

if not exist "!G4!\vendor\cmsis_device_g4" (
    echo Fetching CMSIS device headers for G4 ^(%G4_CMSIS_DEVICE_TAG%^)...
    git -c safe.directory=* clone --depth 1 --branch %G4_CMSIS_DEVICE_TAG% -q "%G4_CMSIS_DEVICE_REPO%" "!G4!\vendor\cmsis_device_g4"
    echo   OK   CMSIS device ^(G4^) fetched
) else (
    echo   OK   CMSIS device ^(G4^) already cached at build\g474\vendor\cmsis_device_g4
)
set /a PASS+=1

if not exist "!G4!\vendor\cmsis_core" (
    echo Fetching generic ARM CMSIS Core headers ^(Include\ only, sparse^)...
    git -c safe.directory=* clone --depth 1 --filter=blob:none --no-checkout -q --branch %CMSIS_CORE_TAG% "%CMSIS_CORE_REPO%" "!G4!\vendor\cmsis_core"
    pushd "!G4!\vendor\cmsis_core"
    git -c safe.directory=* sparse-checkout init --no-cone
    echo /Core/Include/** > .git\info\sparse-checkout
    git -c safe.directory=* checkout -q
    popd
    echo   OK   CMSIS core fetched ^(Include\ only^)
) else (
    echo   OK   CMSIS core already cached at build\g474\vendor\cmsis_core
)
set /a PASS+=1

copy /y "!G4!\vendor\hal\Inc\*.h" "!G4!\common\HAL_Include\" >nul
xcopy /y /i /q /e "!G4!\vendor\hal\Inc\Legacy" "!G4!\common\HAL_Include\Legacy\" >nul
copy /y "!G4!\common\HAL_Include\stm32g4xx_hal_conf_template.h" "!G4!\common\HAL_Include\stm32g4xx_hal_conf.h" >nul
copy /y "!G4!\vendor\hal\Src\*.c" "!G4!\hal_src\" >nul
copy /y "!G4!\vendor\cmsis_device_g4\Include\*.h" "!G4!\common\CMSIS_Include\" >nul
xcopy /y /i /q /e "!G4!\vendor\cmsis_core\Core\Include\*" "!G4!\common\CMSIS_Include\" >nul
if exist "!G4!\common\CMSIS_Include\core_cm4.h" if exist "!G4!\common\HAL_Include\stm32g4xx_hal.h" (
    echo   OK   HAL/CMSIS include tree assembled
    set /a PASS+=1
) else (
    echo   FAIL HAL/CMSIS include tree incomplete - check build\g474\common\ manually
    set /a FAIL+=1
)

REM -----------------------------------------------------------------------
echo.
echo === 4. Robot Controller Board - common compiler flags and shared HAL objects ===
REM -----------------------------------------------------------------------
set "CFLAGS_G4=-mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard -DSTM32G474xx -DUSE_HAL_DRIVER -I!G4!\common\CMSIS_Include -I!G4!\common\HAL_Include -O2 -Wall -ffunction-sections -fdata-sections"
set "LDCOMMON_G4=-mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard -specs=nano.specs -specs=nosys.specs -Wl,--gc-sections"
set "CFLAGS_G4_APP=%CFLAGS_G4% -I%ROOT%\src\mcu_stm32g474 -I%FREERTOS_SRC%\include -I%FREERTOS_SRC%\portable\GCC\ARM_CM4F"

REM HAL core + RCC + GPIO + Cortex + PWR + FLASH (app smoke test) plus FDCAN +
REM IWDG (the real CAN-OTA bootloader, src\mcu_stm32g474\boot\) - extend
REM further as real motion/timer application code lands.
set "HAL_MODULES_G4=stm32g4xx_hal stm32g4xx_hal_cortex stm32g4xx_hal_gpio stm32g4xx_hal_rcc stm32g4xx_hal_rcc_ex stm32g4xx_hal_pwr stm32g4xx_hal_pwr_ex stm32g4xx_hal_flash stm32g4xx_hal_flash_ex stm32g4xx_hal_exti stm32g4xx_hal_fdcan stm32g4xx_hal_iwdg"

set /a HAL_COUNT=0
for %%f in (%HAL_MODULES_G4%) do (
    arm-none-eabi-gcc %CFLAGS_G4% -x c -c "!G4!\hal_src\%%f.c" -o "!G4!\hal_obj\%%f.o"
    if errorlevel 1 (
        echo   FAIL %%f failed to compile
    ) else (
        set /a HAL_COUNT+=1
    )
)
if !HAL_COUNT! EQU 12 (
    echo   OK   12/12 HAL modules compiled
    set /a PASS+=1
) else (
    echo   FAIL only !HAL_COUNT!/12 HAL modules compiled - see errors above
    set /a FAIL+=1
    goto :summary
)

copy /y "!G4!\vendor\cmsis_device_g4\Source\Templates\gcc\startup_stm32g474xx.s" "!G4!\app\" >nul
copy /y "!G4!\vendor\cmsis_device_g4\Source\Templates\system_stm32g4xx.c" "!G4!\app\" >nul
arm-none-eabi-gcc %CFLAGS_G4% -x assembler-with-cpp -c "!G4!\app\startup_stm32g474xx.s" -o "!G4!\app\startup.o"
arm-none-eabi-gcc %CFLAGS_G4% -x c -c "!G4!\app\system_stm32g4xx.c" -o "!G4!\app\system_stm32g4xx.o"
echo   OK   startup + system files compiled
set /a PASS+=1

REM FreeRTOS kernel (ARM_CM4F port) for the Robot Controller Board
set "PORTDIR=%FREERTOS_SRC%\portable\GCC\ARM_CM4F"
set "SECTION_FAIL=0"
for %%f in (%FREERTOS_COMMON_SOURCES%) do (
    arm-none-eabi-gcc %CFLAGS_G4_APP% -I"%FREERTOS_SRC%\include" -I"!PORTDIR!" -x c -c "%FREERTOS_SRC%\%%f" -o "!G4!\freertos_obj\%%f.o"
    if errorlevel 1 set "SECTION_FAIL=1"
)
arm-none-eabi-gcc %CFLAGS_G4_APP% -I"%FREERTOS_SRC%\include" -I"!PORTDIR!" -x c -c "!PORTDIR!\port.c" -o "!G4!\freertos_obj\port.o"
if errorlevel 1 set "SECTION_FAIL=1"
arm-none-eabi-gcc %CFLAGS_G4_APP% -I"%FREERTOS_SRC%\include" -I"!PORTDIR!" -x c -c "%FREERTOS_SRC%\portable\MemMang\heap_4.c" -o "!G4!\freertos_obj\heap_4.o"
if errorlevel 1 set "SECTION_FAIL=1"
if "!SECTION_FAIL!"=="1" (
    echo   FAIL FreeRTOS kernel failed to compile for Robot Controller Board - see errors above
    set /a FAIL+=1
    goto :summary
) else (
    echo   OK   FreeRTOS kernel ^(ARM_CM4F port^) compiled for Robot Controller Board
    set /a PASS+=1
)

REM -----------------------------------------------------------------------
echo.
echo === 5. Robot Controller Board bootloader ^(src\mcu_stm32g474\boot\^) - bare-metal CAN-OTA, no FreeRTOS ===
REM -----------------------------------------------------------------------
set "SRC=%ROOT%\src\mcu_stm32g474\boot"
call :BumpVersion "!SRC!\bootloader_common.h" BOOTLOADER_VERSION "Robot Controller Board bootloader"
set "OUT=!G4!\boot_obj"
del /q "!OUT!\*.o" 2>nul
set "SECTION_FAIL=0"
for %%f in ("!SRC!\*.c") do (
    arm-none-eabi-gcc %CFLAGS_G4% -I"!SRC!" -x c -c "%%f" -o "!OUT!\%%~nf.o"
    if errorlevel 1 (
        echo   FAIL %%~nf.c failed to compile
        set "SECTION_FAIL=1"
    )
)
if "!SECTION_FAIL!"=="1" (
    echo   FAIL Robot Controller Board bootloader failed to compile - see errors above
    set /a FAIL+=1
    goto :summary
) else (
    echo   OK   bootloader_*.c compiled
    set /a PASS+=1
)
set "G4_BOOT_MAJOR=?" & set "G4_BOOT_MINOR=?" & set "G4_BOOT_PATCH=?"
for /f "tokens=3" %%v in ('findstr /c:"#define BOOTLOADER_VERSION_MAJOR" "!SRC!\bootloader_common.h"') do set "G4_BOOT_MAJOR=%%v"
for /f "tokens=3" %%v in ('findstr /c:"#define BOOTLOADER_VERSION_MINOR" "!SRC!\bootloader_common.h"') do set "G4_BOOT_MINOR=%%v"
for /f "tokens=3" %%v in ('findstr /c:"#define BOOTLOADER_VERSION_PATCH" "!SRC!\bootloader_common.h"') do set "G4_BOOT_PATCH=%%v"
set "G4_BOOT_NAME=HYDRA_RCB_BOOTLOADER_v!G4_BOOT_MAJOR!.!G4_BOOT_MINOR!.!G4_BOOT_PATCH!"
arm-none-eabi-gcc %LDCOMMON_G4% -T"!SRC!\STM32G474RETx_BOOTLOADER.ld" "!G4!\app\startup.o" "!G4!\app\system_stm32g4xx.o" "!OUT!\*.o" "!G4!\hal_obj\*.o" -o "!OUT!\!G4_BOOT_NAME!.elf"
if errorlevel 1 (
    echo   FAIL Robot Controller Board bootloader: link failed
    set /a FAIL+=1
    goto :summary
)
arm-none-eabi-objcopy -O binary "!OUT!\!G4_BOOT_NAME!.elf" "!OUT!\!G4_BOOT_NAME!.bin"
arm-none-eabi-objcopy -O ihex "!OUT!\!G4_BOOT_NAME!.elf" "!OUT!\!G4_BOOT_NAME!.hex"
copy /y "!OUT!\!G4_BOOT_NAME!.elf" "%FIRMWARE_OUT%\" >nul
copy /y "!OUT!\!G4_BOOT_NAME!.bin" "%FIRMWARE_OUT%\" >nul
copy /y "!OUT!\!G4_BOOT_NAME!.hex" "%FIRMWARE_OUT%\" >nul
echo   OK   !G4_BOOT_NAME!.bin/.hex/.elf built
set /a PASS+=1

REM -----------------------------------------------------------------------
echo.
echo === 6. Robot Controller Board application ^(src\mcu_stm32g474\^) - FreeRTOS ===
REM -----------------------------------------------------------------------
set "SRC=%ROOT%\src\mcu_stm32g474"
call :BumpVersion "!SRC!\boot\bootloader_common.h" FIRMWARE_VERSION "Robot Controller Board application"
python "%~dp0bump_manifest_version.py" --sync
if errorlevel 1 ( echo VERSION SYNCHRONISATION FAILED. & pause & exit /b 1 )
REM HYDRA_UMC_SCRIPT_STANDARD_VERSION_CAPTURE_AFTER
for /f "usebackq delims=" %%V in (`python -c "import json; print(json.load(open(r'%~dp0hydra-umc.project.json', encoding='utf-8'))['version'])"`) do set "HYDRA_UMC_VERSION_AFTER=%%V"
if not defined HYDRA_UMC_VERSION_BEFORE set "HYDRA_UMC_VERSION_BEFORE=unknown"
if not defined HYDRA_UMC_VERSION_AFTER set "HYDRA_UMC_VERSION_AFTER=unknown"
echo.
echo *****************************************************************************
echo * VERSION INCREMENT COMPLETED
echo * v%HYDRA_UMC_VERSION_BEFORE% ^> v%HYDRA_UMC_VERSION_AFTER%
echo * Project manifest synchronized with the G474 application version.
echo *****************************************************************************
echo.
set "OUT=!G4!\app_obj"
del /q "!OUT!\*.o" 2>nul
arm-none-eabi-gcc %CFLAGS_G4_APP% -I"!SRC!" -x c -c "!SRC!\STM32G474RE_main.c" -o "!OUT!\STM32G474RE_main.o"
if errorlevel 1 (
    echo   FAIL STM32G474RE_main.c failed to compile
    set /a FAIL+=1
    goto :summary
)
set "G4_APP_MAJOR=?" & set "G4_APP_MINOR=?" & set "G4_APP_PATCH=?"
for /f "tokens=3" %%v in ('findstr /c:"#define FIRMWARE_VERSION_MAJOR" "!SRC!\boot\bootloader_common.h"') do set "G4_APP_MAJOR=%%v"
for /f "tokens=3" %%v in ('findstr /c:"#define FIRMWARE_VERSION_MINOR" "!SRC!\boot\bootloader_common.h"') do set "G4_APP_MINOR=%%v"
for /f "tokens=3" %%v in ('findstr /c:"#define FIRMWARE_VERSION_PATCH" "!SRC!\boot\bootloader_common.h"') do set "G4_APP_PATCH=%%v"
set "G4_APP_NAME=HYDRA_RCB_APP_v!G4_APP_MAJOR!.!G4_APP_MINOR!.!G4_APP_PATCH!"
arm-none-eabi-gcc %LDCOMMON_G4% -T"!SRC!\STM32G474RETx_APP.ld" "!G4!\app\startup.o" "!G4!\app\system_stm32g4xx.o" "!OUT!\*.o" "!G4!\hal_obj\*.o" "!G4!\freertos_obj\*.o" -o "!OUT!\!G4_APP_NAME!.elf"
if errorlevel 1 (
    echo   FAIL Robot Controller Board application: link failed
    set /a FAIL+=1
    goto :summary
)
arm-none-eabi-objcopy -O binary "!OUT!\!G4_APP_NAME!.elf" "!OUT!\!G4_APP_NAME!.bin"
arm-none-eabi-objcopy -O ihex "!OUT!\!G4_APP_NAME!.elf" "!OUT!\!G4_APP_NAME!.hex"
copy /y "!OUT!\!G4_APP_NAME!.elf" "%FIRMWARE_OUT%\" >nul
copy /y "!OUT!\!G4_APP_NAME!.bin" "%FIRMWARE_OUT%\" >nul
copy /y "!OUT!\!G4_APP_NAME!.hex" "%FIRMWARE_OUT%\" >nul
echo   OK   !G4_APP_NAME!.bin/.hex/.elf built - FreeRTOS GPIO-toggle smoke test, see src\mcu_stm32g474\README.md
set /a PASS+=1
:SKIP_G474

REM =========================================================================
if not defined DO_H745 goto :SKIP_H745
REM =========================================================================
echo.
echo === 7. Kinematic Brain ^(STM32H745ZIT6^) - ST HAL/CMSIS sources ===
REM -----------------------------------------------------------------------
set "H7=%BUILD%\h745"
for %%D in ("!H7!\vendor" "!H7!\common\HAL_Include" "!H7!\common\CMSIS_Include" "!H7!\hal_src" "!H7!\hal_obj_cm7" "!H7!\hal_obj_cm4" "!H7!\cm7" "!H7!\cm4" "!H7!\cm7_boot_obj" "!H7!\cm7_app_obj" "!H7!\cm4_boot_obj" "!H7!\cm4_app_obj" "!H7!\cm7_freertos_obj" "!H7!\cm4_freertos_obj") do if not exist "%%~D" mkdir "%%~D"

if not exist "!H7!\vendor\hal" (
    echo Fetching STM32H7xx HAL driver ^(%H7_HAL_TAG%^)...
    git -c safe.directory=* clone --depth 1 --branch %H7_HAL_TAG% -q "%H7_HAL_REPO%" "!H7!\vendor\hal"
    echo   OK   HAL driver fetched
) else (
    echo   OK   HAL driver already cached at build\h745\vendor\hal
)
set /a PASS+=1

if not exist "!H7!\vendor\cmsis_device_h7" (
    echo Fetching CMSIS device headers for H7 ^(%H7_CMSIS_DEVICE_TAG%^)...
    git -c safe.directory=* clone --depth 1 --branch %H7_CMSIS_DEVICE_TAG% -q "%H7_CMSIS_DEVICE_REPO%" "!H7!\vendor\cmsis_device_h7"
    echo   OK   CMSIS device ^(H7^) fetched
) else (
    echo   OK   CMSIS device ^(H7^) already cached at build\h745\vendor\cmsis_device_h7
)
set /a PASS+=1

if not exist "!H7!\vendor\cmsis_core" (
    echo Fetching generic ARM CMSIS Core headers ^(Include\ only, sparse^)...
    git -c safe.directory=* clone --depth 1 --filter=blob:none --no-checkout -q --branch %CMSIS_CORE_TAG% "%CMSIS_CORE_REPO%" "!H7!\vendor\cmsis_core"
    pushd "!H7!\vendor\cmsis_core"
    git -c safe.directory=* sparse-checkout init --no-cone
    echo /Core/Include/** > .git\info\sparse-checkout
    git -c safe.directory=* checkout -q
    popd
    echo   OK   CMSIS core fetched ^(Include\ only^)
) else (
    echo   OK   CMSIS core already cached at build\h745\vendor\cmsis_core
)
set /a PASS+=1

copy /y "!H7!\vendor\hal\Inc\*.h" "!H7!\common\HAL_Include\" >nul
xcopy /y /i /q /e "!H7!\vendor\hal\Inc\Legacy" "!H7!\common\HAL_Include\Legacy\" >nul
copy /y "!H7!\common\HAL_Include\stm32h7xx_hal_conf_template.h" "!H7!\common\HAL_Include\stm32h7xx_hal_conf.h" >nul
copy /y "!H7!\vendor\hal\Src\*.c" "!H7!\hal_src\" >nul
copy /y "!H7!\vendor\cmsis_device_h7\Include\*.h" "!H7!\common\CMSIS_Include\" >nul
xcopy /y /i /q /e "!H7!\vendor\cmsis_core\Core\Include\*" "!H7!\common\CMSIS_Include\" >nul
if exist "!H7!\common\CMSIS_Include\core_cm7.h" if exist "!H7!\common\HAL_Include\stm32h7xx_hal.h" (
    echo   OK   HAL/CMSIS include tree assembled
    set /a PASS+=1
) else (
    echo   FAIL HAL/CMSIS include tree incomplete - check build\h745\common\ manually
    set /a FAIL+=1
)

REM -----------------------------------------------------------------------
echo.
echo === 8. Kinematic Brain - common compiler flags and shared HAL objects ^(per core^) ===
REM -----------------------------------------------------------------------
REM App smoke-test core modules plus FDCAN + IWDG + SPI + HSEM - the real
REM CAN-OTA bootloaders (src\mcu_stm32h745\CM7\boot\, CM4\boot\): CM7's talks
REM FDCAN/IWDG/HSEM only (no bus of its own); CM4's is the gateway and needs
REM all four (SPI1 to the CM5, FDCAN1 to STACK A, IWDG, HSEM for the
REM CM7<->CM4 mailbox). Compiled for both cores regardless (same shared
REM module list) - unused object files for one core cost nothing, and
REM keeping one list avoids the two cores' object sets silently drifting apart.
set "HAL_MODULES_H7=stm32h7xx_hal stm32h7xx_hal_cortex stm32h7xx_hal_gpio stm32h7xx_hal_rcc stm32h7xx_hal_rcc_ex stm32h7xx_hal_pwr stm32h7xx_hal_pwr_ex stm32h7xx_hal_flash stm32h7xx_hal_flash_ex stm32h7xx_hal_exti stm32h7xx_hal_mdma stm32h7xx_hal_fdcan stm32h7xx_hal_iwdg stm32h7xx_hal_spi stm32h7xx_hal_hsem"

set "CFLAGS_CM7=-mcpu=cortex-m7 -mthumb -mfpu=fpv5-d16 -mfloat-abi=hard -DSTM32H745xx -DCORE_CM7 -I!H7!\common\CMSIS_Include -I!H7!\common\HAL_Include -O2 -Wall -ffunction-sections -fdata-sections"
set "CFLAGS_CM4=-mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard -DSTM32H745xx -DCORE_CM4 -I!H7!\common\CMSIS_Include -I!H7!\common\HAL_Include -O2 -Wall -ffunction-sections -fdata-sections"
set "LDCOMMON_CM7=-mcpu=cortex-m7 -mthumb -mfpu=fpv5-d16 -mfloat-abi=hard -specs=nano.specs -specs=nosys.specs -Wl,--gc-sections"
set "LDCOMMON_CM4=-mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard -specs=nano.specs -specs=nosys.specs -Wl,--gc-sections"
set "CFLAGS_CM7_APP=%CFLAGS_CM7% -I%ROOT%\src\mcu_stm32h745\CM7 -I%FREERTOS_SRC%\include -I%FREERTOS_SRC%\portable\GCC\ARM_CM7\r0p1"
set "CFLAGS_CM4_APP=%CFLAGS_CM4% -I%ROOT%\src\mcu_stm32h745\CM4 -I%FREERTOS_SRC%\include -I%FREERTOS_SRC%\portable\GCC\ARM_CM4F"
set "COMMON_INC=-I%ROOT%\src\mcu_stm32h745\Common"

REM ---- CM7 HAL objects + startup/system ----
set /a HAL_COUNT=0
for %%f in (%HAL_MODULES_H7%) do (
    arm-none-eabi-gcc %CFLAGS_CM7% -x c -c "!H7!\hal_src\%%f.c" -o "!H7!\hal_obj_cm7\%%f.o"
    if errorlevel 1 (echo   FAIL CM7: %%f failed to compile) else (set /a HAL_COUNT+=1)
)
if !HAL_COUNT! EQU 15 (
    echo   OK   CM7: 15/15 HAL modules compiled
    set /a PASS+=1
) else (
    echo   FAIL CM7: only !HAL_COUNT!/15 HAL modules compiled - see errors above
    set /a FAIL+=1
    goto :summary
)
copy /y "!H7!\vendor\cmsis_device_h7\Source\Templates\gcc\startup_stm32h745xx.s" "!H7!\cm7\" >nul
copy /y "!H7!\vendor\cmsis_device_h7\Source\Templates\system_stm32h7xx_dualcore_boot_cm4_cm7.c" "!H7!\cm7\system_stm32h7xx.c" >nul
arm-none-eabi-gcc %CFLAGS_CM7% -x assembler-with-cpp -c "!H7!\cm7\startup_stm32h745xx.s" -o "!H7!\cm7\startup.o"
arm-none-eabi-gcc %CFLAGS_CM7% -x c -c "!H7!\cm7\system_stm32h7xx.c" -o "!H7!\cm7\system_stm32h7xx.o"
echo   OK   CM7: startup + system files compiled
set /a PASS+=1

REM ---- CM4 HAL objects + startup/system ----
set /a HAL_COUNT=0
for %%f in (%HAL_MODULES_H7%) do (
    arm-none-eabi-gcc %CFLAGS_CM4% -x c -c "!H7!\hal_src\%%f.c" -o "!H7!\hal_obj_cm4\%%f.o"
    if errorlevel 1 (echo   FAIL CM4: %%f failed to compile) else (set /a HAL_COUNT+=1)
)
if !HAL_COUNT! EQU 15 (
    echo   OK   CM4: 15/15 HAL modules compiled
    set /a PASS+=1
) else (
    echo   FAIL CM4: only !HAL_COUNT!/15 HAL modules compiled - see errors above
    set /a FAIL+=1
    goto :summary
)
copy /y "!H7!\vendor\cmsis_device_h7\Source\Templates\gcc\startup_stm32h745xx.s" "!H7!\cm4\" >nul
copy /y "!H7!\vendor\cmsis_device_h7\Source\Templates\system_stm32h7xx_dualcore_boot_cm4_cm7.c" "!H7!\cm4\system_stm32h7xx.c" >nul
arm-none-eabi-gcc %CFLAGS_CM4% -x assembler-with-cpp -c "!H7!\cm4\startup_stm32h745xx.s" -o "!H7!\cm4\startup.o"
arm-none-eabi-gcc %CFLAGS_CM4% -x c -c "!H7!\cm4\system_stm32h7xx.c" -o "!H7!\cm4\system_stm32h7xx.o"
echo   OK   CM4: startup + system files compiled
set /a PASS+=1

REM ---- FreeRTOS kernel, CM7 (ARM_CM7/r0p1 port) ----
set "PORTDIR=%FREERTOS_SRC%\portable\GCC\ARM_CM7\r0p1"
set "SECTION_FAIL=0"
for %%f in (%FREERTOS_COMMON_SOURCES%) do (
    arm-none-eabi-gcc %CFLAGS_CM7_APP% -I"%FREERTOS_SRC%\include" -I"!PORTDIR!" -x c -c "%FREERTOS_SRC%\%%f" -o "!H7!\cm7_freertos_obj\%%f.o"
    if errorlevel 1 set "SECTION_FAIL=1"
)
arm-none-eabi-gcc %CFLAGS_CM7_APP% -I"%FREERTOS_SRC%\include" -I"!PORTDIR!" -x c -c "!PORTDIR!\port.c" -o "!H7!\cm7_freertos_obj\port.o"
if errorlevel 1 set "SECTION_FAIL=1"
arm-none-eabi-gcc %CFLAGS_CM7_APP% -I"%FREERTOS_SRC%\include" -I"!PORTDIR!" -x c -c "%FREERTOS_SRC%\portable\MemMang\heap_4.c" -o "!H7!\cm7_freertos_obj\heap_4.o"
if errorlevel 1 set "SECTION_FAIL=1"
if "!SECTION_FAIL!"=="1" (
    echo   FAIL FreeRTOS kernel failed to compile for CM7 - see errors above
    set /a FAIL+=1
    goto :summary
) else (
    echo   OK   FreeRTOS kernel ^(ARM_CM7/r0p1 port^) compiled for CM7
    set /a PASS+=1
)

REM ---- FreeRTOS kernel, CM4 (ARM_CM4F port) ----
set "PORTDIR=%FREERTOS_SRC%\portable\GCC\ARM_CM4F"
set "SECTION_FAIL=0"
for %%f in (%FREERTOS_COMMON_SOURCES%) do (
    arm-none-eabi-gcc %CFLAGS_CM4_APP% -I"%FREERTOS_SRC%\include" -I"!PORTDIR!" -x c -c "%FREERTOS_SRC%\%%f" -o "!H7!\cm4_freertos_obj\%%f.o"
    if errorlevel 1 set "SECTION_FAIL=1"
)
arm-none-eabi-gcc %CFLAGS_CM4_APP% -I"%FREERTOS_SRC%\include" -I"!PORTDIR!" -x c -c "!PORTDIR!\port.c" -o "!H7!\cm4_freertos_obj\port.o"
if errorlevel 1 set "SECTION_FAIL=1"
arm-none-eabi-gcc %CFLAGS_CM4_APP% -I"%FREERTOS_SRC%\include" -I"!PORTDIR!" -x c -c "%FREERTOS_SRC%\portable\MemMang\heap_4.c" -o "!H7!\cm4_freertos_obj\heap_4.o"
if errorlevel 1 set "SECTION_FAIL=1"
if "!SECTION_FAIL!"=="1" (
    echo   FAIL FreeRTOS kernel failed to compile for CM4 - see errors above
    set /a FAIL+=1
    goto :summary
) else (
    echo   OK   FreeRTOS kernel ^(ARM_CM4F port^) compiled for CM4
    set /a PASS+=1
)

REM -----------------------------------------------------------------------
echo.
echo === 9. Kinematic Brain CM7 bootloader ^(bare-metal CAN-OTA, mailbox-relayed^) + application ^(FreeRTOS^) - src\mcu_stm32h745\CM7\ ===
REM -----------------------------------------------------------------------
set "SRC=%ROOT%\src\mcu_stm32h745\CM7"
call :BumpVersion "!SRC!\boot\bootloader_common.h" BOOTLOADER_VERSION "Kinematic Brain CM7 bootloader"
set "OUT=!H7!\cm7_boot_obj"
del /q "!OUT!\*.o" 2>nul
set "SECTION_FAIL=0"
for %%f in ("!SRC!\boot\*.c") do (
    arm-none-eabi-gcc %CFLAGS_CM7% -I"!SRC!\boot" %COMMON_INC% -x c -c "%%f" -o "!OUT!\%%~nf.o"
    if errorlevel 1 (echo   FAIL %%~nf.c failed to compile & set "SECTION_FAIL=1")
)
if "!SECTION_FAIL!"=="1" (
    echo   FAIL CM7 bootloader failed to compile - see errors above
    set /a FAIL+=1
    goto :summary
) else (
    echo   OK   CM7 bootloader_*.c compiled
    set /a PASS+=1
)
set "CM7_BOOT_MAJOR=?" & set "CM7_BOOT_MINOR=?" & set "CM7_BOOT_PATCH=?"
for /f "tokens=3" %%v in ('findstr /c:"#define BOOTLOADER_VERSION_MAJOR" "!SRC!\boot\bootloader_common.h"') do set "CM7_BOOT_MAJOR=%%v"
for /f "tokens=3" %%v in ('findstr /c:"#define BOOTLOADER_VERSION_MINOR" "!SRC!\boot\bootloader_common.h"') do set "CM7_BOOT_MINOR=%%v"
for /f "tokens=3" %%v in ('findstr /c:"#define BOOTLOADER_VERSION_PATCH" "!SRC!\boot\bootloader_common.h"') do set "CM7_BOOT_PATCH=%%v"
set "CM7_BOOT_NAME=HYDRA_KB_CM7_BOOTLOADER_v!CM7_BOOT_MAJOR!.!CM7_BOOT_MINOR!.!CM7_BOOT_PATCH!"
arm-none-eabi-gcc %LDCOMMON_CM7% -T"!SRC!\boot\STM32H745ZITx_CM7_BOOTLOADER.ld" "!H7!\cm7\startup.o" "!H7!\cm7\system_stm32h7xx.o" "!OUT!\*.o" "!H7!\hal_obj_cm7\*.o" -o "!OUT!\!CM7_BOOT_NAME!.elf"
if errorlevel 1 (
    echo   FAIL CM7 bootloader: link failed
    set /a FAIL+=1
    goto :summary
)
arm-none-eabi-objcopy -O binary "!OUT!\!CM7_BOOT_NAME!.elf" "!OUT!\!CM7_BOOT_NAME!.bin"
arm-none-eabi-objcopy -O ihex "!OUT!\!CM7_BOOT_NAME!.elf" "!OUT!\!CM7_BOOT_NAME!.hex"
copy /y "!OUT!\!CM7_BOOT_NAME!.elf" "%FIRMWARE_OUT%\" >nul
copy /y "!OUT!\!CM7_BOOT_NAME!.bin" "%FIRMWARE_OUT%\" >nul
copy /y "!OUT!\!CM7_BOOT_NAME!.hex" "%FIRMWARE_OUT%\" >nul
echo   OK   !CM7_BOOT_NAME!.bin/.hex/.elf built
set /a PASS+=1

call :BumpVersion "!SRC!\boot\bootloader_common.h" FIRMWARE_VERSION "Kinematic Brain CM7 application"
set "OUT=!H7!\cm7_app_obj"
del /q "!OUT!\*.o" 2>nul
arm-none-eabi-gcc %CFLAGS_CM7_APP% -I"!SRC!" -x c -c "!SRC!\STM32H745ZI_CM7_main.c" -o "!OUT!\STM32H745ZI_CM7_main.o"
if errorlevel 1 (
    echo   FAIL STM32H745ZI_CM7_main.c failed to compile
    set /a FAIL+=1
    goto :summary
)
set "CM7_APP_MAJOR=?" & set "CM7_APP_MINOR=?" & set "CM7_APP_PATCH=?"
for /f "tokens=3" %%v in ('findstr /c:"#define FIRMWARE_VERSION_MAJOR" "!SRC!\boot\bootloader_common.h"') do set "CM7_APP_MAJOR=%%v"
for /f "tokens=3" %%v in ('findstr /c:"#define FIRMWARE_VERSION_MINOR" "!SRC!\boot\bootloader_common.h"') do set "CM7_APP_MINOR=%%v"
for /f "tokens=3" %%v in ('findstr /c:"#define FIRMWARE_VERSION_PATCH" "!SRC!\boot\bootloader_common.h"') do set "CM7_APP_PATCH=%%v"
set "CM7_APP_NAME=HYDRA_KB_CM7_APP_v!CM7_APP_MAJOR!.!CM7_APP_MINOR!.!CM7_APP_PATCH!"
arm-none-eabi-gcc %LDCOMMON_CM7% -T"!SRC!\STM32H745ZITx_CM7_APP.ld" "!H7!\cm7\startup.o" "!H7!\cm7\system_stm32h7xx.o" "!OUT!\*.o" "!H7!\hal_obj_cm7\*.o" "!H7!\cm7_freertos_obj\*.o" -o "!OUT!\!CM7_APP_NAME!.elf"
if errorlevel 1 (
    echo   FAIL CM7 application: link failed
    set /a FAIL+=1
    goto :summary
)
arm-none-eabi-objcopy -O binary "!OUT!\!CM7_APP_NAME!.elf" "!OUT!\!CM7_APP_NAME!.bin"
arm-none-eabi-objcopy -O ihex "!OUT!\!CM7_APP_NAME!.elf" "!OUT!\!CM7_APP_NAME!.hex"
copy /y "!OUT!\!CM7_APP_NAME!.elf" "%FIRMWARE_OUT%\" >nul
copy /y "!OUT!\!CM7_APP_NAME!.bin" "%FIRMWARE_OUT%\" >nul
copy /y "!OUT!\!CM7_APP_NAME!.hex" "%FIRMWARE_OUT%\" >nul
echo   OK   !CM7_APP_NAME!.bin/.hex/.elf built - FreeRTOS GPIO-toggle smoke test
set /a PASS+=1

REM -----------------------------------------------------------------------
echo.
echo === 10. Kinematic Brain CM4 bootloader ^(bare-metal CAN-OTA gateway: SPI1+FDCAN1+mailbox^) + application ^(FreeRTOS^) - src\mcu_stm32h745\CM4\ ===
REM -----------------------------------------------------------------------
set "SRC=%ROOT%\src\mcu_stm32h745\CM4"
call :BumpVersion "!SRC!\boot\bootloader_common.h" BOOTLOADER_VERSION "Kinematic Brain CM4 bootloader"
set "OUT=!H7!\cm4_boot_obj"
del /q "!OUT!\*.o" 2>nul
set "SECTION_FAIL=0"
for %%f in ("!SRC!\boot\*.c") do (
    arm-none-eabi-gcc %CFLAGS_CM4% -I"!SRC!\boot" %COMMON_INC% -x c -c "%%f" -o "!OUT!\%%~nf.o"
    if errorlevel 1 (echo   FAIL %%~nf.c failed to compile & set "SECTION_FAIL=1")
)
if "!SECTION_FAIL!"=="1" (
    echo   FAIL CM4 bootloader failed to compile - see errors above
    set /a FAIL+=1
    goto :summary
) else (
    echo   OK   CM4 bootloader_*.c compiled
    set /a PASS+=1
)
set "CM4_BOOT_MAJOR=?" & set "CM4_BOOT_MINOR=?" & set "CM4_BOOT_PATCH=?"
for /f "tokens=3" %%v in ('findstr /c:"#define BOOTLOADER_VERSION_MAJOR" "!SRC!\boot\bootloader_common.h"') do set "CM4_BOOT_MAJOR=%%v"
for /f "tokens=3" %%v in ('findstr /c:"#define BOOTLOADER_VERSION_MINOR" "!SRC!\boot\bootloader_common.h"') do set "CM4_BOOT_MINOR=%%v"
for /f "tokens=3" %%v in ('findstr /c:"#define BOOTLOADER_VERSION_PATCH" "!SRC!\boot\bootloader_common.h"') do set "CM4_BOOT_PATCH=%%v"
set "CM4_BOOT_NAME=HYDRA_KB_CM4_BOOTLOADER_v!CM4_BOOT_MAJOR!.!CM4_BOOT_MINOR!.!CM4_BOOT_PATCH!"
arm-none-eabi-gcc %LDCOMMON_CM4% -T"!SRC!\boot\STM32H745ZITx_CM4_BOOTLOADER.ld" "!H7!\cm4\startup.o" "!H7!\cm4\system_stm32h7xx.o" "!OUT!\*.o" "!H7!\hal_obj_cm4\*.o" -o "!OUT!\!CM4_BOOT_NAME!.elf"
if errorlevel 1 (
    echo   FAIL CM4 bootloader: link failed
    set /a FAIL+=1
    goto :summary
)
arm-none-eabi-objcopy -O binary "!OUT!\!CM4_BOOT_NAME!.elf" "!OUT!\!CM4_BOOT_NAME!.bin"
arm-none-eabi-objcopy -O ihex "!OUT!\!CM4_BOOT_NAME!.elf" "!OUT!\!CM4_BOOT_NAME!.hex"
copy /y "!OUT!\!CM4_BOOT_NAME!.elf" "%FIRMWARE_OUT%\" >nul
copy /y "!OUT!\!CM4_BOOT_NAME!.bin" "%FIRMWARE_OUT%\" >nul
copy /y "!OUT!\!CM4_BOOT_NAME!.hex" "%FIRMWARE_OUT%\" >nul
echo   OK   !CM4_BOOT_NAME!.bin/.hex/.elf built
set /a PASS+=1

call :BumpVersion "!SRC!\boot\bootloader_common.h" FIRMWARE_VERSION "Kinematic Brain CM4 application"
set "OUT=!H7!\cm4_app_obj"
del /q "!OUT!\*.o" 2>nul
arm-none-eabi-gcc %CFLAGS_CM4_APP% -I"!SRC!" -x c -c "!SRC!\STM32H745ZI_CM4_main.c" -o "!OUT!\STM32H745ZI_CM4_main.o"
if errorlevel 1 (
    echo   FAIL STM32H745ZI_CM4_main.c failed to compile
    set /a FAIL+=1
    goto :summary
)
set "CM4_APP_MAJOR=?" & set "CM4_APP_MINOR=?" & set "CM4_APP_PATCH=?"
for /f "tokens=3" %%v in ('findstr /c:"#define FIRMWARE_VERSION_MAJOR" "!SRC!\boot\bootloader_common.h"') do set "CM4_APP_MAJOR=%%v"
for /f "tokens=3" %%v in ('findstr /c:"#define FIRMWARE_VERSION_MINOR" "!SRC!\boot\bootloader_common.h"') do set "CM4_APP_MINOR=%%v"
for /f "tokens=3" %%v in ('findstr /c:"#define FIRMWARE_VERSION_PATCH" "!SRC!\boot\bootloader_common.h"') do set "CM4_APP_PATCH=%%v"
set "CM4_APP_NAME=HYDRA_KB_CM4_APP_v!CM4_APP_MAJOR!.!CM4_APP_MINOR!.!CM4_APP_PATCH!"
arm-none-eabi-gcc %LDCOMMON_CM4% -T"!SRC!\STM32H745ZITx_CM4_APP.ld" "!H7!\cm4\startup.o" "!H7!\cm4\system_stm32h7xx.o" "!OUT!\*.o" "!H7!\hal_obj_cm4\*.o" "!H7!\cm4_freertos_obj\*.o" -o "!OUT!\!CM4_APP_NAME!.elf"
if errorlevel 1 (
    echo   FAIL CM4 application: link failed
    set /a FAIL+=1
    goto :summary
)
arm-none-eabi-objcopy -O binary "!OUT!\!CM4_APP_NAME!.elf" "!OUT!\!CM4_APP_NAME!.bin"
arm-none-eabi-objcopy -O ihex "!OUT!\!CM4_APP_NAME!.elf" "!OUT!\!CM4_APP_NAME!.hex"
copy /y "!OUT!\!CM4_APP_NAME!.elf" "%FIRMWARE_OUT%\" >nul
copy /y "!OUT!\!CM4_APP_NAME!.bin" "%FIRMWARE_OUT%\" >nul
copy /y "!OUT!\!CM4_APP_NAME!.hex" "%FIRMWARE_OUT%\" >nul
echo   OK   !CM4_APP_NAME!.bin/.hex/.elf built - FreeRTOS GPIO-toggle smoke test, no CM7^<-^>CM4 scheduler sync yet
set /a PASS+=1
:SKIP_H745

REM -----------------------------------------------------------------------
echo.
echo === 11. Firmware manifest ^(firmware\firmware_manifest.json^) ===
REM -----------------------------------------------------------------------
REM Only meaningful once every component exists - a partial build (g474-only
REM or h745-only) would make generate_manifest.py fail looking for the other
REM chip's own binaries, so this only runs for a full 'all' build (the
REM default with no target argument, or an explicit 'all').
if "%TARGET%"=="all" (
    REM python is a hard requirement (step 1), so no availability check needed here.
    python "%ROOT%\generate_manifest.py" "%ROOT%"
    if errorlevel 1 (
        echo   FAIL generate_manifest.py failed - see the traceback above
        set /a FAIL+=1
    ) else (
        echo   OK   firmware_manifest.json regenerated - see it for exact versions/CRC32 of every component just built
        set /a PASS+=1
    )
) else (
    echo   ^(skipped - only regenerated on a full 'all' build, see this script's own comment^)
)

:summary
echo.
echo === Summary ===
echo !PASS! passed, !WARN! warnings, !FAIL! failed
echo.
echo Output binaries are in: %FIRMWARE_OUT%\
echo.
echo Reminder: every _APP binary above is still a FreeRTOS GPIO-toggle
echo smoke test - real motion/vision/relay application logic is not yet
echo written. Every _BOOTLOADER binary IS the real CAN-OTA/SPI-OTA protocol
echo (bare-metal, no FreeRTOS by design) - not yet verified against real
echo hardware. See each src\*\README.md and docs\architecture.md.

REM Keeps the window open when this script is double-clicked from Explorer,
REM on success AND on failure - every FAIL path above reaches this same
REM :summary label via `goto :summary`, so one `pause` here covers all of
REM them. `pause` itself already no-ops instead of hanging when stdin isn't
REM a real console (e.g. redirected from NUL or a pipe - the same trick
REM `build_firmware.bat < NUL` relies on for unattended/automated runs).
echo.
pause

endlocal & set "FAIL=%FAIL%"
if %FAIL% GTR 0 exit /b 1
exit /b 0

REM =============================================================================
REM :BumpVersion <header_path> <MACRO_PREFIX> <label>
REM
REM Bumps ONE component's own version macro family (BOOTLOADER_VERSION or
REM FIRMWARE_VERSION) in its bootloader_common.h in place, via bump_version.py
REM (odometer carry rule: PATCH past 9 -> MINOR+1, PATCH resets to 0 - see
REM that script's own header comment). Called BEFORE each of the 6 components
REM below gets compiled, so the just-bumped value is what actually ends up
REM baked into that binary and in its output filename - never bumped after
REM the fact. Per this repo's own versioning policy, ALL 6 components (3
REM bootloaders + 3 applications) are incremental this way, unlike sibling
REM repo URTC where only the bootloaders are (and there, bumped by hand, not
REM automatically like here). Reached only via `call :BumpVersion ...` -
REM never falls into from normal top-to-bottom flow (the `exit /b 0` above
REM guarantees that).
REM =============================================================================
:BumpVersion
set "BUMP_HDR=%~1"
set "BUMP_PREFIX=%~2"
set "BUMP_LABEL=%~3"
set "BUMP_TMP=%BUILD%\bump_ver.tmp"
python "%ROOT%\bump_version.py" "%BUMP_HDR%" "%BUMP_PREFIX%" > "%BUMP_TMP%"
if errorlevel 1 (
    echo   FAIL %BUMP_LABEL%: version bump failed - see bump_version.py's own error above
    set /a FAIL+=1
    goto :summary
)
set "BUMP_NEWVER="
for /f "usebackq delims=" %%v in ("%BUMP_TMP%") do set "BUMP_NEWVER=%%v"
echo   OK   %BUMP_LABEL% version bumped to v!BUMP_NEWVER! ^(%BUMP_HDR%^)
set /a PASS+=1
goto :eof
