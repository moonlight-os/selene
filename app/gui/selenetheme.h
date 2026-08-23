#pragma once

#include <QColor>
#include <QObject>
#include <QStringList>

// Semantic colours shared by Selene's QML interface and anything rendered
// outside it, notably the in-stream QPainter panel. Components ask what a
// colour means instead of naming a Material swatch, so a future theme can
// change character without every screen learning a new palette by hand.
struct SelenePalette
{
    QColor backdrop;
    QColor surface;
    QColor raised;
    QColor border;
    QColor text;
    QColor muted;
    QColor disabled;
    QColor accent;
    QColor selection;
    QColor hover;
    QColor success;
    QColor warning;
    QColor danger;
};

class SeleneTheme : public QObject
{
    Q_OBJECT

    Q_PROPERTY(QString currentTheme READ currentTheme WRITE setCurrentTheme NOTIFY themeChanged)
    Q_PROPERTY(int currentThemeIndex READ currentThemeIndex NOTIFY themeChanged)
    Q_PROPERTY(QStringList themeIds READ themeIds CONSTANT)
    Q_PROPERTY(QStringList themeNames READ themeNames CONSTANT)
    Q_PROPERTY(bool dark READ dark NOTIFY themeChanged)
    Q_PROPERTY(QColor backdrop READ backdrop NOTIFY themeChanged)
    Q_PROPERTY(QColor surface READ surface NOTIFY themeChanged)
    Q_PROPERTY(QColor raised READ raised NOTIFY themeChanged)
    Q_PROPERTY(QColor border READ border NOTIFY themeChanged)
    Q_PROPERTY(QColor text READ text NOTIFY themeChanged)
    Q_PROPERTY(QColor muted READ muted NOTIFY themeChanged)
    Q_PROPERTY(QColor disabled READ disabled NOTIFY themeChanged)
    Q_PROPERTY(QColor accent READ accent NOTIFY themeChanged)
    Q_PROPERTY(QColor selection READ selection NOTIFY themeChanged)
    Q_PROPERTY(QColor hover READ hover NOTIFY themeChanged)
    Q_PROPERTY(QColor success READ success NOTIFY themeChanged)
    Q_PROPERTY(QColor warning READ warning NOTIFY themeChanged)
    Q_PROPERTY(QColor danger READ danger NOTIFY themeChanged)

public:
    explicit SeleneTheme(QObject* parent = nullptr);

    static const SelenePalette& palette();

    QString currentTheme() const;
    void setCurrentTheme(const QString& id);
    int currentThemeIndex() const;
    QStringList themeIds() const;
    QStringList themeNames() const;
    bool dark() const;

    Q_INVOKABLE void selectThemeAt(int index);

    QColor backdrop() const { return palette().backdrop; }
    QColor surface() const { return palette().surface; }
    QColor raised() const { return palette().raised; }
    QColor border() const { return palette().border; }
    QColor text() const { return palette().text; }
    QColor muted() const { return palette().muted; }
    QColor disabled() const { return palette().disabled; }
    QColor accent() const { return palette().accent; }
    QColor selection() const { return palette().selection; }
    QColor hover() const { return palette().hover; }
    QColor success() const { return palette().success; }
    QColor warning() const { return palette().warning; }
    QColor danger() const { return palette().danger; }

signals:
    void themeChanged();
};
