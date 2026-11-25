#!/usr/bin/env bash
set -euo pipefail

mode="${1:---all}"
repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"

usage() {
    echo "Usage: $0 [--all|--static|--firmware|--ios]" >&2
}

run_static_checks() {
    local test_dir
    local dead_strip_flag
    local demo

    # Apple ld does not accept --gc-sections; -dead_strip is the equivalent.
    dead_strip_flag="-Wl,--gc-sections"
    if [[ "$(uname -s)" == "Darwin" ]]; then
        dead_strip_flag="-Wl,-dead_strip"
    fi

    test_dir="$(mktemp -d /tmp/passport-studio-host-tests.XXXXXX)"

    # Board support package.
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Icomponents/bsp/src \
        tests/test_bsp_display_rounding.c components/bsp/src/bsp_display_rounding.c \
        -o "${test_dir}/test_bsp_display_rounding"
    "${test_dir}/test_bsp_display_rounding"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Icomponents/bsp/src \
        tests/test_bsp_es8311_sleep_check.c components/bsp/src/bsp_es8311_sleep_check.c \
        -o "${test_dir}/test_bsp_es8311_sleep_check"
    "${test_dir}/test_bsp_es8311_sleep_check"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror \
        -Itests/battery_stubs -Itests/bsp_stubs -Icomponents/bsp/include \
        tests/test_bsp_battery.c -o "${test_dir}/test_bsp_battery"
    "${test_dir}/test_bsp_battery"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror \
        -Itests/bsp_stubs -Icomponents/bsp/include \
        tests/test_bsp_button.c -o "${test_dir}/test_bsp_button"
    "${test_dir}/test_bsp_button"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror \
        -Itests/bsp_stubs -Icomponents/bsp/include \
        tests/test_bsp_lvgl_init.c components/bsp/src/bsp_display_rounding.c \
        -o "${test_dir}/test_bsp_lvgl_init"
    "${test_dir}/test_bsp_lvgl_init"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror \
        -Itests/audio_stubs -Icomponents/bsp/include -Icomponents/bsp/src \
        tests/test_bsp_audio_recovery.c components/bsp/src/bsp_es8311_sleep_check.c \
        -o "${test_dir}/test_bsp_audio_recovery"
    "${test_dir}/test_bsp_audio_recovery"

    # Official hardware-test demo.
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Iexamples/hardware-demo/main \
        tests/test_ui_pixel_math.c examples/hardware-demo/main/ui_pixel_math.c \
        -o "${test_dir}/test_ui_pixel_math"
    "${test_dir}/test_ui_pixel_math"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Iexamples/hardware-demo/main \
        tests/test_demo_navigation.c examples/hardware-demo/main/demo_navigation.c \
        -o "${test_dir}/test_demo_navigation"
    "${test_dir}/test_demo_navigation"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Iexamples/hardware-demo/main \
        tests/test_key_guard.c -o "${test_dir}/test_key_guard"
    "${test_dir}/test_key_guard"
    for demo in audio low_power ble wifi; do
        "${CC:-cc}" -std=c11 -Wall -Wextra -Werror \
            -ffunction-sections -fdata-sections \
            -Itests/demo_stubs -Iexamples/hardware-demo/main \
            "tests/test_demo_${demo}_runtime.c" "${dead_strip_flag}" \
            -o "${test_dir}/test_demo_${demo}_runtime"
        "${test_dir}/test_demo_${demo}_runtime"
    done

    # Installer, Passport Link, and the weather and T3 status examples.
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror \
        -Icomponents/installer_core/include -Itests/installer_support \
        tests/test_installer_core.c components/installer_core/inst_table.c \
        components/installer_core/inst_session.c components/installer_core/inst_wifi.c \
        -o "${test_dir}/test_installer_core"
    "${test_dir}/test_installer_core"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Icomponents/passport_link/include \
        tests/test_passport_link.c components/passport_link/link_core.c \
        -o "${test_dir}/test_passport_link"
    "${test_dir}/test_passport_link"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Iexamples/wifi-weather/main \
        tests/test_weather_logic.c examples/wifi-weather/main/weather_logic.c \
        examples/wifi-weather/main/weather_i18n.c \
        -o "${test_dir}/test_weather_logic"
    "${test_dir}/test_weather_logic"
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Iexamples/t3-status/main \
        tests/test_t3_logic.c examples/t3-status/main/t3_logic.c \
        -o "${test_dir}/test_t3_logic"
    "${test_dir}/test_t3_logic"
    node --check examples/t3-status/bridge/bridge.mjs
    node tests/test_t3_bridge.mjs
    PYTHONDONTWRITEBYTECODE=1 python3 tools/check_link_info.py
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_build_installer_bundle.py
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_verify_firmware.py
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_archive_firmware.py
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_battery_profile.py
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_deep_sleep_contract.py
    PYTHONDONTWRITEBYTECODE=1 python3 tests/test_key_wake_contract.py
    rm -rf "${test_dir}"
    echo "Host tests: PASS"
}

