#pragma once

#include <QColor>
#include <QString>

class QWidget;

// The "Fluent Quiet" look, with an optional Acrylic glass backdrop: Windows 11
// Settings metrics and colouring, applied entirely through a stylesheet plus
// one custom widget (FluentSwitch).
//
// Nothing here knows about XMirror. It sets the application style, palette,
// font and stylesheet, and keeps them following the Windows light/dark setting
// for the life of the process.
namespace FluentTheme {

// Windows' current app theme. Falls back to light if it cannot be determined.
bool systemPrefersDark();

// The user's Windows accent colour, adjusted so that white glyphs on top of it
// stay legible (a pale yellow accent is darkened; a near-black one lightened).
// Falls back to the Windows default blue.
QColor systemAccent(bool dark);

// `backdrop` selects the Acrylic variant: window-level surfaces become
// transparent and cards translucent, so what is behind the window shows
// through, blurred. Popups stay solid, because they are separate windows with
// no backdrop behind them.
QString styleSheet(bool dark, const QColor &accent, bool backdrop = false);

// Applies the theme and connects to the system colour-scheme signal so a user
// switching Windows to dark mode does not have to restart XMirror.
void install();

// Puts the Windows 11 Acrylic material behind `window` and switches the stylesheet
// to its translucent variant.
//
// Must be called before the window's native handle exists: it sets
// WA_TranslucentBackground, which Qt only honours at creation. Returns false
// and leaves the solid look untouched where Acrylic is unavailable -- Windows 10,
// or Windows 11 before 22H2 -- or if DWM refuses the request.
bool enableAcrylic(QWidget *window);

bool backdropActive();

}  // namespace FluentTheme
