# RSC NFC Comparison – használati útmutató

A program beolvas egy NFC-kártyát, majd egy második beolvasás után megnézi, hogy
a második olvasás adatai **100%-ig** megegyeznek-e az elsőével. Összeveti a
kártya típusát, az UID-t és minden olyan bájtot, amelyet a kártya kulcs nélkül
kiad.

Mire jó:

- ellenőrizni, hogy egy kártya minden olvasáskor ugyanazt adja-e,
- egy másolatot összevetni az eredetivel,
- több kártyát egymás után összevetni ugyanazzal a mintakártyával.

A kinézete a többi RSC-programéval egyezik, és ugyanaz az „R” ikonja.

## Telepítés

A kész `.fap` fájl az `/ext/apps/NFC/` mappába kerül. A Flipperen ezután itt
találod: **Apps → NFC → RSC NFC Comparison**.

Fordítás és feltöltés a gépről (a `!` miatt a `~\flipper` junction-ön át):

```powershell
cd $HOME\flipper\RSC-NFC-Comparison
ufbt launch
```

## Nyelv

Telepítés után angol. A **Settings** első sora a **Language**: itt állítható
a **Magyar** (vagy a **Deutsch**). Ez a sor minden nyelven *Language* néven
szerepel, így akkor is megtalálható, ha véletlenül olyan nyelvet választasz,
amelyet nem olvasol. (A képernyőn nincsenek ékezetek, mert a Flipper betűkészlete
nem tartalmazza őket.)

## Használat

A főképernyőn felül a program neve, alatta egy nagy **Inditas** gomb, alatta
kisebb sorok. **↑/↓** választ, **OK** megnyit, **Vissza** kilép.

1. **Inditas**: megnyílik az **1. olvasas** képernyő. Tartsd a kártyát a Flipper
   hátuljához. A keretben *Kartyara var* áll, amíg kártyát keres; olvasás közben
   kitöltött lesz (*Olvasas...*). **OK** vagy **Vissza**: megszakítás.
2. **1. olvasas OK**: megmutatja a kártya típusát és UID-ját. Ha készen állsz,
   nyomd meg a **→** (*2. olvasas*) gombot. Hagyhatod a helyén ugyanazt a
   kártyát, vagy odatarthatsz egy másikat.
   Ha be van kapcsolva az **Auto 2. olv.** beállítás, ez a képernyő visszaszámol
   (*2. olvasas: 3 mp*), és a 2. olvasás magától indul. **→**: azonnal indítja,
   **←** / **Vissza**: megszakítja.
3. **2. olvasas**: ugyanúgy, mint az 1. lépés. A keret alatt az első kártya
   típusa és UID-ja látszik.
4. **Eredmény**:
   - **100% EGYEZIK** (kitöltött felirat, zöld LED): minden beolvasott adat
     azonos.
   - **ELTERO** (keretes felirat, piros LED): legalább egy bájt eltér. A sorok
     megmutatják, hol:
     - **Tipus**: protokoll és kártyatípus (pl. NTAG215).
     - **UID**: a kártya azonosítója.
     - **Adat**: minden más, amit a kártya kiadott. Akkor is összeveti, ha az
       UID eltér, így látszik, hogy „ugyanaz a tartalom, más UID” vagy „más a
       tartalom”. Mifare Ultralight / NTAG kártyánál azt is kiírja, *mi* tér el:
       hány lap, vagy *szamlalo*, *alairas*, *verzio*. A *nem vetheto* azt
       jelenti, hogy a két kártya más típusú, nem lehet összevetni.
   - **↑/↓**: második oldal. Mindkét olvasás (típus + UID), és hogy ebben a
     menetben hány 2. olvasás egyezett az elsővel.
   - **→ Tovabb**: újabb kártya összevetése **ugyanazzal** az 1. olvasással
     (több kártya ellenőrzéséhez).
   - **OK Uj**: újrakezdés egy új 1. olvasással.
   - **← Menu** / **Vissza**: vissza a főképernyőre.

Egy 1. olvasás után a főképernyőn megjelenik az **Osszevetes az 1.-vel** sor
is: ezzel új 2. olvasást indítasz a megőrzött 1. olvasáshoz. A legalsó sor
mutatja, melyik kártya az.

Ha egy olvasás nem sikerül (pl. túl korán elvetted a kártyát), rövid ideig
*Olvasasi hiba* látszik, és a program újra próbálkozik az odatartott kártyával.

## Beállítások

- **Language**: English (alapértelmezett) · Magyar · Deutsch.
- **Auto 2. olv.** (automatikus 2. olvasás): Ki (alapértelmezett) · 1 s · 2 s ·
  3 s · 5 s. Bekapcsolva az 1. olvasás után nem kell gombot nyomni: az
  *1. olvasas OK* képernyő a beállított ideig látszik, utána magától indul a
  2. olvasás. A várakozás alatt kicserélheted a kártyát, így nem ugyanazt a
  kártyát olvassa be rögtön újra. Csak az 1. olvasás után működik. Az eredmény
  képernyőn a **→ Tovabb** továbbra is kézzel indítja a következő kártyát.
- **LED**: Ki / Be. Olvasás közben ciánkék villogás, utána zöld (egyezik) vagy
  piros (eltér), amíg az eredmény látszik.
- **Rezges**: Ki / Be. Egy rezgés sikeres olvasáskor és egyezéskor, kettő
  eltéréskor.
- **Hang**: Ki / Be. Rövid csippanás az 1. olvasáskor, két magas hang
  egyezéskor, egy hosszú mély hang eltéréskor.
- **Verzio**: a program verziója (csak olvasható).

A beállításokat a program megjegyzi
(`/ext/apps_data/rsc_nfc_comparison/settings.bin`).

## Mit vet össze?

A program a firmware saját NFC-olvasóit használja (ugyanazt az olvasást, mint a
beépített NFC alkalmazás), és a két eredményt mezőről mezőre veti össze.
**Kulcsot vagy jelszót soha nem próbál.**

| Kártya | Mit olvas be és vet össze |
| --- | --- |
| Mifare Ultralight, NTAG21x | minden jelszó nélkül olvasható lap, verzió, aláírás, számlálók |
| ISO15693 / SLIX (ICODE) | UID, rendszeradatok, jelszó nélkül olvasható blokkok |
| DESFire, Mifare Plus, NTAG4xx, Type 4 Tag | amit a kártya azonosítás nélkül kiad |
| FeliCa, ST25TB, ISO14443-3A/3B/4A/4B | a szabadon olvasható adatok |
| **Mifare Classic** | **csak UID, ATQA, SAK.** A szektorokhoz kulcs kell. A típus mellett `*` áll |
| Bankkártya (EMV) | csak az ISO14443-4A szint (UID, ATQA, SAK, ATS) |

Tudnivalók:

- Azoknál a kártyáknál, amelyek minden olvasáskor véletlen UID-t adnak
  (telefon, egyes bank- és DESFire-kártyák), mindig **UID: ELTER** lesz. Ez
  helyes eredmény.
- Azok az NTAG21x chipek, amelyeken be van kapcsolva az NFC-számláló, minden
  olvasáskor eggyel továbblépnek. Ugyanannak a kártyának két olvasása ilyenkor a
  számlálóban eltér (**Adat: ELTER (szamlalo)**). Ez is helyes eredmény.
