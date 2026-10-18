#!/usr/bin/env bash
# HyperCore Studio - Build-Skript (Zero-Dependency Monolith via GResource)
#
# Nutzung:
#   ./build.sh --deb      baut ein Debian/Ubuntu .deb-Paket
#   ./build.sh --fedora   baut ein Fedora/RHEL .rpm-Paket
#   ./build.sh --bin      kompiliert nur das Binary nach dist/
#
# Debian Build-Voraussetzungen:
#   gcc pkg-config glib-compile-resources dpkg-deb
#   libgtk-4-dev libgtksourceview-5-dev libvte-2.91-gtk4-dev
#   libwebkitgtk-6.0-dev libjson-glib-dev libsoup-3.0-dev libgit2-dev
#
# Fedora Build-Voraussetzungen:
#   gcc pkgconf glib2-devel rpm-build rpmdevtools
#   gtk4-devel gtksourceview5-devel vte291-gtk4-devel
#   webkitgtk6.0-devel json-glib-devel libsoup3-devel libgit2-devel

set -euo pipefail
export PATH="$PATH:/usr/sbin:/sbin"

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC_DIR="$ROOT_DIR/src"
RES_DIR="$ROOT_DIR/resources"
DIST_DIR="$ROOT_DIR/dist"
PKG_DIR="$ROOT_DIR/packaging"

BIN="hypercore"
PKGNAME="hypercore-studio"
VERSION="$(grep 'define APP_VERSION' "$SRC_DIR/version.h" | sed -E 's/.*"([^"]+)".*/\1/')"

# pkg-config-Namen sind identisch auf Debian und Fedora
PKGS="gtk4 gtksourceview-5 vte-2.91-gtk4 webkitgtk-6.0 json-glib-1.0 libsoup-3.0 libgit2"

mkdir -p "$DIST_DIR"

# ------------------------------------------------------------------
# Detect distro family
# ------------------------------------------------------------------
detect_family() {
    if [ -f /etc/os-release ]; then
        . /etc/os-release
        case "${ID_LIKE:-$ID}" in
            *fedora*|*rhel*|*centos*) echo "rpm" ;;
            *debian*|*ubuntu*)        echo "deb" ;;
            *)
                case "${ID:-}" in
                    fedora|rhel|centos|rocky|alma) echo "rpm" ;;
                    *) echo "deb" ;;
                esac ;;
        esac
    else
        echo "deb"
    fi
}

# ------------------------------------------------------------------
# Paket-Namen je Family fuer Fehlermeldungen
# ------------------------------------------------------------------
DEV_PKGS_DEB="libgtk-4-dev libgtksourceview-5-dev libvte-2.91-gtk4-dev \
libwebkitgtk-6.0-dev libjson-glib-dev libsoup-3.0-dev libgit2-dev"

DEV_PKGS_RPM="gtk4-devel gtksourceview5-devel vte291-gtk4-devel \
webkitgtk6.0-devel json-glib-devel libsoup3-devel libgit2-devel"

# ------------------------------------------------------------------
# Tools pruefen
# ------------------------------------------------------------------
check_tools() {
    local miss=0
    for t in gcc pkg-config glib-compile-resources; do
        command -v "$t" >/dev/null 2>&1 || { echo "FEHLT: $t"; miss=1; }
    done
    for p in $PKGS; do
        pkg-config --exists "$p" 2>/dev/null || { echo "FEHLT (dev): $p"; miss=1; }
    done
    if [ "$miss" = "1" ]; then
        echo "==> Build-Abhaengigkeiten fehlen."
        local fam; fam="$(detect_family)"
        if [ "$fam" = "rpm" ]; then
            echo "    Fedora/RHEL: sudo dnf install $DEV_PKGS_RPM"
        else
            echo "    Debian/Ubuntu: sudo apt install $DEV_PKGS_DEB"
        fi
        exit 1
    fi
}

