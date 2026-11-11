#pragma once
// Text shown inside the glasses: calibration steps, toasts and placeholders.
#include "util.h"
#include "../core/fusion.h"

namespace ox::text {

inline const wchar_t* calibrationStep(CalibrationWizard::Step s) {
    switch (s) {
    case CalibrationWizard::HoldStill: return tr(L"Mira al frente sin moverte", L"Look straight ahead and keep still");
    case CalibrationWizard::TurnRight:
        return tr(L"Gira la cabeza despacio hacia la derecha", L"Slowly turn your head to the right");
    case CalibrationWizard::LookUp:
        return tr(L"Vuelve al centro y levanta la vista hacia el techo", L"Back to the center, then look up at the ceiling");
    default: return L"";
    }
}

inline const wchar_t* calibrationError(CalibrationWizard::Error e) {
    switch (e) {
    case CalibrationWizard::NeverStill:
        return tr(L"No te has quedado quieto el tiempo suficiente.", L"You didn't keep still for long enough.");
    case CalibrationWizard::TiltedTurn:
        return tr(L"Has inclinado la cabeza al girar. Gira solo de lado a lado.",
                  L"Your head tilted while turning. Turn only from side to side.");
    case CalibrationWizard::NoTurn: return tr(L"No he notado el giro a la derecha.", L"The turn to the right didn't show up.");
    case CalibrationWizard::NoLookUp: return tr(L"No he notado que mirases hacia arriba.", L"Looking up didn't show up.");
    default: return L"";
    }
}

inline const wchar_t* noSensor() {
    return tr(L"No llegan datos del sensor de las gafas.\nMientras tanto, la vista sigue al ratón.",
              L"No data from the glasses' sensor.\nUntil it comes, the view follows the mouse.");
}
inline const wchar_t* needsCalibration() {
    return tr(L"Para mirar arriba y abajo hay que calibrar.\nSeguimiento > Calibrar (unos 10 segundos).",
              L"Looking up and down needs a calibration.\nTracking > Calibrate (about 10 seconds).");
}
inline const wchar_t* glassesMode() {
    return tr(L"Las gafas tienen que estar en modo Seguir (no Anclar)\ncon el estabilizador en fijo.",
              L"The glasses must be in Follow mode (not Anchor)\nwith the stabilizer on Fixed.");
}
inline const wchar_t* viewFrozen() { return tr(L"Vista congelada", L"View frozen"); }
inline const wchar_t* viewUnfrozen() { return tr(L"Vista libre", L"View unfrozen"); }
inline const wchar_t* waitingImage() { return tr(L"Esperando imagen", L"Waiting for the image"); }
inline const wchar_t* demoScreen() { return tr(L"Pantalla de prueba", L"Test screen"); }

} // namespace ox::text
