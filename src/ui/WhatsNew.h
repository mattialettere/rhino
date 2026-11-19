#pragma once
class QWidget;

namespace cad::ui {

// Finestra "Novità di questa versione" (testo in res/novita.html).
// All'avvio compare finche' l'utente non sceglie "Non mostrare piu' per questa versione";
// dal menu ? si riapre sempre.
bool WhatsNewWanted();                 // va mostrata all'avvio per questa versione?
void ShowWhatsNew(QWidget* parent);    // finestra non modale, si chiude con Chiudi o Esc

}  // namespace cad::ui
