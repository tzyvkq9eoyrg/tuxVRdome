# tuxVRdome

Ein kleiner VR-Videoplayer für Linux (OpenXR + libmpv), getestet mit SteamVR und der Steam Frame.
Videos und Fotos laufen auf einer flachen oder gebogenen virtuellen Leinwand vor einem ruhigen
Nachthimmel oder als 180°/360°- bzw. Fisheye-Video, mono oder 3D. Dekodiert wird über NVDEC auf der
GPU (HEVC, AV1, VP9, H.264, bis 8K, auch HDR). YouTube (Suche, Links, Playlists) läuft über yt-dlp.

## Voraussetzungen

- Linux mit NVIDIA-Grafikkarte (NVDEC; ohne geht es mit `--hwdec=no` langsamer)
- SteamVR als aktive OpenXR-Runtime (SteamVR-Einstellungen → *OpenXR*)
- für YouTube: [yt-dlp](https://github.com/yt-dlp/yt-dlp), am besten aktuell über pipx:
  `pipx install yt-dlp` (aktualisieren: `pipx upgrade yt-dlp`)

## Installieren (AppImage)

```
chmod +x tuxVRdome-1.0.0-x86_64.AppImage
./tuxVRdome-1.0.0-x86_64.AppImage                 # startet im Dateibrowser
./tuxVRdome-1.0.0-x86_64.AppImage video.mkv       # spielt ein Video
```

Das AppImage braucht FUSE (`sudo apt install libfuse2t64`) und ein System, das mindestens so neu
ist wie das, auf dem es gebaut wurde (Ubuntu 26.04).

## Aus dem Quellcode bauen

Abhängigkeiten (Ubuntu 26.04):

```
sudo apt install build-essential cmake pkg-config libopenxr-dev libmpv-dev libglfw3-dev libgl-dev \
  libvulkan-dev libpango1.0-dev libcairo2-dev libcurl4-openssl-dev libgdk-pixbuf-2.0-dev \
  nlohmann-json3-dev libglib2.0-dev libavformat-dev libavcodec-dev libavutil-dev libswscale-dev
```

```
./build.sh                    # baut nach build/ und führt die automatischen Tests aus
./build/tuxvrdome [video]     # starten
sudo cmake --install build    # optional: installieren, mit Eintrag im Anwendungsmenü
./make-appimage.sh            # optional: das AppImage bauen
```

Optionen: `--hwdec=nvdec-copy` oder `--hwdec=no` (falls NVDEC Probleme macht), `--no-mirror` (kein
Vorschaufenster auf dem Desktop), `--timing` (Bildzeiten ausgeben), `--version`.

## Bedienung mit den Controllern

| Eingabe | Funktion |
|---|---|
| Trigger | Menü öffnen, im Menü klicken, daneben zielen = Menü schließen; auf der Zeitleiste halten und ziehen |
| Menütaste (links) | Menü öffnen/schließen |
| A (rechts) | Play / Pause |
| Obere Taste rechts | Ausrichten |
| Rechter Stick links/rechts | Spulen; gedrückt halten spult immer schneller (10 s → 30 s → 60 s) |
| Linker Stick links/rechts | Ansicht um 30° drehen |
| Stick hoch/runter | Lauter/leiser; in Listen stufenlos blättern (je weiter geneigt, desto schneller) |
| Grip halten | Leinwand bzw. Menü (wenn der Laser darauf zeigt) mit der Hand verschieben; dabei Stick hoch/runter = Leinwandgröße |

## Das Menü

**Player-Seite:** Zeitleiste mit Vorschaubild und Kapitelmarken, Wiedergabeknöpfe, Lautstärke,
Wiederholen (Video / Ordner bzw. Playlist; ein Abzeichen zeigt, was wiederholt wird), Geschwindigkeit,
A-B-Schleife, Schlaf-Timer. Unten Infos zum Video sowie Knöpfe für Kapitel, Tonspur, Untertitel
(auch aus einer Datei) und bei YouTube die Qualität. Links unten Dateien, darüber Favoriten, daneben
YouTube; rechts Ausrichten und Format. Oben links Löschen und der Favoriten-Stern für die laufende
Datei. Der Pfeil oben klappt das Menü zu einer schmalen Leiste zusammen. Wendet man sich mehr als
30° ab, gleitet das Menü hinterher (abschaltbar; von Hand verschoben behält es seine Höhe).

**Dateibrowser:** Liste oder Galerie mit Vorschaubildern, Sortierung nach Name, Datum oder Größe
(Ansicht und Sortierung werden pro Ordner gemerkt). Ordner und Dateien lassen sich per Stern zu den
Favoriten hinzufügen; der Stern-Knopf oben zeigt die Favoriten.

**Einstellungen:** Sprache (Deutsch/Englisch), Design (Transparent oder Material), Liegemodus,
Hintergrund, „Menü folgt Blick“; Unterseiten Bild (Helligkeit, Kontrast, Sättigung, Zoom),
Untertitel (Größe, Höhe, Abstand) und YouTube (Standardqualität).

## Formate

Das Format wird aus dem Dateinamen erkannt (z. B. `_180_SBS`, `_360_TB`, `FISHEYE190`, `MKX200`).
Ohne Kennzeichnung gilt: großes 2:1-Video = 180° 3D nebeneinander, großes 1:1-Video = 360° 3D
übereinander, großes 2:1-Foto = 360°-Panorama. Von Hand gewählte Formate werden pro Video gemerkt.
Bei YouTube entscheiden der Suchfilter (360°/VR180), YouTubes Formatangaben und ein Blick aufs Bild.

Bei 3D- und 180°/360°-Videos zeichnet der Player die Untertitel selbst als schwebende Tafel, die der
Kopfbewegung folgt. Bei 2D-Videos stehen sie im Bild, wahlweise auch schwebend.

## Liegemodus

„Ausrichten“ übernimmt dann die volle Neigung des Kopfes: im Liegen an die Decke schauen und
ausrichten – Leinwand, Video, Untertitel und Menü liegen dann dort.

## Tastatur (Desktop-Fenster im Fokus)

| Taste | Funktion |
|---|---|
| Tab | Menü öffnen/schließen |
| Leertaste | Play / Pause |
| ← / → | Spulen |
| A / D | Ansicht links / rechts drehen |
| ↑ / ↓ | Nächstes / vorheriges Video im Ordner |
| + / − , M | Lautstärke, Stumm |
| P / L / C | Projektion / 3D-Anordnung / gebogene Leinwand umschalten |
| W / S, Q / E | Leinwand näher/weiter, kleiner/größer |
| R | Ausrichten |
| F | Video vertikal spiegeln |
| Esc, Esc | Beenden |

## Gespeicherte Daten und Privatsphäre

Alles liegt in `~/.config/tuxvrdome/`:

- `settings.conf` – Einstellungen
- `videos.conf`, `folders.conf` – pro Video bzw. Ordner Gemerktes (Format, Bild, Lautstärke,
  Sortierung); nur Prüfsummen der Pfade bzw. YouTube-IDs, keine Datei- oder Ordnernamen
- `favorites.conf` – die Favoriten, als Pfade (anders lassen sie sich nicht wiederfinden)

Vorschaubilder werden nur im Speicher gehalten. In der Konsolenausgabe werden Dateipfade unkenntlich
gemacht; Videotitel und -beschreibungen werden nicht ausgegeben.

Gelöschte Videos landen im Papierkorb, wo es einen gibt; auf Netzlaufwerken warnt der Dialog, dass
endgültig gelöscht wird.

## Lizenz

GPL-3.0 – siehe [LICENSE](LICENSE).
