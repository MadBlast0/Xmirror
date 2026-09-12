#include "fluenttheme.h"

#include <QApplication>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QPalette>
#include <QDir>
#include <QHash>
#include <QOperatingSystemVersion>
#include <QPointer>
#include <QRegularExpression>
#include <QWidget>
#include <QImage>
#include <QPainter>
#include <QSettings>
#include <QStandardPaths>
#include <QStyleFactory>
#include <QStyleHints>

#include <cmath>

#include <windows.h>
#include <dwmapi.h>

namespace {

// --- colour helpers ------------------------------------------------------

double channel(int v) {
    const double c = v / 255.0;
    return c <= 0.03928 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

double luminance(const QColor &c) {
    return 0.2126 * channel(c.red()) + 0.7152 * channel(c.green()) +
           0.0722 * channel(c.blue());
}

double contrast(const QColor &a, const QColor &b) {
    double la = luminance(a) + 0.05;
    double lb = luminance(b) + 0.05;
    if (la < lb) std::swap(la, lb);
    return la / lb;
}

// The Windows accent is chosen for wallpaper harmony, not for legibility. It is
// used here as a fill behind white (light mode) or black (dark mode) glyphs, so
// nudge it until that pairing is actually readable rather than trusting it.
QColor legibleAccent(QColor accent, bool dark) {
    const QColor glyph = dark ? QColor(Qt::black) : QColor(Qt::white);
    for (int i = 0; i < 24 && contrast(accent, glyph) < 3.5; ++i) {
        accent = dark ? accent.lighter(106) : accent.darker(106);
    }
    return accent;
}

// --- chevrons ------------------------------------------------------------
//
// A stylesheet can only take an arrow from an image file, and once
// QComboBox::drop-down is styled at all Qt stops drawing the base style's
// arrow. Shipping icon files would mean touching packaging for two 10x6
// glyphs, so they are painted once into the cache directory instead, in the
// theme's own colour. Both 1x and @2x are written, which is the convention Qt
// uses to pick a high-DPI variant.
QString writeChevron(const QString &name, const QColor &color, bool up) {
    const QString dir =
        QStandardPaths::writableLocation(QStandardPaths::CacheLocation) +
        "/theme";
    QDir().mkpath(dir);

    const QString base = dir + "/" + name;
    for (int scale : {1, 2}) {
        QImage image(10 * scale, 6 * scale, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);

        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing, true);
        QPen pen(color, 1.3 * scale);
        pen.setCapStyle(Qt::RoundCap);
        pen.setJoinStyle(Qt::RoundJoin);
        painter.setPen(pen);

        const qreal top = 1.4 * scale;
        const qreal bottom = 4.6 * scale;
        QPolygonF chevron;
        if (up) {
            chevron << QPointF(1.2 * scale, bottom) << QPointF(5.0 * scale, top)
                    << QPointF(8.8 * scale, bottom);
        } else {
            chevron << QPointF(1.2 * scale, top) << QPointF(5.0 * scale, bottom)
                    << QPointF(8.8 * scale, top);
        }
        painter.drawPolyline(chevron);
        painter.end();

        image.save(scale == 1 ? base + ".png" : base + "@2x.png");
    }
    return base + ".png";
}

struct Palette {
    QString bg, card, cardLine, line, fg, mut, hover, sel;
    QString field, fieldLine, fieldHover, btn, btnHover;
    QString warnBg, warnFg, warnLine;
    QString infoBg, infoFg, infoLine;
    QString ok, scroll;
    QString swTrack, swTrackHover, swBorder, swKnob, disabled;
    QString popup, cardHover;
};

Palette paletteFor(bool dark) {
    Palette p;
    if (dark) {
        p.bg = "#202020";  p.card = "#2b2b2b";  p.cardLine = "#363636";
        p.line = "#2d2d2d"; p.fg = "#f2f2f2";   p.mut = "#a0a0a6";
        p.hover = "#2e2e2e"; p.sel = "#2d2d2d";
        p.field = "#333333"; p.fieldLine = "#434343"; p.fieldHover = "#3a3a3a";
        p.btn = "#333333";   p.btnHover = "#3a3a3a";
        p.warnBg = "#433519"; p.warnFg = "#f5d98a"; p.warnLine = "#6a5426";
        p.infoBg = "#1d2b36"; p.infoFg = "#8fd3ff"; p.infoLine = "#33505f";
        p.ok = "#6ccb5f";    p.scroll = "#4a4a4a";
        p.swTrack = "#272727"; p.swTrackHover = "#313131";
        p.swBorder = "#9a9a9e"; p.swKnob = "#cfcfd4"; p.disabled = "#4a4a4a";
        p.popup = "#2b2b2b"; p.cardHover = "#323232";
    } else {
        p.bg = "#f3f3f3";  p.card = "#fbfbfb";  p.cardLine = "#e8e8ea";
        p.line = "#e2e2e5"; p.fg = "#1b1b1f";   p.mut = "#5d5d63";
        p.hover = "#eaeaec"; p.sel = "#ffffff";
        p.field = "#fdfdfd"; p.fieldLine = "#d6d6da"; p.fieldHover = "#f5f5f6";
        p.btn = "#fbfbfb";   p.btnHover = "#f2f2f3";
        p.warnBg = "#fff4ce"; p.warnFg = "#7a4d00"; p.warnLine = "#e8cf8a";
        p.infoBg = "#eaf3fb"; p.infoFg = "#00457a"; p.infoLine = "#bcd8ef";
        p.ok = "#0f7b0f";    p.scroll = "#c4c4c8";
        p.swTrack = "#ffffff"; p.swTrackHover = "#f0f0f0";
        p.swBorder = "#8a8a8f"; p.swKnob = "#5d5d63"; p.disabled = "#c8c8cc";
        p.popup = "#fbfbfb"; p.cardHover = "#f6f6f7";
    }
    return p;
}

// Acrylic variant. Surfaces that sit directly on the window become transparent so
// the material shows; cards and fields keep enough fill to stay legible over
// any wallpaper. Popups (p.popup) are untouched -- they are separate windows
// with no material behind them, and a translucent fill there reads as black.
void applyBackdrop(Palette &p, bool dark) {
    p.bg = "transparent";
    p.swTrack = "transparent";
    if (dark) {
        p.card = "rgba(255, 255, 255, 13)";  p.cardLine = "rgba(255, 255, 255, 18)";
        p.cardHover = "rgba(255, 255, 255, 22)";
        p.line = "rgba(255, 255, 255, 20)";  p.hover = "rgba(255, 255, 255, 14)";
        p.sel = "rgba(255, 255, 255, 24)";
        p.field = "rgba(255, 255, 255, 18)"; p.fieldLine = "rgba(255, 255, 255, 30)";
        p.fieldHover = "rgba(255, 255, 255, 26)";
        p.btn = "rgba(255, 255, 255, 20)";   p.btnHover = "rgba(255, 255, 255, 30)";
        p.warnBg = "rgba(67, 53, 25, 200)";  p.infoBg = "rgba(29, 43, 54, 190)";
        p.swTrackHover = "rgba(255, 255, 255, 18)";
    } else {
        p.card = "rgba(255, 255, 255, 160)"; p.cardLine = "rgba(0, 0, 0, 16)";
        p.cardHover = "rgba(255, 255, 255, 200)";
        p.line = "rgba(0, 0, 0, 20)";        p.hover = "rgba(255, 255, 255, 110)";
        p.sel = "rgba(255, 255, 255, 190)";
        p.field = "rgba(255, 255, 255, 215)"; p.fieldLine = "rgba(0, 0, 0, 30)";
        p.fieldHover = "rgba(255, 255, 255, 240)";
        p.btn = "rgba(255, 255, 255, 200)";  p.btnHover = "rgba(255, 255, 255, 235)";
        p.warnBg = "rgba(255, 244, 206, 215)"; p.infoBg = "rgba(234, 243, 251, 205)";
        p.swTrackHover = "rgba(255, 255, 255, 120)";
    }
}

QString rgba(const QColor &c, int alpha) {
    return QString("rgba(%1, %2, %3, %4)")
        .arg(c.red()).arg(c.green()).arg(c.blue()).arg(alpha);
}

// The stylesheet below is written against $tokens rather than literal colours
// so the light and dark variants cannot drift apart.
const char *const kStyleSheet = R"QSS(
QWidget { color: $fg; }
QMainWindow { background: $bg; }
QToolTip {
  background: $popup; color: $fg; border: 1px solid $line; padding: 4px 8px;
}

/* --- section rail --- */
QListWidget#rail {
  background: $bg; border: none; outline: none; padding: 8px;
}
QListWidget#rail::item {
  padding: 8px 10px; margin: 1px 0px; border-radius: 6px; color: $fg;
  border: 1px solid transparent; border-left: 3px solid transparent;
}
QListWidget#rail::item:hover { background: $hover; }
QListWidget#rail::item:selected {
  background: $sel; color: $fg;
  border: 1px solid $cardLine; border-left: 3px solid $accent;
}

