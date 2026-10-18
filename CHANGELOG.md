# HyperCore Studio v1.1 - "Alles laeuft beim Oeffnen"

Umbau zu einer IDE, die beim Oeffnen eines Projekts die komplette
Umgebung hochfaehrt - Sandbox, Dienste, Live-Ansicht - statt nur einen
Dateibaum zu zeichnen.

## Sandbox ist wieder echt
- Der erzwungene Rueckfall auf LOCAL ist weg: CHROOT und DOCKER werden
  tatsaechlich benutzt. Bei DOCKER laeuft ein persistenter Container pro
  Projekt mit Bind-Mount und veroeffentlichten Ports, damit Dienste
  dauerhaft darin leben koennen.
- Build, Run, Syntaxpruefung und Agenten-Werkzeuge laufen alle ueber
  dieselbe Sandbox-Instanz (`sandbox_exec_argv`).
- Der Abhaengigkeits-Check prueft IN der Sandbox und richtet sich nach
  Sprache UND Projekttyp statt pauschal nach GTK.

## Maschinenraum: Dienste starten automatisch (services.c, neu)
- Projekt oeffnen startet, was der Projekttyp braucht: Webserver
  (`php -S` bzw. `python3 -m http.server`), Datenbank (MariaDB mit
  eigenem Datenverzeichnis je Projekt, Datenbank wird automatisch
  angelegt, root ohne Passwort), Mail-Catcher und GUI-Emulator.
- Jeder Dienst wird per TCP-Health-Check verifiziert, mit Retry und
  eigener Logdatei - kein blindes Warten.
- Neues unteres Panel mit den Reitern Konsole, Dienste und Mail.
- Der SMTP-Mail-Catcher steckt in der IDE selbst; eine erzeugte php.ini
  und ein sendmail-Ersatz sorgen dafuer, dass `mail()` ohne jede
  Konfiguration im Mail-Reiter landet.
- Zugangsdaten der Datenbank kommen per Umgebung zum Webserver, weil die
  Ports pro Sitzung frei gewaehlt werden.

## Live-Ansicht ist sofort da und folgt dem Tippen
- Die Vorschau oeffnet sich automatisch beim Oeffnen einer .ui- oder
  Web-Datei. Es muss nichts mehr "gestartet" werden.
- GTK `.ui` wird bei jedem Tastendruck direkt aus dem Editor-Puffer
  gezeichnet - ohne Speichern und ohne Build. Bei halbfertigem XML
  bleibt die letzte gueltige Ansicht stehen und ein Hinweisstreifen
  nennt den echten Grund (eigene Wohlgeformtheitspruefung, da
  GtkBuilder abgeschnittenes XML nicht meldet).
- HTML/CSS/JS werden ebenfalls live aus dem Puffer gerendert.
- Entprellte Kette beim Tippen: nach 250 ms Vorschau + Syntaxpruefung,
  nach 900 ms Auto-Save. Fehler werden damit WAEHREND des Tippens
  markiert und verschwinden wieder, sobald sie behoben sind.
- Auto-Save respektiert die Git-Zeitreise-Schreibsperre und laesst sich
  in den Einstellungen abschalten.

## Emulator
- Es wird `gtk4-broadwayd` gesucht (der Name unter Debian/Ubuntu);
  vorher wurde `broadwayd` gesucht und nie gefunden.
- Der Emulator wartet per Health-Check auf den Broadway-Server, statt
  blind nach 1300 ms zu laden - Ursache des schwarzen Fensters.
- Webprojekte landen nicht mehr im GUI-Emulator (weisses Fenster),
  sondern im Live-Browser. "Run" leitet selbst an die richtige Stelle.

## Eigene Konsole statt offener Bash
- Kein PTY und kein Shell-Prozess mehr: die Ausgabeflaeche nimmt keine
  Tastatureingaben an, es gibt keine Bash-History des Benutzers.
- Eigener IDE-Befehlssatz mit eigener History: build, run, emulator,
  services, restart <dienst>, url, db, mail, open, snapshot, errors,
  sandbox, clear, help.
- Rechtsklick: Auswahl oder alle Fehler an den Agenten senden.
  Build-Fehler gehen automatisch an den Agenten.

## Agent benutzt die Werkzeuge
- Der Ausweg "gib Code als Codeblock aus" ist aus dem System-Prompt
  entfernt; die Arbeitsweise ist jetzt verbindlich: lesen mit read_file,
  aendern mit replace_in_file/write_file, kein Code im Chat.
