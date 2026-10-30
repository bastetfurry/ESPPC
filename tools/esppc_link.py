# ESPPC: PlatformIO post script (platformio.ini, extra_scripts). Adds src/esppc_iram.ld to the
# firmware's link, which puts the CPU core's code in IRAM after all other code. Not in build_flags:
# those reach the bootloader's link as well, which has no IRAM text section to insert after.
import os

Import("env")

env.Prepend(LINKFLAGS=["-T", os.path.join(env.subst("$PROJECT_DIR"), "src", "esppc_iram.ld")])
