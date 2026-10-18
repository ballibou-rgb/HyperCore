# HyperCore Studio – Custom Agent-Werkzeuge

Custom-Werkzeuge erweitern den KI-Agenten um eigene Shell-Befehle.
Sie erscheinen dem Agenten als vollwertige Tools neben den eingebauten.

## Speicherort

```
~/.config/hypercore/tools/
```

Jede `.json`-Datei in diesem Verzeichnis ist ein Werkzeug.
Der Agent lädt sie automatisch beim Start und nach "Neu laden" in den Einstellungen.

---

## Minimales Werkzeug

```json
{
  "name": "check_syntax",
  "description": "Prüft alle PHP-Dateien im Projekt auf Syntaxfehler",
  "command": "find . -name '*.php' | xargs php -l 2>&1"
}
```

Felder:
| Feld | Pflicht | Beschreibung |
|------|---------|--------------|
| `name` | ✔ | Eindeutiger Name (kein Leerzeichen, kein Sonderzeichen) |
| `description` | ✔ | Was das Werkzeug tut – der Agent liest diese Beschreibung |
| `command` | ✔ | Shell-Befehl der ausgeführt wird |
| `args` | – | Parameter-Liste (siehe unten) |

---

## Werkzeug mit Parametern

Parameter werden als `{argname}` im Command eingesetzt.
Der Wert wird automatisch shell-gequotet (kein Injection-Risiko).

```json
{
  "name": "db_query",
  "description": "SQL-Abfrage auf der Projekt-Datenbank ausführen",
  "command": "mariadb -h 127.0.0.1 -P {port} -u root -e {query}",
  "args": [
    {
      "name": "query",
      "description": "SQL-Abfrage (einzeilig, z.B. SELECT * FROM users LIMIT 5)"
    },
    {
      "name": "port",
      "description": "Datenbankport",
      "optional": true
    }
  ]
}
```

Argument-Felder:
| Feld | Pflicht | Beschreibung |
|------|---------|--------------|
| `name` | ✔ | Parametername (entspricht `{name}` im command) |
| `description` | ✔ | Beschreibung für den Agenten |
| `optional` | – | `true` = nicht erforderlich, wird durch `""` ersetzt |

---

## Praxisbeispiele

### NPM-Skript ausführen
```json
{
  "name": "npm_run",
  "description": "Ein NPM-Skript aus package.json ausführen (z.B. lint, test, build)",
  "command": "npm run {script} 2>&1",
  "args": [
    { "name": "script", "description": "Name des npm-Skripts (lint, test, build, …)" }
  ]
}
```

### Composer installieren
```json
{
  "name": "composer_install",
  "description": "PHP-Abhängigkeiten per Composer installieren",
  "command": "composer install --no-interaction 2>&1"
}
```

### Datenbankdump erstellen
```json
{
  "name": "db_dump",
  "description": "Datenbankdump in die Datei backup.sql schreiben",
  "command": "mariadb-dump -h 127.0.0.1 -P {port} -u root {database} > backup.sql 2>&1",
  "args": [
    { "name": "database", "description": "Name der Datenbank" },
    { "name": "port",     "description": "DB-Port", "optional": true }
  ]
}
```

### Python-Tests ausführen
```json
{
  "name": "run_tests",
  "description": "Python-Unit-Tests mit pytest ausführen",
  "command": "python3 -m pytest {path} -v 2>&1",
  "args": [
    { "name": "path", "description": "Pfad zu den Tests (leer = alle)", "optional": true }
  ]
}
```

### Datei mit einem Tool analysieren
```json
{
  "name": "analyze_file",
  "description": "Eine Datei mit wc, head, tail oder grep analysieren",
  "command": "{tool} {args} {filepath}",
  "args": [
    { "name": "tool",     "description": "Tool: wc, head, tail, grep, …" },
    { "name": "filepath", "description": "Relativer Pfad zur Datei" },
    { "name": "args",     "description": "Zusätzliche Argumente (z.B. -n 20 für head)", "optional": true }
  ]
}
```

---

## Tipps

**Ausführungsumgebung:**
- CWD ist immer der Projektordner
- Läuft in der konfigurierten Sandbox (LOCAL / CHROOT / DOCKER)
- Alle Umgebungsvariablen der Sandbox sind verfügbar

**Ausgabe:**
- stdout und stderr werden zusammengeführt zurückgegeben
- Maximal 8000 Zeichen (Rest wird abgeschnitten)
- Exit-Code wird am Ende angehängt: `[exit 0]`

**Sicherheit:**
- Argumente werden automatisch shell-gequotet
- Kein Ausbruch über `cd ../..` möglich (CWD bleibt Projektordner)
- Keine Netzwerkverbindung außerhalb der Sandbox

**Name-Konventionen:**
- Kleinbuchstaben + Unterstriche: `check_syntax`, `run_tests`
- Kein Präfix `tool_` nötig
- Keine Duplikate zu eingebauten Tools: list_directory, read_file, write_file,
  replace_in_file, search_project, project_status, run_command, get_url, run_build

---

## Verwaltung in HyperCore Studio

**Einstellungen → Werkzeuge** zeigt alle eingebauten und eigenen Werkzeuge.

Buttons:
- **+ Neu** – Legt eine Vorlage-JSON an und öffnet sie im Editor
- **Neu laden** – Lädt alle JSON-Dateien neu (ohne Neustart)
- **Ordner öffnen** – Öffnet `~/.config/hypercore/tools/` im Dateimanager
- **✏ (Stift)** – Öffnet die JSON-Datei im Editor
- **🗑 (Löschen)** – Löscht die JSON-Datei (nach Bestätigung)

---

## Fehlersuche

Der Agent meldet Fehler direkt im Chat:

| Fehler | Ursache | Lösung |
|--------|---------|--------|
| `Unbekanntes Werkzeug: xyz` | Name stimmt nicht | Name in JSON prüfen |
| `Kein Projekt geöffnet` | Kein aktives Projekt | Projekt öffnen |
| `Befehl konnte nicht gestartet werden` | Tool nicht in Sandbox | Tool installieren |
| `[exit 1]` | Befehl fehlgeschlagen | Stderr-Ausgabe lesen |

Zum Testen eines Werkzeugs direkt im Chat:
> "Führe das Werkzeug `check_syntax` aus."