- Liefert der Agent trotzdem nur Prosa mit Codeblock, wird er einmal
  unmissverstaendlich aufgefordert und der Request wiederholt.
- Der Verlauf inklusive Werkzeugaufrufe und -ergebnisse ueberlebt die
  Nutzerrunde; das Kontextfenster bricht nie ein tool_calls/tool-Paar auf.
- Neues Werkzeug `project_status`: Projekttyp, Sandbox, laufende Dienste
  mit Ports, DB-Zugang, Mail-Catcher, offene Fehler.

## Wizard ist dynamisch
- Statisches C-Routing Sprache -> Templates: bei PHP gibt es kein GTK4
  mehr, bei Bash nur Console. Die Liste wird beim Sprachwechsel neu
  aufgebaut.
- Dienst-Optionen erscheinen nur, wo sie hingehoeren: bei Webprojekten
  eine MySQL/MariaDB-Option samt Erklaerung, was automatisch startet.
- Der System-Check fragt nur die wirklich noetigen Werkzeuge ab und
  prueft sie in der Sandbox.
- PHP+Web+DB-Template zeigt eine funktionierende mysqli-Verbindung.

## Behobene Fehler
- `sandbox_exec_argv` liess im lokalen Modus das `cd` ins Projekt weg;
  der Webserver lieferte dadurch auf jede Datei 404.
- Der MySQL-Socket lag im Projektordner und sprengte bei tiefen Pfaden
  das 107-Zeichen-Limit fuer Unix-Sockets; mariadbd brach ab.
- `g_subprocess_get_if_exited()` wurde ohne vorheriges `wait` aufgerufen
  (GLib-Assertion); das Prozessende wird jetzt per `wait_async` verfolgt.
- Der Teiler der Live-Vorschau wurde auf Breite 0 gesetzt, solange das
  Widget noch kein Layout hatte.
- Die Syntaxpruefung loeschte die Problemliste aller Dateien statt nur
  der geprueften.
- Emulator-Zustand lag in statischen Globals und ueberlebte den
  Projektwechsel.

# HyperCore Studio v1 (vormals Werkbank/Grundstein-Linie)

Vollstaendiges Rebrand auf **HyperCore Studio** (DATANET.OVH LTD), Format `.hcs`,
plus komplette v2-Architektur laut Masterplan:

- **Zero-Dependency-Monolith** via GResource (CSS/Icons/Templates einkompiliert).
- **Workspace** `~/HyperCore` + statisches C-Routing im Wizard.
- **Tresor-Whitelist** (`.hcs`): nur gelistete Dateien sind sichtbar.
- **Sandbox** (local/chroot/docker) + **Ladescreen & Inspektor** (rot/gruen,
  1-Klick-Install) + postinst-Container-Setup.
- **Nativer Emulator (Broadway)** + **Snapshot-Tool**.
- **KI-Agent v2**: Personas, Smart-Context, Graceful-Degradation,
  Tool-Transparenz, Auto-Crash-Hook (5 Werkzeuge, agentischer Loop).
- **RAG-Engine** (Ollama-Embeddings, Cosine + lexikalischer Fallback,
  Delta/GC/Rebuild, visueller Manager).
- **Native Source Control (libgit2)**: Auto-Versioning, Time Machine
  (Read-Only), Zurueck-zu-Latest, KI-Commit-Messages.
- **Global Identity** + **Strict Compliance Mode**.
- **Flat-Design**, eigenes Icon-Set, echtes HyperCore-Logo, erweitertes Ueber-Fenster.

## Fixes nach v1-Feedback
- Run läuft wieder zuverlässig & sauber: kein `cd`/Rohbefehl in der Konsole,
  freundliche Meldungen ("▶ Starte Programm …", "✔/✘ beendet"), Ausführung
  im Projektordner. Broadway-Emulator ist jetzt OPTIONAL (Menü Werkzeuge ->
  "Im Emulator starten"), nicht mehr der Standard-Run.
- RAG-Endlosschleife behoben (100% CPU ohne Ollama) -> sauberer Fallback.
- Generiertes GTK4-Programm findet window.ui relativ zur Binary (läuft auch
  aus bin/); build.sh legt window.ui neben die Binary.
- Logfile: ~/.cache/hypercore/hypercore.log (gefiltert auf Relevantes).
- Dynamische .deb-Abhängigkeiten unabhängig von ldconfig im PATH.
