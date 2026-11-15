# Stazione vento stampata 3D – anemometro a coppette + banderuola in linea

Colonna unica da infilare in cima al palo (Ø32). Dal basso: **base** → **rotore a coppe** → **testa** con il potenziometro → **banderuola**.
Circa 11 cm sopra il palo fino al mozzo, più la pinna della coda.

## File del progetto

| File | Cosa fa |
|---|---|
| `comune.py` | **Tutti i parametri** (dizionario `P`) e le funzioni di base: disegno, esportazione STL, anteprime |
| `stazione.py` | Disegna i pezzi → `stl/*.stl` e l'anteprima `assieme.png` |
| `sezione.py` | Sezione verticale con la minuteria → `sezione.png` |
| `dettaglio_reed.py` | Dettaglio della cava del reed e delle fessure per i reofori → `dettaglio_reed.png` |
| `inerzia.py` | Calcolo di massa e momento d'inerzia di un pezzo |

```
pip install manifold3d trimesh numpy matplotlib
python3 stazione.py      # STL + assieme.png
python3 sezione.py       # sezione.png
python3 dettaglio_reed.py  # dettaglio_reed.png
```

Per cambiare una misura modifica `P` in `comune.py` e rilancia `stazione.py`.
Da verificare sui tuoi componenti: `pot_*` (potenziometro), `palo_d`, `asse_d`, `magnete_*`, `reed_*`, `cusc_tol`.

## Come funziona

- **Tubo asse fisso Ø8** (8×1 inox/ottone/alluminio o carbonio 8×6), lungo ~48 mm: bloccato nella base con un grano e nella testa con due.
  Fa da perno al rotore e i fili del potenziometro ci passano dentro fino al palo.
- **Rotore leggero** (22 g stampati, ≈1050 g·cm²): mozzo minimo su **un solo 608 ceramico** che gira attorno al tubo.
  Il cuscinetto sta nel piano dei bracci, così la spinta del vento non crea momento ribaltante e ne basta uno.
  L'anello interno è serrato tra il collarino della base e quello della testa, senza precarico.
- **Coppa e braccio in un pezzo** (braccio a lama 7×3 mm, parete 1 mm): niente massa e viti al raggio massimo.
- **Conteggio giri**: 2 magneti 6×3 opposti nella faccia inferiore del mozzo (rotore bilanciato, **2 impulsi/giro**),
  reed in ampolla (~2×14 mm) nella faccia superiore della base, a ~3,5–4 mm dai magneti. I fili del reed scendono direttamente nel palo.
- **Banderuola senza cuscinetti propri**: il mozzo ha un foro cieco (chiuso in alto) calzato su 12 mm di albero del potenziometro, con un grano M3.
  Le boccole del pot reggono il carico: tienila **leggera e bilanciata** col contrappeso.
- **Pioggia**: la testa fa da ombrello sul mozzo del rotore, la base ha la faccia superiore smussata, il mozzo della banderuola ha una gonna sopra la piastra.

Ho verificato numericamente che rotore e banderuola non tocchino nulla a nessun angolo, anche fra loro.

## Pezzi da stampare (`stl/`)

| File | Q.tà | Orientamento di stampa |
|---|---|---|
| base.stl | 1 | come esce (innesto palo sul piatto) |
| rotore.stl | 1 | come esce (fondo sul piatto) |
| coppa_braccio.stl | 3 | bordo della coppa sul piatto, braccio piatto sul piatto; 2 perimetri |
| testa.stl | 1 | **capovolta** (alette sul piatto) |
| piastra_pot.stl | 1 | piatta |
| mozzo_banderuola.stl | 1 | come esce (faccia superiore sul piatto) |
| coda.stl | 1 | piatta |

Materiale: **PETG o ASA**. Base e testa 4 perimetri, 30% infill; rotore, coppe, mozzo banderuola e coda leggeri (2–3 perimetri, 10–15%; coda piena da 3 mm). Nessun supporto.

## Minuteria

- 1× cuscinetto 608 ceramico (8×22×7)
- tubo Ø8 × ~48 mm
- reed in ampolla ~14 mm (Ø ≤ 2,5) + 2 magneti al neodimio Ø6×3 assiali
- potenziometro 20 kΩ a rotazione continua (default bussola 3/8", albero 1/4"), con albero che sporga **20 mm** sopra la piastra
- 3× M3×12 + dadi (testa/piastra), 3× M3×10 autofilettanti (bracci), 4 grani M3 (1 base/tubo, 2 testa/tubo, 1 mozzo/albero pot), 2× M3×20 (coda), 1 vite M4 (palo)
- vite M8 + rondelle per il contrappeso

## Montaggio

1. Salda prima le prolunghe ai reofori. Reed nella cava della base, reofori piegati a 90° ad almeno 2 mm dal vetro e infilati nelle due fessure verticali (2,4×3,5 mm), che scendono dritte in una camera sotto il reed collegata al palo. Poi incolla il reed.
   Prima di incollarlo prova a passare un magnete a mano: se il reed chiude due volte per passaggio, approfondisci la cava o giralo in senso radiale.
2. Tubo nella base, grano.
3. 608 nel mozzo (da sotto, fino alla battuta), 2 magneti con la stessa polarità verso il basso, bracci-coppa negli attacchi (una vite ciascuno). Infila il rotore sul tubo fino al collarino.
4. Pot sotto la piastra con il dado sulla bussola; fili del pot dentro la testa e giù nel tubo; chiudi la testa con le 3 viti.
5. Infila la testa sul tubo finché il collarino tocca l'anello interno del 608 (il mozzo deve restare libero con ~2 mm sopra e sotto), stringi i grani.
6. Mozzo banderuola sull'albero del pot, grano; coda e contrappeso; bilancia finché resta ferma in qualsiasi posizione; allinea lo zero a Nord.

## Sensibilità al vento debole

- Per lo **spunto** conta soprattutto l'attrito: 608 ceramico **senza schermi e sgrassato**, nessun precarico, reed senza contatto.
- La **bassa inerzia** fa seguire le raffiche e impedisce al rotore di continuare a girare quando il vento cala.
  L'inerzia ormai è quasi tutta nelle coppe: `coppa_sp` 0,8 toglie un altro ~20%.
  Accorciare `rotore_r_coppa` riduce l'inerzia ma anche la coppia, quindi peggiora lo spunto.
- Con 2 magneti gli impulsi per giro sono 2: aggiorna la taratura (velocità vento ↔ giri).
