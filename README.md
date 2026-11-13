# Sculpt Pad 0.1.0

Strumento virtuale stereo con la stessa interfaccia della famiglia Sculpt: una
manopola Morph, volume, VU e quattro indicatori round robin.

| Morph | Carattere |
| --- | --- |
| 0% | Archi morbidi, da registrazioni reali di violoncelli e violini |
| 33,3% | Ensemble caldo: archi con un sostegno sintetico discreto |
| 66,7% | Pad analogico, con oscillatori leggermente scordati e filtro in movimento |
| 100% | Pad digitale arioso, con armoniche e movimento stereo lento |

Tutte le posizioni intermedie sono utilizzabili. Il passaggio funziona anche
sugli accordi già premuti: i quattro timbri continuano ad avanzare durante il
morph. Attacchi morbidi, sustain continuo e rilascio graduale sono incorporati.
Non servono altri controlli per costruire il suono.

## File per avviare la compilazione

Il workflow è già compreso nel progetto:

```
.github/workflows/build-macos-vst3.yml
```

La cartella `.github` comincia con un punto. Il percorso deve partire dalla
**radice del repository**, sullo stesso livello di `CMakeLists.txt`, `Source`,
`Samples` e `tools`. Carica il contenuto di questa cartella di progetto nella
radice del repository. Caricare soltanto lo ZIP su GitHub non lo estrae e non
avvia la compilazione.

Il workflow è configurato per ogni push, per le pull request e per l'avvio
manuale. Dopo aver caricato i file, apri **Actions → Build macOS VST3**.
Per avviarlo a mano, il workflow deve essere presente nel branch predefinito:
seleziona **Run workflow** e conferma il branch.

Se il caricamento dal browser omette `.github`, usa **Add file → Create new
file**, inserisci il percorso completo sopra e incolla il contenuto del file
`build-macos-vst3.yml` fornito anche separatamente. Salva il commit.

Una compilazione riuscita pubblica l'artefatto **Sculpt-Pad-macOS-VST3**. Apri
l'esecuzione completata, scarica l'artefatto ed estrai gli ZIP fino al bundle
**Sculpt Pad.vst3**. Il workflow non installa il plug-in sul tuo computer.

Questo pacchetto contiene sorgenti, campioni e configurazione di compilazione.
Il VST3 macOS viene prodotto dall'esecuzione del workflow: non è un binario già
compilato o già verificato in una DAW. Le verifiche locali eseguite sono riportate
in `docs/VERIFICA.txt`.

Documentazione GitHub per l'avvio manuale:
https://docs.github.com/en/actions/how-tos/manage-workflow-runs/manually-run-a-workflow

## Installazione dopo la compilazione

Copia il bundle completo `Sculpt Pad.vst3` in:

```
~/Library/Audio/Plug-Ins/VST3/
```

Quindi riavvia la DAW o esegui una nuova scansione dei plug-in. La build è
universale Apple Silicon/Intel, con destinazione macOS 11 o successivo. Il
bundle usa una firma locale ad hoc; non è notarizzato. Il progetto genera VST3,
non AU o AAX.

## Anteprima audio

`Preview/SculptPad-demo.wav` è un render di 68 secondi prodotto direttamente
da `Source/PadEngine.h`, con la stessa banca incorporata dal plug-in. Non usa
riverbero esterno, effetti aggiunti o normalizzazione diversa per ogni timbro.

| Tempo | Ascolto |
| --- | --- |
| 0:00–0:12 | Archi, 0% |
| 0:12–0:24 | Ensemble, 33,3% |
| 0:24–0:36 | Analog, 66,7% |
| 0:36–0:48 | Digital, 100% |
| 0:48–1:08 | Un accordo tenuto, dal naturale al digitale, poi rilascio |

I primi quattro esempi ripetono gli stessi due accordi alla stessa velocity.
Nell'ultimo esempio le note vengono rilasciate con il pedale premuto: il morph
continua sull'accordo sostenuto; il pedale viene rilasciato a 1:03.

## Comportamento musicale

- 32 voci principali, con otto brevi code dedicate ai cambi di voce a piena polifonia.
- Velocity, pedale sustain CC64 e pitch bend ±2 semitoni dal plug-in.
- MIDI su 16 canali; sustain e pitch bend sono separati per canale.
- Morph e volume automatizzabili e salvati nello stato del progetto della DAW.
- Quattro variazioni round robin automatiche per nota, senza controlli aggiuntivi.
- Il rilascio raggiunge circa −60 dB in 2,4 secondi e termina entro circa 4,8
  secondi. Al programma ospite viene dichiarata una coda massima di 6 secondi.

La banca acustica contiene **una registrazione per nota fondamentale**. I
quattro round robin sono variazioni modellate di attacco, tono e fase intorno
alle registrazioni e alle componenti sintetiche: non sono quattro riprese
acustiche differenti. L'intonazione della registrazione non viene cambiata dal
round robin.

## Sorgente acustica

16 registrazioni stereo di sezioni di archi di VSCO 2 Community Edition,
con fondamentali tra MIDI 36 e 86. Le altre note utilizzano il campione più
vicino, con trasposizione filtrata. I loop del sustain hanno sovrapposizioni
graduali, senza interrompere gli accordi lunghi.

Le registrazioni sono distribuite dalla sorgente originale con licenza CC0.
Crediti, licenza integrale e modifiche ai campioni sono conservati in
`ThirdParty/VSCO/`. I dettagli verificabili di ogni file sono in
`Samples/source-info.json`.

Fonti:
https://github.com/sgossner/VSCO-2-CE
https://versilian-studios.com/vsco-community/

## Compilazione locale su Mac

Servono Xcode Command Line Tools e CMake 3.22 o successivo. Dalla cartella
che contiene questo README:

```bash
bash tools/build-macos.sh
```

Lo script recupera JUCE 8.0.4 e produce `dist/Sculpt-Pad-macOS-VST3.zip`.
Incorpora i campioni, verifica le architetture `arm64` e `x86_64`, copia i
crediti nel bundle e applica la firma ad hoc. JUCE e i suoi termini di licenza
sono identificati in `ThirdParty/JUCE-8.0.4-LICENSE.md`.

Per riutilizzare una copia locale di JUCE 8.0.4:

```bash
SCULPT_JUCE_SOURCE_DIR=/percorso/JUCE bash tools/build-macos.sh
```

## Verifica del motore e rigenerazione dell'anteprima

I test del nucleo DSP richiedono un compilatore C++17, senza JUCE:

```bash
bash tools/test-core.sh
```

Per rigenerare il render dalla stessa banca:

```bash
mkdir -p build-preview
c++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror tools/render_preview.cpp -o build-preview/render_preview
build-preview/render_preview Samples Preview/SculptPad-demo.wav
```

L'analisi numerica usa Python, NumPy e SciPy:

```bash
python3 tools/analyse_preview.py Preview/SculptPad-demo.wav --output docs/preview-metrics.json
```

I campioni necessari sono già inclusi. `tools/import_factory_samples.py` è
fornito per riprodurre la preparazione delle sorgenti e non viene eseguito
dalla compilazione ordinaria.