/* --- pages --- */
QScrollArea { background: $bg; border: none; }
QScrollArea > QWidget#qt_scrollarea_viewport { background: transparent; }
QFrame#pageContent { background: $bg; }
QLabel#pageTitle {
  font-family: "Segoe UI Variable Display", "Segoe UI";
  font-size: 16pt; font-weight: 600; color: $fg;
}
QLabel#pageLede { font-size: 9pt; color: $mut; }
QLabel#groupLabel { font-size: 8pt; font-weight: 600; color: $mut; }

/* --- setting rows --- */
QFrame#row {
  background: $card; border: 1px solid $cardLine; border-radius: 6px;
}
QFrame#row QLabel { background: transparent; }
QLabel#rowName { font-size: 10pt; color: $fg; }
QLabel#rowDesc { font-size: 9pt; color: $mut; }

QLabel#badge {
  font-size: 8pt; padding: 1px 7px; border-radius: 8px;
  border: 1px solid $cardLine; color: $mut;
}
QLabel#badge[tier="restart"] { border-color: $warnLine; color: $warnFg; }
QLabel#badge[tier="next"] { border-color: $infoLine; color: $infoFg; }

/* --- inputs --- */
QLineEdit, QComboBox, QAbstractSpinBox {
  background: $field; border: 1px solid $fieldLine; border-radius: 5px;
  padding: 5px 9px; color: $fg;
  selection-background-color: $accent; selection-color: $accentText;
}
QLineEdit:hover, QComboBox:hover, QAbstractSpinBox:hover { background: $fieldHover; }
QLineEdit:focus, QComboBox:focus, QAbstractSpinBox:focus { border: 1px solid $accent; }
QLineEdit:disabled, QComboBox:disabled, QAbstractSpinBox:disabled {
  color: $mut; background: $bg;
}
QComboBox::drop-down {
  subcontrol-origin: padding; subcontrol-position: center right;
  width: 22px; border: none; background: transparent;
}
QComboBox::down-arrow { image: url("$chevronDown"); width: 10px; height: 6px; }
QAbstractSpinBox::up-button, QAbstractSpinBox::down-button {
  subcontrol-origin: border; width: 20px; border: none; background: transparent;
}
QAbstractSpinBox::up-button { subcontrol-position: top right; }
QAbstractSpinBox::down-button { subcontrol-position: bottom right; }
QAbstractSpinBox::up-arrow { image: url("$chevronUp"); width: 10px; height: 6px; }
QAbstractSpinBox::down-arrow { image: url("$chevronDown"); width: 10px; height: 6px; }
QComboBox QAbstractItemView {
  background: $popup; border: 1px solid $line; border-radius: 6px;
  padding: 4px; outline: none;
  selection-background-color: $hover; selection-color: $fg;
}

