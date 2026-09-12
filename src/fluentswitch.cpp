#include "fluentswitch.h"

#include <QEnterEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPropertyAnimation>

namespace {
// Track geometry, in logical pixels. The widget is slightly larger than the
// track so the focus ring has somewhere to go without being clipped.
constexpr int kTrackWidth = 40;
constexpr int kTrackHeight = 20;
constexpr int kMargin = 3;
constexpr qreal kKnobOff = 11.0;  // diameter when off
constexpr qreal kKnobOn = 13.0;   // diameter when on
}  // namespace

FluentSwitch::FluentSwitch(QWidget *parent) : QAbstractButton(parent) {
    setCheckable(true);
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_Hover);

    m_animation = new QPropertyAnimation(this, "position", this);
    m_animation->setDuration(140);
    m_animation->setEasingCurve(QEasingCurve::OutCubic);
}

QSize FluentSwitch::sizeHint() const {
    return QSize(kTrackWidth + kMargin * 2, kTrackHeight + kMargin * 2);
}

void FluentSwitch::setPosition(qreal value) {
    m_position = value;
    update();
}

void FluentSwitch::checkStateSet() {
    m_animation->stop();
    m_animation->setStartValue(m_position);
    m_animation->setEndValue(isChecked() ? 1.0 : 0.0);
    // Snapping rather than animating before the widget is on screen avoids a
    // visible sweep on every control when the window is first shown.
    if (isVisible()) {
        m_animation->start();
    } else {
        setPosition(isChecked() ? 1.0 : 0.0);
    }
}

void FluentSwitch::enterEvent(QEnterEvent *event) {
    m_hovered = true;
    update();
    QAbstractButton::enterEvent(event);
}

void FluentSwitch::leaveEvent(QEvent *event) {
    m_hovered = false;
    update();
    QAbstractButton::leaveEvent(event);
}

void FluentSwitch::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const QRectF track(kMargin, (height() - kTrackHeight) / 2.0,
                       kTrackWidth, kTrackHeight);
    const qreal radius = track.height() / 2.0;
    const bool on = m_position > 0.5;
    const bool enabled = isEnabled();

    // --- track ---
    QColor fill;
    QColor border;
    if (!enabled) {
        fill = on ? m_disabled : Qt::transparent;
        border = m_disabled;
    } else if (on) {
        fill = m_accent;
        border = m_accent;
    } else {
        fill = m_hovered ? m_trackHover : m_track;
        border = m_trackBorder;
    }

    painter.setPen(QPen(border, 1.0));
    painter.setBrush(fill);
    painter.drawRoundedRect(track.adjusted(0.5, 0.5, -0.5, -0.5),
                            radius, radius);

    // --- knob ---
    // Grows as it travels, which is what makes the Windows toggle read as a
    // state change rather than a slider.
    const qreal diameter = kKnobOff + (kKnobOn - kKnobOff) * m_position;
    const qreal travelLeft = track.left() + radius;
    const qreal travelRight = track.right() - radius;
    const qreal centreX = travelLeft + (travelRight - travelLeft) * m_position;

    QColor knob;
    if (!enabled) {
        knob = m_disabled;
    } else {
        knob = on ? m_knobOn : m_knob;
    }

    painter.setPen(Qt::NoPen);
    painter.setBrush(knob);
    painter.drawEllipse(QPointF(centreX, track.center().y()),
                        diameter / 2.0, diameter / 2.0);

    // --- focus ring ---
    // Drawn outside the track, in the Windows 11 manner: a hairline gap and
    // then a ring in the foreground colour.
    if (hasFocus()) {
        const QRectF ring = track.adjusted(-kMargin + 0.5, -kMargin + 0.5,
                                           kMargin - 0.5, kMargin - 0.5);
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(m_trackBorder, 1.0));
        painter.drawRoundedRect(ring, ring.height() / 2.0, ring.height() / 2.0);
    }
}
