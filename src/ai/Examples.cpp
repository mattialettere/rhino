// Esempi completi per l'assistente AI (piano + comandi di ogni parte). Nelle istruzioni
// entrano solo i due piu' vicini alla richiesta (SelectExamples). I test eseguono ogni
// esempio e controllano che ogni parte stia nel suo ingombro del piano.
#include "ai/Assistant.h"

namespace cad::ai {

const std::vector<Example>& Examples() {
  static const std::vector<Example> ex = {
      {"tavolo", "tavol table scrivani desk banco",
       "un tavolo 120x80 alto 75 cm",
       R"(# Table 1200x800, 750 high (cm -> mm): a top on four legs at the corners, standing on z = 0
PART Top | flat top 1200x800, 40 thick: _Box | 0,0,710 1200,800,750
PART Legs | four square legs 60x60 under the corners: _Box, then _Copy | 40,40,0 1160,760,712
)",
       {{"Top", R"(_Box 0,0,710 1200,800 40 => Top
_SetObjectColor 190,140,90 @Top)"},
        {"Legs", R"(_Box 40,40,0 100,100 712 => Legs
_Copy 0,0,0 1060,0,0 1060,660,0 0,660,0 _Enter @Legs => Legs
_SetObjectColor 150,100,60 @Legs)"}}},

      {"sedia", "sedi chair sgabell stool poltron",
       "una sedia",
       R"(# Chair 450x450: seat at 450, four legs, backrest up to 900 along the back edge (y = 450)
PART Seat | seat board 450x450, 30 thick: _Box | 0,0,420 450,450,450
PART Legs | four legs 40x40 under the corners: _Box, then _Copy | 0,0,0 450,450,425
PART Backrest | two rear posts up to 900 and a back panel between them: _Box | 0,410,445 450,450,900
)",
       {{"Seat", R"(_Box 0,0,420 450,450 30 => Seat)"},
        {"Legs", R"(_Box 0,0,0 40,40 425 => Legs
_Copy 0,0,0 410,0,0 410,410,0 0,410,0 _Enter @Legs => Legs)"},
        {"Backrest", R"(_Box 0,410,445 40,450 455 => Backrest
_Box 410,410,445 450,450 455 => Backrest
_Box 40,420,650 410,445 250 => Backrest)"}}},

      {"lampada", "lampad lamp abat lume luce",
       "una lampada da tavolo ad arco",
       R"(# Arc desk lamp about 450 high: heavy base disc, bent stem rising forward (+Y), shade hanging at its end
PART Base | disc 220 wide, 20 thick: _Cylinder | -110,-110,0 110,110,20
PART Stem | bent tube along a smooth path: _PipeThrough | -8,-8,10 8,308,448
PART Shade | cone-shaped shade at the end of the stem: _Lathe | -90,210,330 90,390,450
)",
       {{"Base", R"(_Cylinder 0,0,0 110 20 => Base
_SetObjectColor 40,40,40 @Base)"},
        {"Stem", R"(_PipeThrough 8 0,0,10 0,40,250 0,160,430 0,300,440 _Enter => Stem)"},
        {"Shade", R"(_Lathe 0,300,330 90,0 70,50 30,105 10,120 _Enter => Shade
_SetObjectColor 230,200,60 @Shade)"}}},

      {"tazza", "tazz mug cup bicchier glass caraff",
       "una tazza",
       R"(# Mug 84 wide and 95 high, standing on z = 0, handle on the +X side
PART Body | hollow round cup: _Lathe outside, _Lathe inside, _BooleanDifference | -42,-42,0 42,42,95
PART Handle | curved handle on the side: _PipeThrough | 32,-6,14 74,6,81
)",
       {{"Body", R"(_Lathe 0,0,0 38,0 40,50 42,95 _Enter => Body
_Lathe 0,0,6 34,0 36,50 38,95 _Enter => Inside
_BooleanDifference @Body @Inside => Body
_SetObjectColor 220,90,60 @Body)"},
        {"Handle", R"(_PipeThrough 6 38,0,75 62,0,72 68,0,48 60,0,24 38,0,20 _Enter => Handle
_SetObjectColor 220,90,60 @Handle)"}}},

      {"bottiglia", "bottigli bottle vino wine birra beer flacon ampoll vaso vase",
       "una bottiglia di vino",
       R"(# Wine bottle 75 wide and 300 high, standing on z = 0, turned around the vertical axis
PART Bottle | glass bottle: radius,height profile turned with _Lathe | -37.5,-37.5,0 37.5,37.5,300
PART Cork | cork in the neck: _Cylinder | -10,-10,290 10,10,315
PART Label | paper label around the body: _Tube | -38.5,-38.5,60 38.5,38.5,160
)",
       {{"Bottle", R"(_Lathe 0,0,0 37.5,0 37.5,190 34,215 15,250 13,290 15,300 _Enter => Bottle
_SetObjectColor 30,90,40 @Bottle)"},
        {"Cork", R"(_Cylinder 0,0,290 10 25 => Cork
_SetObjectColor 190,150,100 @Cork)"},
        {"Label", R"(_Tube 0,0,60 38.5 37.6 100 => Label
_SetObjectColor 240,235,215 @Label)"}}},

      {"chitarra", "chitarr guitar basso bass ukulel mandolin liuto strument corde string",
       "una chitarra acustica",
       R"(# Acoustic guitar lying on its back, length along Y (970), body 490 long and 380 wide, 100 deep, top at z = 100
PART Body | body: symmetric outline with _SymCurve, _ExtrudeCrv 100, round sound hole cut into the top | -190,0,0 190,490,100
PART Neck | rounded neck along Y: _SectionLoft | -28,470,64 28,790,100
PART Fingerboard | dark tapered board on the neck, reaching the sound hole: _Plate | -28,400,100 28,790,106
PART Head | flat headstock with three tuning pegs on each side: _Box, _PipeThrough | -60,780,78 60,970,94
PART Bridge | bridge glued on the top: _Box | -80,140,100 80,170,110
PART Strings | six thin strings from the bridge to the nut: _PipeThrough | -25,150,106 25,790,113
)",
       {{"Body", R"(_SymCurve 0,490 75,484 130,462 145,425 140,385 122,345 120,310 135,270 165,225 185,170 190,110 178,55 140,15 75,1 0,0 _Enter => Outline
_ExtrudeCrv _Solid=_Yes 100 @Outline => Body
_Delete @Outline
_Cylinder 0,340,92 45 20 => Hole
_BooleanDifference @Body @Hole => Body
_SetObjectColor 205,155,95 @Body)"},
        {"Neck", R"(_SectionLoft 0,470,82 56 36 0,620,83 52 34 0,790,84 46 32 _Enter => Neck
_SetObjectColor 120,70,30 @Neck)"},
        {"Fingerboard", R"(_Plate 6 -22,790,103 22,790,103 28,400,103 -28,400,103 _Close => Fingerboard
_SetObjectColor 40,30,25 @Fingerboard)"},
        {"Head", R"(_Box -45,780,78 45,970 16 => Head
_PipeThrough 4 -60,820,86 60,820,86 _Enter => Head
_PipeThrough 4 -60,870,86 60,870,86 _Enter => Head
_PipeThrough 4 -60,920,86 60,920,86 _Enter => Head
_SetObjectColor 120,70,30 @Head)"},
        {"Bridge", R"(_Box -80,140,100 80,170 10 => Bridge
_SetObjectColor 40,30,25 @Bridge)"},
        {"Strings", R"(_PipeThrough 0.7 -25,155,111 -18,788,108 _Enter => Strings
_PipeThrough 0.7 -15,155,111 -10.8,788,108 _Enter => Strings
_PipeThrough 0.7 -5,155,111 -3.6,788,108 _Enter => Strings
_PipeThrough 0.7 5,155,111 3.6,788,108 _Enter => Strings
_PipeThrough 0.7 15,155,111 10.8,788,108 _Enter => Strings
_PipeThrough 0.7 25,155,111 18,788,108 _Enter => Strings
_SetObjectColor 215,215,205 @Strings)"}}},

      {"violino", "violin viola violoncell cello contrabbass arco bow",
       "un violino",
       R"(# Violin lying on its back, length along Y (595): body 356 long and 208 wide, ribs 32 high, arched top up to z = 46
PART Body | symmetric outline (_SymCurve) extruded 32 for the ribs, arched top lofted to a smaller copy, f-holes cut | -104,0,0 104,356,46
PART Neck | neck and pegbox along Y (_SectionLoft), scroll at the end (_Torus turned upright, _Sphere) | -23,345,25 23,586,71
PART Fingerboard | black tapered board above the neck and the top: _Plate | -21,210,49 21,482,55
PART Pegs | four pegs across the pegbox with oval heads: _PipeThrough, _Ellipsoid | -33,482,34 33,546,52
PART Bridge | thin upright bridge standing on the top: _Plate in the plane y = 160 | -21,158,46 21,162,78
PART Tailpiece | black tailpiece near the bottom end, end pin and chin rest: _SymCurve, _ExtrudeCrv, _PipeThrough, _Ellipsoid | -83,-12,12 20,135,52
PART Strings | four strings from the tailpiece over the bridge to the nut: _PipeThrough | -19,128,51 19,483,80
)",
       {{"Body", R"(_SymCurve 0,356 45,351 72,337 84,312 82,287 68,266 57,248 55,215 56,190 60,165 72,148 92,130 102,100 103,70 95,38 75,14 42,2 0,0 _Enter => Outline
_ExtrudeCrv _Solid=_Yes 32 @Outline => Body
_Copy 0,0,0 0,0,32 _Enter @Outline => Rim
_Copy 0,0,0 0,0,46 _Enter @Outline => Crest
_Scale 0,178,46 0.55 @Crest
_Loft @Rim @Crest => Arch
_Cap @Arch
_BooleanUnion @Body @Arch => Body
_Delete @Outline @Rim @Crest
_PipeThrough 3 40,205,41 33,190,42 34,172,42 41,152,42 37,137,41 _Enter => Fhole
_Mirror 0,0,0 0,1,0 @Fhole => Fhole
_BooleanDifference @Body @Fhole => Body
_SetObjectColor 165,85,30 @Body)"},
        {"Neck", R"(_SectionLoft 0,345,36 34 24 0,420,37 30 22 0,482,38 26 20 _Enter => Neck
_SectionLoft 0,478,40 26 26 0,512,42 24 26 0,548,44 22 24 _Enter => Neck
_Torus 0,563,48 16 7 => Neck
_Rotate3D 0,563,48 0,564,48 90 @Neck
_Sphere 0,563,48 10 => Neck
_SetObjectColor 165,85,30 @Neck)"},
        {"Fingerboard", R"(_Plate 6 -12,482,52 12,482,52 21,210,52 -21,210,52 _Close => Fingerboard
_SetObjectColor 30,22,20 @Fingerboard)"},
        {"Pegs", R"(_PipeThrough 3 -27,490,42 27,490,42 _Enter => Pegs
_PipeThrough 3 -27,506,43 27,506,43 _Enter => Pegs
_PipeThrough 3 -27,522,43 27,522,43 _Enter => Pegs
_PipeThrough 3 -27,538,44 27,538,44 _Enter => Pegs
_Ellipsoid -30,490,42 3 8 8 => Pegs
_Ellipsoid 30,506,43 3 8 8 => Pegs
_Ellipsoid -30,522,43 3 8 8 => Pegs
_Ellipsoid 30,538,44 3 8 8 => Pegs
_SetObjectColor 30,22,20 @Pegs)"},
        {"Bridge", R"(_Plate 3 -21,160,46 21,160,46 16,160,78 -16,160,78 _Close => Bridge
_SetObjectColor 235,205,150 @Bridge)"},
        {"Tailpiece", R"(_SymCurve 0,135,48 20,130,48 16,80,48 9,30,48 0,25,48 _Enter => TpOutline
_ExtrudeCrv _Solid=_Yes 4 @TpOutline => Tailpiece
_Delete @TpOutline
_PipeThrough 4 0,4,16 0,-12,16 _Enter => Tailpiece
_Ellipsoid -45,45,36 38 28 6 => Tailpiece
_SetObjectColor 30,22,20 @Tailpiece)"},
        {"Strings", R"(# each string: straight from the tailpiece to the top of the bridge, then straight to the nut
_PipeThrough 0.6 -7.5,130,52 -18,160,78 _Enter => Strings
_PipeThrough 0.6 -18,160,78 -10.5,482,56 _Enter => Strings
_PipeThrough 0.6 -2.5,130,52 -6,160,78 _Enter => Strings
_PipeThrough 0.6 -6,160,78 -3.5,482,56 _Enter => Strings
_PipeThrough 0.6 2.5,130,52 6,160,78 _Enter => Strings
_PipeThrough 0.6 6,160,78 3.5,482,56 _Enter => Strings
_PipeThrough 0.6 7.5,130,52 18,160,78 _Enter => Strings
_PipeThrough 0.6 18,160,78 10.5,482,56 _Enter => Strings
_SetObjectColor 215,215,205 @Strings)"}}},

      {"aereo", "aere aeroplan airplan plane jet velivol avion elicotter aliant",
       "un aereo di linea",
       R"(# Airliner, length along Y (38000), wingspan 34000, fuselage 4000 high from z = 1000 (landing gear not modelled)
PART Fuselage | long rounded body from the nose (y = 0) to the raised tail: _SectionLoft along Y | -1950,0,1000 1950,38000,5000
PART Wings | swept wings: _Plate for the right wing, then _Mirror | -17000,13000,1800 17000,21500,2600
PART Engines | two engines under the wings: _SectionLoft, then _Mirror | -7600,11500,200 7600,16000,2400
PART Fin | vertical tail fin: _SectionLoft of flat sections stacked in Z | -250,31250,4500 250,37800,9500
PART Stabilizers | small horizontal tail wings: _Plate, then _Mirror | -6500,33500,3800 6500,37500,4400
)",
       {{"Fuselage", R"(_SectionLoft 0,0,2800 400 400 0,1000,2900 2600 2800 0,3500,3000 3900 4000 0,30000,3000 3900 4000 0,35500,3500 2000 2200 0,38000,4200 300 400 _Enter => Fuselage
_SetObjectColor 235,235,240 @Fuselage)"},
        {"Wings", R"(_Plate 400 1500,13000,2000 17000,19500,2400 17000,21500,2400 1500,20000,2000 _Close => Wings
_Mirror 0,0,0 0,1,0 @Wings => Wings
_SetObjectColor 200,200,210 @Wings)"},
        {"Engines", R"(_SectionLoft 6500,11500,1300 1800 1800 6500,13500,1300 2200 2200 6500,16000,1300 1600 1600 _Enter => Engines
_Mirror 0,0,0 0,1,0 @Engines => Engines
_SetObjectColor 150,150,160 @Engines)"},
        {"Fin", R"(_SectionLoft 0,33500,4600 500 4500 0,35500,7000 350 3000 0,37000,9500 200 1600 _Enter => Fin
_SetObjectColor 40,70,160 @Fin)"},
        {"Stabilizers", R"(_Plate 200 1000,33500,4000 6500,36500,4300 6500,37500,4300 1000,37500,4000 _Close => Stabilizers
_Mirror 0,0,0 0,1,0 @Stabilizers => Stabilizers
_SetObjectColor 40,70,160 @Stabilizers)"}}},

      {"automobile", "auto macchin car automobil vettur camion truck furgon",
       "un'automobile",
       R"(# Car 4300 long (along Y, front at y = 4300), 1800 wide, 1400 high, wheels standing on z = 0
PART Body | lower body with a rounded outline seen from above: _SymCurve, _ExtrudeCrv 650 at z 300 | -900,0,300 900,4300,950
PART Cabin | rounded cabin with the windows on top of the body: _SectionLoft along Y | -760,1250,500 760,3300,1400
PART Wheels | four wheels: short thick rods along X with _PipeThrough | -920,470,0 920,3830,660
PART Lights | two headlights at the front: _Ellipsoid, then _Mirror | -720,4230,640 720,4310,760
)",
       {{"Body", R"(_SymCurve 0,4300,300 600,4270,300 850,4100,300 900,3600,300 900,800,300 860,200,300 600,10,300 0,0,300 _Enter => Outline
_ExtrudeCrv _Solid=_Yes 650 @Outline => Body
_Delete @Outline
_SetObjectColor 200,30,30 @Body)"},
        {"Cabin", R"(_SectionLoft 0,1250,950 1400 100 0,1700,950 1500 900 0,2900,950 1450 900 0,3300,950 1300 100 _Enter => Cabin
_SetObjectColor 120,160,200 @Cabin)"},
        {"Wheels", R"(_PipeThrough 330 -920,800,330 -700,800,330 _Enter => Wheels
_PipeThrough 330 700,800,330 920,800,330 _Enter => Wheels
_PipeThrough 330 -920,3500,330 -700,3500,330 _Enter => Wheels
_PipeThrough 330 700,3500,330 920,3500,330 _Enter => Wheels
_SetObjectColor 30,30,30 @Wheels)"},
        {"Lights", R"(_Ellipsoid -600,4260,700 120 40 60 => Lights
_Mirror 0,0,0 0,1,0 @Lights => Lights
_SetObjectColor 250,240,170 @Lights)"}}},

      {"teiera", "teier teapot caffettier bollitor kettle brocc jug",
       "una teiera",
       R"(# Teapot about 200 high standing on z = 0: round body, lid with knob, spout on the -Y side, handle on the +Y side
PART Body | round belly turned around the vertical axis: _Lathe | -100,-100,0 100,100,150
PART Lid | lid and knob on top: _Lathe | -55,-55,148 55,55,196
PART Spout | spout rising from the side: _SectionLoft | -20,-165,40 20,-80,160
PART Handle | loop handle: _PipeThrough | -8,77,30 8,168,140
)",
       {{"Body", R"(_Lathe 0,0,0 60,0 90,40 100,80 85,130 55,150 _Enter => Body
_SetObjectColor 240,240,235 @Body)"},
        {"Lid", R"(_Lathe 0,0,148 55,0 45,20 12,30 10,38 16,44 0,48 _Enter => Lid
_SetObjectColor 240,240,235 @Lid)"},
        {"Spout", R"(_SectionLoft 0,-85,60 40 40 0,-130,100 26 26 0,-165,150 16 16 _Enter => Spout
_SetObjectColor 240,240,235 @Spout)"},
        {"Handle", R"(_PipeThrough 8 0,85,120 0,140,130 0,160,90 0,140,50 0,90,40 _Enter => Handle
_SetObjectColor 240,240,235 @Handle)"}}},

      {"barca", "barca boat nave ship vela sail yacht gommon canoa canoe kayak",
       "una barca a vela",
       R"(# Sailboat 8000 long (along Y, bow at y = 8000), 2600 wide: hull up to the deck at z = 1200, keel below z = 0, mast and sail
PART Hull | deck outline (_SymCurve) at z 1200 lofted to a smaller copy at z 0 (_Copy, _Scale, _Loft, _Cap) | -1300,0,0 1300,8000,1200
PART Keel | fin keel under the hull: _Plate in the plane x = 0 | -100,3500,-1400 100,5000,50
PART Mast | tall mast: _PipeThrough | -80,4720,1150 80,4880,11000
PART Sail | triangular main sail behind the mast: _Plate in the plane x = 0 | -20,1800,1600 20,4780,10800
)",
       {{"Hull", R"(_SymCurve 0,8000,1200 700,7200,1200 1200,5500,1200 1300,3500,1200 1150,1000,1200 800,0,1200 0,0,1200 _Enter => Deck
_Copy 0,0,0 0,0,-1200 _Enter @Deck => Bottom
_Scale 0,4000,0 0.6 @Bottom
_Loft @Deck @Bottom => Hull
_Cap @Hull
_Delete @Deck @Bottom
_SetObjectColor 240,240,240 @Hull)"},
        {"Keel", R"(_Plate 200 0,3500,50 0,5000,50 0,4700,-1400 0,4000,-1400 _Close => Keel
_SetObjectColor 60,60,70 @Keel)"},
        {"Mast", R"(_PipeThrough 80 0,4800,1150 0,4800,11000 _Enter => Mast
_SetObjectColor 180,180,190 @Mast)"},
        {"Sail", R"(_Plate 40 0,4700,1600 0,4700,10800 0,1800,1600 _Close => Sail
_SetObjectColor 250,250,245 @Sail)"}}},
  };
  return ex;
}

}  // namespace cad::ai
