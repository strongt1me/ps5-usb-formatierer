#!/usr/bin/env bash
# Baut den Starter app/eboot.bin als nativen PS5-Titel (Ablauf wie ps5-native-app-boilerplate/tools/build.sh):
#   1. starter.c mit clang-18 und den Zielflags des SDK übersetzen (Header aus PS5_PAYLOAD_SDK, SDK 0.44)
#   2. mit prospero-lld des SDK zu einer Zwischen-PIE linken, Importe gegen die Stubs in target/lib
#   3. ps5-native-tool: in einen PS5-Titel umwandeln (Prozess-Parameter, SDK-Kennungen) und signieren
# Aufruf aus dem Projektordner (für .incbin "usb_format.elf"): PS5_PAYLOAD_SDK=... app/build_title.sh
# ps5-native-tool stammt aus ps5-native-app-boilerplate (build/host); Pfad über PS5_NATIVE_TOOL änderbar.
set -euo pipefail
: "${PS5_PAYLOAD_SDK:?PS5_PAYLOAD_SDK setzen}"
TOOL=${PS5_NATIVE_TOOL:-$HOME/lmms-ps5/boilerplate/build/host/ps5-native-tool}
CLANG=${PS5_CLANG:-$(command -v clang-18)}
[[ -x $TOOL ]] || { echo "ps5-native-tool fehlt: $TOOL" >&2; exit 2; }
[[ -x $CLANG ]] || { echo "clang-18 fehlt" >&2; exit 2; }

# Werte von ps5-native-app-boilerplate, dort auf FW 6.02 und 12.70 geprüft
MODULE_SDK=0x02000009
COMPANION_SDK=0x08050001
FSELF_MAGIC=0x1D3D154F

cd "$(dirname "$0")/.."
B=app/build
mkdir -p "$B"
flags=(-target x86_64-sie-ps5 -fvisibility-nodllstorageclass=default -isysroot "$PS5_PAYLOAD_SDK"
       -isystem "$PS5_PAYLOAD_SDK/target/include" -fno-stack-protector -fno-plt -femulated-tls -fPIC
       -ffreestanding -fno-builtin -std=c11 -O2 -Wall -Wextra -Werror -I.
       -funwind-tables -fasynchronous-unwind-tables)   # ps5-native-tool verlangt .eh_frame
"$CLANG" "${flags[@]}" -c app/title/starter.c -o "$B/starter.o"

"$PS5_PAYLOAD_SDK/bin/prospero-lld" -T app/title/title.ld --eh-frame-hdr \
    --version-script app/title/symbols.map -e _start -o "$B/pie.elf" "$B/starter.o" \
    --as-needed "$PS5_PAYLOAD_SDK"/target/lib/*.so

# Nur diese Importe sind vorgesehen (libkernel, libSceSystemService) - alles andere wäre zur Laufzeit NULL
allowed="close connect recv sceKernelDebugOutText sceKernelSendNotificationRequest sceKernelUsleep sceSystemServiceLoadExec send socket"
imports=$("$PS5_PAYLOAD_SDK/bin/llvm-nm" -D --undefined-only "$B/pie.elf" | awk '{ sub(/@.*/, "", $2); print $2 }' | sort -u)
for sym in $imports; do
    [[ " $allowed " == *" $sym "* ]] || { echo "unerwarteter Import: $sym" >&2; exit 1; }
done
echo "Importe: $(echo $imports)"

"$TOOL" link --in "$B/pie.elf" --out "$B/eboot.elf" --stub-dir "$PS5_PAYLOAD_SDK/target/lib" \
    --module-sdk "$MODULE_SDK" --companion-sdk "$COMPANION_SDK" --file-name eboot.elf
"$TOOL" self --sign --in "$B/eboot.elf" --out app/eboot.bin --magic "$FSELF_MAGIC"
"$TOOL" self --inspect --file app/eboot.bin
