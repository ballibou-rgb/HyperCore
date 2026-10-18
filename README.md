# HyperCore Studio — IDE (GTK4) · DATANET.OVH LTD

## Bauen
    sudo apt install gcc pkg-config dpkg-dev libglib2.0-dev-bin \
      libgtk-4-dev libgtksourceview-5-dev libvte-2.91-gtk4-dev \
      libwebkitgtk-6.0-dev libjson-glib-dev libsoup-3.0-dev libgit2-dev
    ./build.sh --deb && sudo apt install ./dist/hypercore-studio_1.0.0_amd64.deb
    # schnell: ./build.sh --bin && ./dist/hypercore

## Hinweise
- Logfile: ~/.cache/hypercore/hypercore.log
- Live-Preview gilt fuer .ui (GTK), .html und .php — nicht fuer .c/.py.
- Agent/RAG brauchen einen laufenden Ollama-Server (sonst lexikalischer Fallback).
- Nativer Emulator: Menue Werkzeuge -> "Im Emulator starten" (braucht broadwayd).
