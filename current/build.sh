#!/bin/bash

clear
set -e

# always build from the folder that contains this script
cd "$(dirname "$(readlink -f "$0")")"

echo "======== TempeR Build ========"
echo

# ── check required tools ──────────────────────────────────────────────────────

missing=()

if ! command -v gcc >/dev/null 2>&1 && ! command -v clang >/dev/null 2>&1; then
    missing+=("gcc (or clang)")
fi

if ! command -v pkg-config >/dev/null 2>&1; then
    missing+=("pkg-config")
fi

if [ ${#missing[@]} -ne 0 ]; then
    echo "ERROR: Some required build tools are missing:"
    echo

    for dep in "${missing[@]}"; do
        echo "  - $dep"
    done

    echo
    echo "Please install the required build dependencies and try again."
    echo

    if command -v pacman >/dev/null 2>&1; then
        echo "On Arch Linux (and Arch based systems):"
        echo "  sudo pacman -S --needed base-devel pkgconf gtk4"
    fi

    exit 1
fi

# ── check GTK 4 ───────────────────────────────────────────────────────────────

if ! pkg-config --exists "gtk4 >= 4.12" 2>/dev/null; then
    echo "ERROR: GTK 4 (>= 4.12) development files not found."
    echo
    if command -v pacman >/dev/null 2>&1; then
        echo "  sudo pacman -S --needed gtk4"
    fi
    exit 1
fi

# ── check source files ────────────────────────────────────────────────────────

SRC=(src/main.c src/gui.c src/sensors.c src/settings.c src/monitor.c \
     src/icons.c src/overlay.c src/tray.c src/specs.c)

for f in "${SRC[@]}" src/tempfm.h; do
    if [ ! -f "$f" ]; then
        echo "ERROR: $f not found."
        echo
        echo "Please run this script from the project root (the folder with src/)."
        exit 1
    fi
done

# ── pick compiler ─────────────────────────────────────────────────────────────

if command -v gcc >/dev/null 2>&1; then
    CC=gcc
else
    CC=clang
fi

# ── Tela icon theme (component icons) ─────────────────────────────────────────
# Same icons as ExploreR.  Without Tela the program still works and shows
# emoji instead.

tela_installed() {
    for d in /usr/share/icons/Tela /usr/local/share/icons/Tela \
             "$HOME/.local/share/icons/Tela" "$HOME/.icons/Tela"; do
        [ -f "$d/index.theme" ] && return 0
    done
    command -v pacman >/dev/null 2>&1 && pacman -Qq tela-icon-theme >/dev/null 2>&1 && return 0
    return 1
}

if tela_installed; then
    echo "Icons      : Tela icon theme found"
else
    echo "Icons      : Tela icon theme not found, installing it..."
    if command -v yay >/dev/null 2>&1; then
        yay -S --needed tela-icon-theme || true
    elif command -v paru >/dev/null 2>&1; then
        paru -S --needed tela-icon-theme || true
    else
        echo "             Neither yay nor paru is available."
        echo "             Install Tela by hand: https://github.com/vinceliuice/Tela-icon-theme"
    fi
    if tela_installed; then
        echo "Icons      : Tela installed"
    else
        echo "Icons      : Tela still missing, TempeR will show emoji icons"
    fi
fi
echo

# ── X11 (overlay above full-screen windows) ──────────────────────────────────
# libX11 is already a dependency of GTK 4 on Arch: nothing to install.  The
# overlay uses it to stay above games through XWayland (or X11).

X11_CFLAGS=""
X11_LIBS=""
X11_DEFINE=""

if pkg-config --exists gtk4-x11 x11 2>/dev/null; then
    X11_CFLAGS=$(pkg-config --cflags x11)
    X11_LIBS=$(pkg-config --libs x11)
    X11_DEFINE="-DTEMPFM_HAVE_X11"
    echo "Overlay    : above full-screen windows (X11 / XWayland)"
else
    echo "Overlay    : GTK built without X11, shown as a normal window"
fi
echo

# ── optional: NVIDIA proprietary driver ───────────────────────────────────────
# The NVIDIA driver exports no hwmon sensor; TempeR reads that GPU through
# nvidia-smi (part of the nvidia-utils package) when it is installed.
# Nothing to link: it is only checked here to tell the user.

if [ -d /sys/bus/pci/drivers/nvidia ]; then
    if command -v nvidia-smi >/dev/null 2>&1; then
        echo "NVIDIA GPU : nvidia-smi found, GPU temperature available"
    else
        echo "NVIDIA GPU : nvidia-smi not found, GPU temperature unavailable"
        echo "             (install nvidia-utils to see it)"
    fi
    echo
fi

# ── build ─────────────────────────────────────────────────────────────────────

GTK_CFLAGS=$(pkg-config --cflags gtk4)
GTK_LIBS=$(pkg-config --libs gtk4)

echo "Compiler : $CC"
echo "GTK4     : $(pkg-config --modversion gtk4)"
echo "Backends : Wayland + X11"
echo "Sources  : ${SRC[*]}"
echo "Output   : ./TempeR"
echo

echo "Compiling..."
# shellcheck disable=SC2086
$CC $GTK_CFLAGS $X11_CFLAGS \
    -D_GNU_SOURCE $X11_DEFINE \
    -Wall -Wextra \
    -O2 \
    -o TempeR \
    "${SRC[@]}" \
    $GTK_LIBS $X11_LIBS

echo
echo "================================"
echo "Build complete!"
echo "Executable: ./TempeR"
echo "================================"
