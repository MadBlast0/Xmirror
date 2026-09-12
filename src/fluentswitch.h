#pragma once

#include <QAbstractButton>
#include <QColor>

class QPropertyAnimation;

// A Windows 11 style toggle.
//
// QCheckBox cannot be made to look like one through a stylesheet alone: its
// indicator is a fixed square sub-control, so the track and knob are painted
// here instead. It is otherwise a plain checkable QAbstractButton, which means
// setChecked()/isChecked()/toggled() behave exactly as they did on the
// QCheckBox instances this replaces.
//
// Every colour arrives from the stylesheet through qproperty- bindings, so the
// whole palette stays in fluenttheme.cpp rather than being hardcoded twice.
class FluentSwitch : public QAbstractButton {
    Q_OBJECT
    Q_PROPERTY(QColor accentColor MEMBER m_accent)
    Q_PROPERTY(QColor trackColor MEMBER m_track)
    Q_PROPERTY(QColor trackHoverColor MEMBER m_trackHover)
    Q_PROPERTY(QColor trackBorderColor MEMBER m_trackBorder)
    Q_PROPERTY(QColor knobColor MEMBER m_knob)
    Q_PROPERTY(QColor knobOnColor MEMBER m_knobOn)
    Q_PROPERTY(QColor disabledColor MEMBER m_disabled)
    Q_PROPERTY(qreal position READ position WRITE setPosition)

public:
    explicit FluentSwitch(QWidget *parent = nullptr);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override { return sizeHint(); }

    qreal position() const { return m_position; }
    void setPosition(qreal value);

protected:
    void paintEvent(QPaintEvent *event) override;
    // Called by QAbstractButton for every checked-state change, whether it came
    // from a click or from setChecked(), so the animation cannot be bypassed.
    void checkStateSet() override;
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    QPropertyAnimation *m_animation = nullptr;
    qreal m_position = 0.0;   // 0 = off, 1 = on
    bool m_hovered = false;

    QColor m_accent{0x00, 0x67, 0xc0};
    QColor m_track{0xff, 0xff, 0xff};
    QColor m_trackHover{0xf0, 0xf0, 0xf0};
    QColor m_trackBorder{0x8a, 0x8a, 0x8f};
    QColor m_knob{0x5d, 0x5d, 0x63};
    QColor m_knobOn{0xff, 0xff, 0xff};
    QColor m_disabled{0xc0, 0xc0, 0xc4};
};