run_ios_checks() {
    (cd ios/PassportKit && swift test)
    echo "iOS logic tests: PASS"
}

run_firmware_checks() (
    local build_dir

    if ! command -v idf.py >/dev/null 2>&1; then
        echo "ERROR: idf.py is not available; activate ESP-IDF 5.5.3 first." >&2
        return 1
    fi

    build_dir="$(mktemp -d /tmp/passport-studio-firmware.XXXXXX)"
    trap 'case "${build_dir}" in /tmp/passport-studio-firmware.*) rm -rf -- "${build_dir}" ;; esac' EXIT
    mkdir -p "${repo_root}/build"

    # Resident installer, with the hooked bootloader and the shared partition
    # table, then the first-install image that carries all three.
    SDKCONFIG_DEFAULTS="${repo_root}/installer/sdkconfig.defaults" \
        idf.py -C installer -B "${build_dir}/installer" \
        -D "SDKCONFIG=${build_dir}/installer/sdkconfig" build
    PYTHONDONTWRITEBYTECODE=1 python3 tools/build_installer_bundle.py \
        --build "${build_dir}/installer" \
        -o "${repo_root}/build/Passport-Installer-FirstInstall-full.bin"

    # Wi-Fi weather example, installed through the iPhone app.
    SDKCONFIG_DEFAULTS="${repo_root}/examples/wifi-weather/sdkconfig.defaults" \
        idf.py -C examples/wifi-weather -B "${build_dir}/wifi-weather" \
        -D "SDKCONFIG=${build_dir}/wifi-weather/sdkconfig" build
    idf.py -C examples/wifi-weather -B "${build_dir}/wifi-weather" \
        -D "SDKCONFIG=${build_dir}/wifi-weather/sdkconfig" \
        merge-bin -o Wifi-Weather-full.bin
    python3 tools/verify_firmware.py "${build_dir}/wifi-weather" Wifi-Weather
    python3 tools/archive_firmware.py create "${build_dir}/wifi-weather" \
        --archive-root "${repo_root}/build/firmware" --project Wifi-Weather
    install -m 0644 "${build_dir}/wifi-weather/Wifi-Weather-full.bin" \
        "${repo_root}/build/Wifi-Weather-merged.bin"

    # T3 Code status display, installed through the iPhone app.
    SDKCONFIG_DEFAULTS="${repo_root}/examples/t3-status/sdkconfig.defaults" \
        idf.py -C examples/t3-status -B "${build_dir}/t3-status" \
        -D "SDKCONFIG=${build_dir}/t3-status/sdkconfig" build
    idf.py -C examples/t3-status -B "${build_dir}/t3-status" \
        -D "SDKCONFIG=${build_dir}/t3-status/sdkconfig" \
        merge-bin -o T3-Status-full.bin
    python3 tools/verify_firmware.py "${build_dir}/t3-status" T3-Status
    python3 tools/archive_firmware.py create "${build_dir}/t3-status" \
        --archive-root "${repo_root}/build/firmware" --project T3-Status
    install -m 0644 "${build_dir}/t3-status/T3-Status-full.bin" \
        "${repo_root}/build/T3-Status-merged.bin"

    # Official hardware-test demo, as a board self-check.
    SDKCONFIG_DEFAULTS="${repo_root}/examples/hardware-demo/sdkconfig.defaults" \
        idf.py -C examples/hardware-demo -B "${build_dir}/hardware-demo" \
        -D "SDKCONFIG=${build_dir}/hardware-demo/sdkconfig" build
    idf.py -C examples/hardware-demo -B "${build_dir}/hardware-demo" \
        -D "SDKCONFIG=${build_dir}/hardware-demo/sdkconfig" \
        merge-bin -o Hardware-Demo-full.bin
    python3 tools/verify_firmware.py "${build_dir}/hardware-demo" Hardware-Demo
    python3 tools/archive_firmware.py create "${build_dir}/hardware-demo" \
        --archive-root "${repo_root}/build/firmware" --project Hardware-Demo
    install -m 0644 "${build_dir}/hardware-demo/Hardware-Demo-full.bin" \
        "${repo_root}/build/Hardware-Demo-full.bin"
    echo "Firmware build: PASS"
)

cd "${repo_root}"
case "${mode}" in
    --all)
        run_static_checks
        run_ios_checks
        run_firmware_checks
        ;;
    --static)
        run_static_checks
        ;;
    --ios)
        run_ios_checks
        ;;
    --firmware)
        run_firmware_checks
        ;;
    *)
        usage
        exit 2
        ;;
esac
