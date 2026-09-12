#pragma once

#include <QColor>
#include <QString>

// The "Fluent Quiet" look: Windows 11 Settings metrics on plain, solid grey
// surfaces, applied entirely through a stylesheet plus one custom widget
// (FluentSwitch).
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

QString styleSheet(bool dark, const QColor &accent);

// Applies the theme and connects to the system colour-scheme signal so a user
// switching Windows to dark mode does not have to restart XMirror.
void install();

}  // namespace FluentTheme
