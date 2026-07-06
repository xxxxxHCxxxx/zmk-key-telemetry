target_sources_ifdef(CONFIG_ZMK_KEY_TELEMETRY app PRIVATE src/telemetry.c)

zephyr_include_directories(include)
