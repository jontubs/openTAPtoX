# TAP-Protokoll: öffentlicher Arbeitsstand

Dieses Dokument beschreibt nur den verallgemeinerten Protokollstand von openTAPtoX. Reale Busmitschnitte, Gerätekennungen, Funkprofile, Schlüssel, Join-Seeds und zeitbezogene Anlagendaten gehören nicht in das öffentliche Repository.

Das Projekt ist inoffiziell und nicht mit Tigo verbunden. Die beschriebenen Abläufe beruhen auf Reverse Engineering und können unvollständig sein.

## Systemgrenze

```text
ESP32 <-> RS485-Transceiver <-> TAP <-> Funknetz <-> Optimierer
  |
  +-> Weboberfläche / MQTT / Home Assistant
```

Der ESP ersetzt die CCA auf der TAP-seitigen RS485-Verbindung. Er liest Zustände und Telemetrie, verwaltet die lokale Zuordnung erkannter Optimierer und kann ausgewählte TAP-Kommandos auslösen. Eine TAP-Antwort bestätigt nur die TAP-seitige Verarbeitung; sie beweist keine erfolgreiche Funkübertragung oder elektrische Freigabe des Optimierers.

## Transport

- RS485, 38400 Baud, 8N1
- Binäre Frames mit Start- und Endmarkierung
- 16-Bit-Zieladresse und 16-Bit-Typcode in Big Endian
- Nutzdaten variabler Länge
- CRC16 am Frame-Ende
- Das Richtungsbit der Adresse kennzeichnet TAP-zu-Host-Frames; die übrigen Bits bilden die Kurzadresse.

Vereinfachtes Format:

```text
start | address:u16be | type:u16be | payload:n | crc16:u16le | end
```

Der Decoder unter `tools/decoder/` prüft Framing und CRC, bevor er Nutzdaten interpretiert.

## Regulärer Pollbetrieb

Der Controller fragt den TAP mit `0x0148` ab. Der TAP antwortet mit `0x0149`. Die Antwort kann lediglich Status und den Empfangscursor enthalten oder zusätzlich gepufferte Funkpakete liefern.

Wichtige Paketfamilien innerhalb von `0x0149`:

| Pakettyp | Bedeutung |
| --- | --- |
| `0x09` | Topologie-/Join-Hinweis eines Optimierers |
| `0x27` | Knotentabelle bzw. Identitätszuordnung |
| `0x31` | Leistungs- und elektrische Telemetrie |

Ein leerer `0x0149`-Frame ist ein Lebenszeichen des TAP, aber kein Beleg für Optimierer-Telemetrie.

## Aktive Kommandos

Viele Managementfunktionen werden als Anfrage `0x0B0F` und Antwort `0x0B10` transportiert. Der innere Subcommand bestimmt die Operation.

| Subcommand | Verallgemeinerte Funktion |
| --- | --- |
| `0x06` | Text-/Versionsabfrage |
| `0x0D` | Funkprofil lesen oder schreiben |
| `0x17` | knotenbezogenes Kommando |
| `0x22` | PV-/RSD-Zustand steuern |
| `0x26` / `0x27` | Knotentabelle anfordern / lesen |
| `0x29` | Knotentabelle schreiben |
| `0x2B` | Tabellen-/Netzwerkzustand zurücksetzen |
| `0x2D` / `0x2F` | Lernfenster steuern / Netzwerkstatus lesen |
| `0x41` | profilgebundenes Join-Material |

Schreibende Befehle sind zustandsbehaftet und teilweise destruktiv. Die Firmware verlangt für riskante Web-Endpunkte eine explizite Bestätigung und prüft nach Adress-, Tabellen- oder Profiländerungen den gelesenen Zustand.

## Identitäten und Knotentabelle

TAP und Optimierer besitzen lange Hardwareadressen. Diese sind installationsbezogene Identifikatoren und werden in öffentlichen Beispielen ausschließlich synthetisch dargestellt. Die TAP-Knotentabelle ordnet einer langen Adresse eine kurze bzw. rohe Node-ID zu.

Bei der Inbetriebnahme kann Bit 15 einer rohen Node-ID einen noch nicht bestätigten Eintrag kennzeichnen. Ein sicherer Ablauf unterscheidet daher:

1. Identität als ausstehend eintragen.
2. Tabelle zurücklesen und exakt prüfen.
3. Lernfenster starten.
4. Auf echte Funk-Topologie des passenden Geräts warten.
5. Nur den per Funk belegten Eintrag bestätigen.
6. Reporting konfigurieren und auf echte `0x31`-Pakete warten.

Ein ACK auf einen Tabellenschreibbefehl reicht nicht als Erfolgsnachweis.

## Funkprofil und Join-Material

Funkdeskriptoren, Netzwerk-Key-Material und `0x41`-Join-Seeds können profil- oder installationsgebunden sein. openTAPtoX darf solches Material nur lokal aus dem aktuell verbundenen System lernen und zusammen mit der passenden TAP-Identität bzw. dem passenden Profil verwenden. Das Repository enthält keine realen Werte und liefert keine universellen Seeds aus.

Material eines fremden Profils darf nicht testweise auf eine Anlage übertragen werden. Neben Datenschutzrisiken kann dies einen funktionierenden Funkzustand beschädigen.

## Sicherer Zustandsautomat

Der normale Start bevorzugt einen passiven Warm-Attach:

```text
passiv lauschen
  -> bekannte Kurzadresse prüfen
  -> Version, Tabelle und Cursor lesen
  -> regulär pollen
```

Adressvergabe, Enumeration, Profiländerung, Tabellenneubau und RF-Recovery sind getrennte, explizite Pfade. Nach einer Mutation wird der Zielzustand zurückgelesen. Bei Abweichung bricht die Firmware ab und versucht keine unbeschränkte Folge weiterer Schreibbefehle.

## Erfolgskriterien

Die Ebenen müssen getrennt bewertet werden:

1. **RS485:** gültige TAP-Antwort mit korrekter CRC.
2. **TAP-Zustand:** erwartete Adresse, Tabelle und Netzwerkstatus wurden zurückgelesen.
3. **Funk:** passende reale Topologie- oder Telemetriepakete sind eingetroffen.
4. **Elektrik:** frische Spannungs-/Leistungsdaten belegen den gewünschten Zustand.

Ein Erfolg auf einer früheren Ebene beweist keine spätere Ebene.

## Lokale Analysewerkzeuge

Die Werkzeuge unter `tools/decoder/` können lokale Captures decodieren, Sitzungen zusammenfassen, zwei Traces vergleichen und kontrollierte Replay-Pläne erzeugen. Capture-Dateien und erzeugte Reports bleiben außerhalb von Git. Hinweise zum sicheren Teilen stehen in [`PRIVACY.md`](../PRIVACY.md).

Offene technische Fragen sollten mit kleinen synthetischen Fixtures oder mit lokal aufbewahrten, vollständig kontrollierten Mitschnitten untersucht werden. Reale Geräte- oder Haushaltsdaten sind kein zulässiges Testfixture für das öffentliche Repository.
