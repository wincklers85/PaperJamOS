# PaperJam OS

**PaperJam OS v0.0.1-alpha** è il primo firmware/OS touch per **M5Stack M5Paper di prima generazione (ESP32, 540×960 e-paper)** con supporto iniziale al modulo **PN532 NFC**.

## Stato della v0.0.1-alpha

Questa prima alpha include:

- splash screen grafico "PaperJam OS"
- versione firmware visibile
- boot log stile Linux con percentuale di caricamento
- interfaccia chiara/bianca pensata per e-paper
- framebuffer a schermo intero 540×960
- refresh completo GC16 ai cambi pagina per ridurre schermate incomplete/ghosting
- status bar con:
  - ora RTC
  - stato Wi-Fi
  - stato Bluetooth
  - stato NFC
  - batteria
- swipe down dalla parte alta dello schermo per aprire Quick Settings
- toggle touch Wi-Fi / Bluetooth / NFC
- Home con una prima app: **NFC**
- app NFC con:
  - scansione ISO14443A
  - UID/seriale
  - ATQA
  - SAK
  - classificazione indicativa MIFARE Classic / Ultralight-NTAG / DESFire-ISO14443-4 / Plus e tag ISO14443A generici
  - log anche su Serial USB
- sleep tramite tasto centrale/PWR dell'M5Paper
- wake tramite il normale pulsante di alimentazione
- tasti laterali già acquisiti dall'OS per estensioni future

## Hardware target

Solo **M5Paper prima generazione**, non M5Paper S3.

Pin hardware M5Paper usati dal progetto:

| Funzione | GPIO |
|---|---:|
| E-paper / SD SPI | 12, 13, 14, 15 |
| Touch / RTC I²C interno SDA | 21 |
| Touch / RTC I²C interno SCL | 22 |
| Tasto destro | 37 |
| Tasto centrale / PWR | 38 |
| Tasto sinistro | 39 |

## PN532 su Port B

Cablaggio richiesto:

| PN532 | M5Paper Port B |
|---|---|
| GND | GND |
| VCC | 5V |
| SCL | GPIO 26 |
| SDA | GPIO 33 |

> Nota: in questa revisione di prova il PN532 è collegato al **Port B** e usa un **secondo bus I²C hardware** con SDA su GPIO33 e SCL su GPIO26. Il bus I²C interno dell'M5Paper resta separato, così touch, RTC ed EEPROM non vengono disturbati.

Il modulo PN532 deve essere configurato in **modalità I²C** tramite i relativi switch/jumper. Al boot PaperJam OS esegue anche una scansione del bus I²C2 e stampa sul Serial Monitor gli indirizzi trovati, utile per diagnosticare il collegamento.

## Compilazione con PlatformIO

1. Installa Visual Studio Code + PlatformIO.
2. Clona la repository:
   ```bash
   git clone https://github.com/wincklers85/PaperJamOS.git
   cd PaperJamOS
   ```
3. Collega M5Paper via USB-C.
4. Compila:
   ```bash
   pio run
   ```
5. Carica:
   ```bash
   pio run -t upload
   ```
6. Monitor seriale:
   ```bash
   pio device monitor
   ```

## Uso

- All'avvio compare il boot di PaperJam OS.
- Tocca l'icona **NFC** per aprire il lettore.
- Avvicina una card/tag PN532 per visualizzare UID, tecnologia stimata, ATQA e SAK.
- Fai **swipe verso il basso dalla status bar** per aprire i Quick Settings.
- Tocca Wi-Fi, BT o NFC per abilitarli/disabilitarli.
- Premi il tasto centrale/PWR per mandare il dispositivo in sleep/shutdown.
- Il display e-paper conserva l'ultima schermata anche senza refresh continuo.

## Nota sul riconoscimento dei tag

Il PN532 fornisce direttamente UID e parametri ISO14443A come ATQA/SAK. PaperJam OS usa ATQA/SAK e lunghezza UID per classificare il tag. Alcune famiglie (in particolare MIFARE Plus in determinati security level o cloni) possono condividere valori SAK con altre famiglie: in questi casi l'app mostra una classificazione prudente invece di dichiarare un modello non verificabile.

## Roadmap breve

- v0.0.2-alpha: lettura NDEF e dump sicuro di memoria leggibile
- gestione impostazioni persistenti
- migliore gestione power/sleep
- notifiche di sistema
- file/app framework
- Wi-Fi manager
- Bluetooth manager

## Licenza

MIT.