/* --- buttons --- */
QPushButton {
  background: $btn; border: 1px solid $fieldLine; border-radius: 5px;
  padding: 6px 14px; color: $fg;
}
QPushButton:hover { background: $btnHover; }
QPushButton:pressed { background: $hover; color: $mut; }
QPushButton:disabled { color: $mut; }
QPushButton#primary {
  background: $accent; border: 1px solid $accent; color: $accentText;
}
QPushButton#primary:hover { background: $accentHover; border-color: $accentHover; }

/* --- banners --- */
QFrame#noticeBar { background: $warnBg; border-bottom: 1px solid $warnLine; }
QFrame#noticeBar QLabel { color: $warnFg; font-size: 9pt; background: transparent; }
QFrame#deferralBar { background: $infoBg; border-bottom: 1px solid $infoLine; }
QFrame#deferralBar QLabel { color: $infoFg; font-size: 9pt; background: transparent; }

/* --- footer --- */
QFrame#footer { background: $bg; border-top: 1px solid $line; }
QFrame#footer QLabel { color: $mut; font-size: 9pt; background: transparent; }
QFrame#footer QLabel#statusDot {
  border-radius: 4px; background: $mut;
  min-width: 8px; max-width: 8px; min-height: 8px; max-height: 8px;
}
QFrame#footer QLabel#statusDot[state="live"] { background: $ok; }
QFrame#footer QLabel#statusDot[state="error"] { background: $warnFg; }