# ------------------------------------------------------------------
# Binary bauen (shared)
# ------------------------------------------------------------------
build_binary() {
    check_tools
    echo "==> Kompiliere Ressourcen (GResource-Monolith)"
    glib-compile-resources --sourcedir="$RES_DIR" \
        --target="$DIST_DIR/resources.c" --generate-source \
        "$RES_DIR/hypercore.gresource.xml"

    local DEBUG_FLAGS=""
    if [ "${HC_DEBUG_BUILD:-0}" = "1" ]; then
        DEBUG_FLAGS="-DHC_DEBUG -g -O0"
        echo "==> DEBUG-Build aktiv (HC_DEBUG=1, Optimierung deaktiviert)"
    fi

    echo "==> Kompiliere $PKGNAME $VERSION"
    gcc ${DEBUG_FLAGS:--O2} -o "$DIST_DIR/$BIN" \
        "$SRC_DIR"/*.c "$DIST_DIR/resources.c" -I"$SRC_DIR" \
        $(pkg-config --cflags --libs $PKGS) -lm -Wall
    echo "==> Binary: $DIST_DIR/$BIN"
}

# ------------------------------------------------------------------
# Dynamische Laufzeit-Abhaengigkeiten (Debian)
# ------------------------------------------------------------------
gen_deps_deb() {
    local bin="$DIST_DIR/$BIN" out="" real p path
    while read -r path; do
        [ -z "$path" ] && continue
        real="$(readlink -f "$path" 2>/dev/null)"
        [ -z "$real" ] && real="$path"
        p="$(dpkg -S "$real" 2>/dev/null | cut -d: -f1 | head -1)"
        [ -n "$p" ] && out="$out $p"
    done < <(ldd "$bin" 2>/dev/null | grep -E \
        'libgtk-4|libgtksourceview-5|libvte-2\.91-gtk4|libwebkitgtk-6\.0|libjson-glib-1\.0|libsoup-3\.0|libgit2' \
        | awk '{print $3}')
    echo "$out" | tr ' ' '\n' | sed '/^$/d' | sort -u | tr '\n' ',' | sed 's/,$//; s/,/, /g'
}

# ------------------------------------------------------------------
# Dynamische Laufzeit-Abhaengigkeiten (Fedora/RPM)
# ------------------------------------------------------------------
gen_deps_rpm() {
    local bin="$DIST_DIR/$BIN" out="" path pkg
    while read -r path; do
        [ -z "$path" ] && continue
        path="$(readlink -f "$path" 2>/dev/null || echo "$path")"
        pkg="$(rpm -qf "$path" 2>/dev/null | head -1)"
        # Nur den Paketnamen ohne Version/Release (fuer Requires-Zeile)
        pkg="$(echo "$pkg" | sed -E 's/-[0-9].*$//')"
        [ -n "$pkg" ] && [[ "$pkg" != *"not owned"* ]] && out="$out $pkg"
    done < <(ldd "$bin" 2>/dev/null | grep -E \
        'libgtk-4|libgtksourceview|libvte|libwebkit|libjson-glib|libsoup|libgit2' \
        | awk '{print $3}')
    echo "$out" | tr ' ' '\n' | sed '/^$/d' | sort -u | tr '\n' ' '
}

# ------------------------------------------------------------------
# .deb bauen
# ------------------------------------------------------------------
build_deb() {
    build_binary
    echo "==> Baue .deb-Paket"

    if ! command -v dpkg-deb >/dev/null 2>&1; then
        echo "FEHLT: dpkg-deb  (sudo apt install dpkg)"; exit 1
    fi

    local DEPS
    DEPS="$(gen_deps_deb)"
    [ -z "$DEPS" ] && DEPS="libgtk-4-1, libgtksourceview-5-0, libvte-2.91-gtk4-0, libwebkitgtk-6.0-4, libjson-glib-1.0-0, libsoup-3.0-0, libgit2-1.7"
    echo "    Abhaengigkeiten: $DEPS"

    local r="$DIST_DIR/deb-root"
    rm -rf "$r"
    mkdir -p "$r/DEBIAN" "$r/usr/bin" "$r/usr/share/applications" \
             "$r/usr/share/icons/hicolor/scalable/apps" \
             "$r/usr/share/icons/hicolor/256x256/apps" \
             "$r/usr/share/mime/packages" \
             "$r/usr/share/doc/$PKGNAME" \
             "$r/usr/libexec/hypercore"

    install -m 755 "$DIST_DIR/$BIN"              "$r/usr/bin/$BIN"
    install -m 644 "$RES_DIR/icons/app-logo.svg" "$r/usr/share/icons/hicolor/scalable/apps/$PKGNAME.svg"
    install -m 644 "$RES_DIR/icons/app-icon.png" "$r/usr/share/icons/hicolor/256x256/apps/$PKGNAME.png"
    install -m 644 "$ROOT_DIR/INFO.txt"          "$r/usr/share/doc/$PKGNAME/INFO.txt"
    [ -f "$ROOT_DIR/CHANGELOG.md" ] && \
        install -m 644 "$ROOT_DIR/CHANGELOG.md"  "$r/usr/share/doc/$PKGNAME/CHANGELOG.md"
    install -m 755 "$PKG_DIR/hypercore-sandbox-setup" \
                   "$r/usr/libexec/hypercore/hypercore-sandbox-setup"

    sed -e "s/\${VERSION}/$VERSION/" -e "s|\${DEPS}|$DEPS|" \
        "$PKG_DIR/debian/control.in" > "$r/DEBIAN/control"
    install -m 755 "$PKG_DIR/debian/postinst" "$r/DEBIAN/postinst"
    install -m 755 "$PKG_DIR/debian/postrm"   "$r/DEBIAN/postrm"

    cat > "$r/usr/share/applications/$PKGNAME.desktop" <<DESKTOP
[Desktop Entry]
Type=Application
Name=HyperCore Studio
GenericName=Entwicklungsumgebung
Comment=IDE fuer C, C++, Python, Bash und PHP
Exec=/usr/bin/hypercore %f
Icon=$PKGNAME
Terminal=false
Categories=Development;IDE;
MimeType=application/x-hypercore-project;
StartupNotify=true
DESKTOP

    cat > "$r/usr/share/mime/packages/$PKGNAME.xml" <<'MIME'
<?xml version="1.0" encoding="UTF-8"?>
<mime-info xmlns="http://www.freedesktop.org/standards/shared-mime-info">
  <mime-type type="application/x-hypercore-project">
    <comment>HyperCore Studio Projekt</comment>
    <comment xml:lang="de">HyperCore Studio Projekt</comment>
    <glob pattern="*.hcs"/>
    <icon name="hypercore-studio"/>
  </mime-type>
</mime-info>
MIME

    chmod 755 "$r/DEBIAN"
    dpkg-deb --build --root-owner-group "$r" \
        "$DIST_DIR/${PKGNAME}_${VERSION}_amd64.deb" >/dev/null
    rm -rf "$r"
    echo "==> Fertig: $DIST_DIR/${PKGNAME}_${VERSION}_amd64.deb"
    echo "    Installieren mit:  sudo apt install $DIST_DIR/${PKGNAME}_${VERSION}_amd64.deb"
}

# ------------------------------------------------------------------
# .rpm bauen (Fedora / RHEL / Rocky / Alma)
# ------------------------------------------------------------------
build_rpm() {
    build_binary
    echo "==> Baue .rpm-Paket"

    if ! command -v rpmbuild >/dev/null 2>&1; then
        echo "FEHLT: rpmbuild  (sudo dnf install rpm-build rpmdevtools)"; exit 1
    fi

    # rpmbuild-Verzeichnisstruktur unter dist/ anlegen (kein ~/rpmbuild)
    local RPMROOT="$DIST_DIR/rpmbuild"
    mkdir -p "$RPMROOT"/{BUILD,BUILDROOT,RPMS,SOURCES,SPECS,SRPMS}

    # Quellen ins SOURCES-Verzeichnis kopieren
    cp "$DIST_DIR/$BIN"                          "$RPMROOT/SOURCES/hypercore"
    cp "$RES_DIR/icons/app-logo.svg"             "$RPMROOT/SOURCES/app-logo.svg"
    cp "$RES_DIR/icons/app-icon.png"             "$RPMROOT/SOURCES/app-icon.png"
    cp "$ROOT_DIR/INFO.txt"                      "$RPMROOT/SOURCES/INFO.txt"
    cp "$PKG_DIR/hypercore-sandbox-setup"        "$RPMROOT/SOURCES/hypercore-sandbox-setup"

    # Laufzeit-Abhaengigkeiten ermitteln
    local DEPS_RPM
    DEPS_RPM="$(gen_deps_rpm)"
    if [ -z "$DEPS_RPM" ]; then
        # Fallback: bekanntnte Fedora-Paketnamen
        DEPS_RPM="gtk4 gtksourceview5 vte291-gtk4 webkitgtk6.0 json-glib libsoup3 libgit2"
    fi
    echo "    Abhaengigkeiten: $DEPS_RPM"

    # Requires-Zeilen aus der Leerzeichen-getrennten Liste bauen
    local REQUIRES_LINES=""
    for pkg in $DEPS_RPM; do
        REQUIRES_LINES="${REQUIRES_LINES}Requires:       $pkg"$'\n'
    done

    local CHDATE
    CHDATE="$(date '+%a %b %d %Y')"

    # Spec aus Template erzeugen via awk (multiline-sicher).
    awk \
        -v VERSION="$VERSION" \
        -v REQUIRES="$REQUIRES_LINES" \
        -v CHDATE="$CHDATE" \
        '
        /\$\{VERSION\}/        { gsub(/\$\{VERSION\}/, VERSION) }
        /\$\{REQUIRES_LINES\}/ { printf "%s", REQUIRES; next }
        { print }
        END {
            print ""
            print "%changelog"
            print "* " CHDATE " build <build@datanet.ovh> - " VERSION "-1"
            print "- Automatisch generiertes Paket"
        }
        ' "$PKG_DIR/fedora/hypercore-studio.spec.in" \
        > "$RPMROOT/SPECS/hypercore-studio.spec"

    # Arch aus dem System ableiten
    local ARCH
    ARCH="$(uname -m)"

    rpmbuild -bb \
        --define "_topdir $RPMROOT" \
        --define "_bindir /usr/bin" \
        --define "_datadir /usr/share" \
        --define "_libexecdir /usr/libexec" \
        "$RPMROOT/SPECS/hypercore-studio.spec" 2>&1 | \
        grep -v "^Processing files\|^Requires\|^Checking\|^Wrote:" || true

    local RPM_OUT
    RPM_OUT="$(find "$RPMROOT/RPMS" -name "*.rpm" | head -1)"
    if [ -z "$RPM_OUT" ]; then
        echo "!! rpmbuild hat kein RPM erzeugt - Logausgabe oben pruefen."; exit 1
    fi

    cp "$RPM_OUT" "$DIST_DIR/${PKGNAME}-${VERSION}-1.${ARCH}.rpm"
    rm -rf "$RPMROOT"
    echo "==> Fertig: $DIST_DIR/${PKGNAME}-${VERSION}-1.${ARCH}.rpm"
    echo "    Installieren mit:  sudo dnf install $DIST_DIR/${PKGNAME}-${VERSION}-1.${ARCH}.rpm"
}

# ------------------------------------------------------------------
# Einstiegspunkt
# ------------------------------------------------------------------
# --debug als optionales zweites Argument (z.B. ./build.sh --deb --debug)
for arg in "$@"; do
    [ "$arg" = "--debug" ] && export HC_DEBUG_BUILD=1
done

case "${1:-}" in
    --deb)    build_deb    ;;
    --fedora) build_rpm    ;;
    --bin)    build_binary ;;
    *)
        echo "Nutzung: $0 --deb | --fedora | --bin [--debug]"
        echo ""
        echo "  --deb      Debian/Ubuntu .deb-Paket"
        echo "  --fedora   Fedora/RHEL/Rocky/Alma .rpm-Paket"
        echo "  --bin      Nur Binary kompilieren (kein Paket)"
        echo "  --debug    Debug-Build: HC_DEBUG aktiviert, kein Optimieren"
        exit 1
        ;;
esac