QFrame#sep { background: $line; border: none; }

/* --- scrollbars --- */
QScrollBar:vertical { background: transparent; width: 12px; margin: 0px; }
QScrollBar::handle:vertical {
  background: $scroll; border-radius: 3px; min-height: 28px; margin: 2px 4px;
}
QScrollBar::handle:vertical:hover { background: $mut; }
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0px; }
QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: transparent; }
QScrollBar:horizontal { height: 0px; }

/* --- tray menu --- */
QMenu { background: $popup; border: 1px solid $line; padding: 5px; }
QMenu::item { padding: 7px 24px 7px 14px; border-radius: 5px; color: $fg; }
QMenu::item:selected { background: $hover; }
QMenu::item:disabled { color: $mut; }
QMenu::separator { height: 1px; background: $line; margin: 5px 8px; }

/* --- home page --- */
/* Selectors on labels inside the hero and footer carry the container too:
   "QFrame#hero QLabel" would otherwise outrank "QLabel#heroIcon" and wipe
   its background. */
QFrame#hero { background: $card; border: 1px solid $cardLine; border-radius: 8px; }
QFrame#hero QLabel { background: transparent; }
QFrame#hero QLabel#heroIcon {
  background: $accentSoft; color: $accent; border-radius: 12px;
  min-width: 52px; max-width: 52px; min-height: 52px; max-height: 52px;
}
QFrame#hero QLabel#heroIcon[state="live"] { background: $okSoft; color: $ok; }
QFrame#hero QLabel#heroIcon[state="stopped"] { background: $hover; color: $mut; }
QFrame#hero QLabel#heroIcon[state="error"] { background: $warnBg; color: $warnFg; }
QLabel#heroTitle {
  font-family: "Segoe UI Variable Display", "Segoe UI";
  font-size: 13pt; font-weight: 600; color: $fg;
}
QLabel#heroSub { font-size: 9pt; color: $mut; }

QFrame#tile { background: $card; border: 1px solid $cardLine; border-radius: 6px; }
QFrame#tile QLabel { background: transparent; }
QLabel#tileSub { font-size: 8pt; color: $mut; }

/* A 1px border plus 1px padding, or a 2px border and none, so selecting a
   card never shifts its contents. */
QPushButton#presetCard {
  background: $card; border: 1px solid $cardLine; border-radius: 6px;
  padding: 1px; text-align: left;
}
QPushButton#presetCard:hover { background: $cardHover; }
QPushButton#presetCard:checked { border: 2px solid $accent; padding: 0px; }
QPushButton#presetCard QLabel { background: transparent; }
QLabel#presetTitle { font-size: 10pt; font-weight: 600; color: $fg; }
QLabel#presetDesc { font-size: 9pt; color: $mut; }

/* --- the toggle's palette, so it is not duplicated in C++ --- */
FluentSwitch {
  qproperty-accentColor: $accent;
  qproperty-trackColor: $swTrack;
  qproperty-trackHoverColor: $swTrackHover;
  qproperty-trackBorderColor: $swBorder;
  qproperty-knobColor: $swKnob;
  qproperty-knobOnColor: $accentText;
  qproperty-disabledColor: $disabled;
}
)QSS";

void applyPalette(bool dark, const QColor &accent) {
    // Set even though nearly everything is styled: QMessageBox, QInputDialog and
    // the native file dialogs are not, and would otherwise stay light.
    QPalette p;
    if (dark) {
        p.setColor(QPalette::Window, QColor("#202020"));
        p.setColor(QPalette::WindowText, QColor("#f2f2f2"));
        p.setColor(QPalette::Base, QColor("#2b2b2b"));
        p.setColor(QPalette::AlternateBase, QColor("#333333"));
        p.setColor(QPalette::Text, QColor("#f2f2f2"));
        p.setColor(QPalette::Button, QColor("#333333"));
        p.setColor(QPalette::ButtonText, QColor("#f2f2f2"));
        p.setColor(QPalette::ToolTipBase, QColor("#2b2b2b"));
        p.setColor(QPalette::ToolTipText, QColor("#f2f2f2"));
        p.setColor(QPalette::Disabled, QPalette::Text, QColor("#7a7a80"));
        p.setColor(QPalette::Disabled, QPalette::ButtonText, QColor("#7a7a80"));
        p.setColor(QPalette::Disabled, QPalette::WindowText, QColor("#7a7a80"));
    } else {
        p.setColor(QPalette::Window, QColor("#f3f3f3"));
        p.setColor(QPalette::WindowText, QColor("#1b1b1f"));
        p.setColor(QPalette::Base, QColor("#ffffff"));
        p.setColor(QPalette::AlternateBase, QColor("#f6f6f7"));
        p.setColor(QPalette::Text, QColor("#1b1b1f"));
        p.setColor(QPalette::Button, QColor("#fbfbfb"));
        p.setColor(QPalette::ButtonText, QColor("#1b1b1f"));
        p.setColor(QPalette::ToolTipBase, QColor("#ffffff"));
        p.setColor(QPalette::ToolTipText, QColor("#1b1b1f"));
        p.setColor(QPalette::Disabled, QPalette::Text, QColor("#9a9aa0"));
        p.setColor(QPalette::Disabled, QPalette::ButtonText, QColor("#9a9aa0"));
        p.setColor(QPalette::Disabled, QPalette::WindowText, QColor("#9a9aa0"));
    }
    p.setColor(QPalette::Highlight, accent);
    p.setColor(QPalette::HighlightedText, dark ? Qt::black : Qt::white);
    p.setColor(QPalette::Link, accent);
    QApplication::setPalette(p);
}

bool g_backdrop = false;
QList<QPointer<QWidget>> g_backdropWindows;

// The material takes its light or dark tint from the window's immersive
// dark-mode flag, not from the app's colours, so the flag has to follow the
// system too.
void setWindowDarkMode(QWidget *window, bool dark) {
    const HWND hwnd = reinterpret_cast<HWND>(window->winId());
    const BOOL value = dark ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */,
                          &value, sizeof(value));
}

void apply() {
    const bool dark = FluentTheme::systemPrefersDark();
    const QColor accent = FluentTheme::systemAccent(dark);
    applyPalette(dark, accent);
    qApp->setStyleSheet(FluentTheme::styleSheet(dark, accent, g_backdrop));

    for (const QPointer<QWidget> &window : std::as_const(g_backdropWindows)) {
        if (window) setWindowDarkMode(window, dark);
    }
}

}  // namespace

bool FluentTheme::systemPrefersDark() {
    return QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark;
}

QColor FluentTheme::systemAccent(bool dark) {
    const QColor fallback = dark ? QColor("#4cc2ff") : QColor("#0067c0");

    // DWM stores the accent as a little-endian ABGR DWORD, not RGB.
    QSettings dwm(R"(HKEY_CURRENT_USER\Software\Microsoft\Windows\DWM)",
                  QSettings::NativeFormat);
    const QVariant raw = dwm.value("AccentColor");
    if (!raw.isValid()) return fallback;

    bool ok = false;
    const quint32 abgr = raw.toUInt(&ok);
    if (!ok) return fallback;

    const QColor accent(int(abgr & 0xFF), int((abgr >> 8) & 0xFF),
                        int((abgr >> 16) & 0xFF));
    if (!accent.isValid()) return fallback;

    return legibleAccent(accent, dark);
}

QString FluentTheme::styleSheet(bool dark, const QColor &accent, bool backdrop) {
    const Palette solid = paletteFor(dark);
    Palette p = solid;
    if (backdrop) applyBackdrop(p, dark);
    const QColor accentHover = dark ? accent.lighter(112) : accent.darker(108);

    const QString chevronDown =
        writeChevron(dark ? "chevron-down-dark" : "chevron-down-light",
                     QColor(solid.mut), false);
    const QString chevronUp =
        writeChevron(dark ? "chevron-up-dark" : "chevron-up-light",
                     QColor(solid.mut), true);

    const QHash<QString, QString> tokens = {
        {"chevronDown", chevronDown}, {"chevronUp", chevronUp},
        {"accent", accent.name()}, {"accentHover", accentHover.name()},
        {"accentText", dark ? QStringLiteral("#000000") : QStringLiteral("#ffffff")},
        {"accentSoft", rgba(accent, dark ? 46 : 34)},
        {"okSoft", rgba(QColor(p.ok), dark ? 46 : 34)},
        {"bg", p.bg}, {"card", p.card}, {"cardLine", p.cardLine},
        {"cardHover", p.cardHover}, {"popup", p.popup},
        {"line", p.line}, {"fg", p.fg}, {"mut", p.mut},
        {"hover", p.hover}, {"sel", p.sel},
        {"field", p.field}, {"fieldLine", p.fieldLine}, {"fieldHover", p.fieldHover},
        {"btn", p.btn}, {"btnHover", p.btnHover},
        {"warnBg", p.warnBg}, {"warnFg", p.warnFg}, {"warnLine", p.warnLine},
        {"infoBg", p.infoBg}, {"infoFg", p.infoFg}, {"infoLine", p.infoLine},
        {"ok", p.ok}, {"scroll", p.scroll},
        {"swTrack", p.swTrack}, {"swTrackHover", p.swTrackHover},
        {"swBorder", p.swBorder}, {"swKnob", p.swKnob}, {"disabled", p.disabled},
    };

    // Whole-token lookup rather than sequential replace(), so a token that is
    // a prefix of another ($card / $cardLine) can never corrupt it.
    const QString source = QString::fromUtf8(kStyleSheet);
    static const QRegularExpression tokenPattern(QStringLiteral("\\$([A-Za-z]+)"));
    QString qss;
    qss.reserve(source.size() + 1024);
    qsizetype last = 0;
    auto it = tokenPattern.globalMatch(source);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        qss += QStringView(source).mid(last, m.capturedStart() - last);
        const auto found = tokens.constFind(m.captured(1));
        if (found != tokens.cend()) {
            qss += found.value();
        } else {
            qWarning() << "[theme] Unknown stylesheet token" << m.captured(0);
            qss += m.captured(0);
        }
        last = m.capturedEnd();
    }
    qss += QStringView(source).mid(last);
    return qss;
}

void FluentTheme::install() {
    QApplication::setStyle(QStyleFactory::create("Fusion"));

    // Segoe UI Variable is the Windows 11 UI face and is absent on Windows 10,
    // where Segoe UI is the correct fallback.
    const QStringList families = QFontDatabase::families();
    const QString family = families.contains("Segoe UI Variable Text")
                               ? QStringLiteral("Segoe UI Variable Text")
                               : QStringLiteral("Segoe UI");
    QFont font(family, 10);
    font.setHintingPreference(QFont::PreferNoHinting);
    QApplication::setFont(font);

    apply();

    QObject::connect(QGuiApplication::styleHints(),
                     &QStyleHints::colorSchemeChanged, qApp,
                     [](Qt::ColorScheme) { apply(); });
}

bool FluentTheme::enableAcrylic(QWidget *window) {
    // DWMWA_SYSTEMBACKDROP_TYPE arrived in Windows 11 22H2 (build 22621).
    // Earlier builds accept the call and silently do nothing, which on a
    // translucent window would leave it see-through, so gate on the build.
    const QOperatingSystemVersion os = QOperatingSystemVersion::current();
    if (os.type() != QOperatingSystemVersion::Windows || os.microVersion() < 22621) {
        return false;
    }
    if (window->testAttribute(Qt::WA_WState_Created)) {
        qWarning() << "[theme] enableAcrylic() called after the window was created; ignored.";
        return false;
    }

    window->setAttribute(Qt::WA_TranslucentBackground, true);
    const HWND hwnd = reinterpret_cast<HWND>(window->winId());

    // A negative margin extends the frame over the whole client area, which is
    // what lets the material show behind Qt's transparent pixels.
    const MARGINS margins{-1, -1, -1, -1};
    // DWMSBT_TRANSIENTWINDOW, i.e. Acrylic: a blur of whatever is behind the
    // window. Mica (2) samples only the wallpaper and reads near-solid on a
    // dark or neutral one, which was too subtle for this design.
    const int backdrop = 3;
    const bool ok =
        SUCCEEDED(DwmExtendFrameIntoClientArea(hwnd, &margins)) &&
        SUCCEEDED(DwmSetWindowAttribute(hwnd, 38 /* DWMWA_SYSTEMBACKDROP_TYPE */,
                                        &backdrop, sizeof(backdrop)));
    if (!ok) {
        // The window is already translucent. Keeping the solid stylesheet
        // (g_backdrop stays false) paints an opaque background over it.
        qWarning() << "[theme] DWM refused the Acrylic backdrop; using solid surfaces.";
        return false;
    }

    g_backdropWindows.append(window);
    g_backdrop = true;
    apply();
    return true;
}

bool FluentTheme::backdropActive() {
    return g_backdrop;
}
