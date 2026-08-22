#include "selenetheme.h"

#include <QSettings>

#include <array>
#include <atomic>

namespace {
struct ThemeDefinition
{
    const char* id;
    const char* name;
    bool dark;
    SelenePalette palette;
};

const std::array<ThemeDefinition, 3> Themes {{
    {
        "moonlit-orbit", "Moonlit Orbit", true,
        {
            QColor(0x08, 0x0B, 0x12),       // backdrop
            QColor(0x12, 0x18, 0x24, 0xF5), // surface
            QColor(0x1B, 0x23, 0x33),       // raised
            QColor(0x2C, 0x38, 0x50),       // border
            QColor(0xF5, 0xF7, 0xFF),       // text
            QColor(0xA9, 0xB6, 0xCD),       // muted
            QColor(0xA9, 0xB6, 0xCD, 0x72), // disabled
            QColor(0x91, 0xA6, 0xFF),       // accent
            QColor(0x58, 0x6D, 0xC2, 0xE6), // selection
            QColor(0xFF, 0xFF, 0xFF, 0x16), // hover
            QColor(0x6D, 0xD6, 0xA7),       // success
            QColor(0xF1, 0xC2, 0x66),       // warning
            QColor(0xFF, 0x7E, 0x88),       // danger
        },
    },
    {
        "polar-dawn", "Polar Dawn", false,
        {
            QColor(0xE9, 0xF0, 0xF7),       // backdrop
            QColor(0xF8, 0xFB, 0xFF, 0xF8), // surface
            QColor(0xFF, 0xFF, 0xFF),       // raised
            QColor(0xC4, 0xD1, 0xDF),       // border
            QColor(0x13, 0x20, 0x31),       // text
            QColor(0x55, 0x65, 0x79),       // muted
            QColor(0x55, 0x65, 0x79, 0x72), // disabled
            QColor(0x3F, 0x62, 0xC8),       // accent
            QColor(0xB7, 0xC9, 0xF7),       // selection
            QColor(0x20, 0x3A, 0x60, 0x12), // hover
            QColor(0x20, 0x8B, 0x68),       // success
            QColor(0xA8, 0x6C, 0x10),       // warning
            QColor(0xC9, 0x3E, 0x52),       // danger
        },
    },
    {
        "velvet-circuit", "Velvet Circuit", true,
        {
            QColor(0x11, 0x09, 0x15),       // backdrop
            QColor(0x20, 0x12, 0x29, 0xF5), // surface
            QColor(0x2E, 0x1A, 0x3A),       // raised
            QColor(0x4B, 0x2D, 0x5C),       // border
            QColor(0xFF, 0xF5, 0xFD),       // text
            QColor(0xC8, 0xA9, 0xC7),       // muted
            QColor(0xC8, 0xA9, 0xC7, 0x72), // disabled
            QColor(0xED, 0x8F, 0xD1),       // accent
            QColor(0x89, 0x43, 0x87, 0xE6), // selection
            QColor(0xFF, 0xFF, 0xFF, 0x16), // hover
            QColor(0x77, 0xD8, 0xB0),       // success
            QColor(0xF3, 0xC5, 0x72),       // warning
            QColor(0xFF, 0x77, 0x92),       // danger
        },
    },
}};

std::atomic<int> ActiveTheme {0};

int themeIndexForId(const QString& id)
{
    for (std::size_t i = 0; i < Themes.size(); ++i) {
        if (id == QLatin1String(Themes[i].id)) {
            return static_cast<int>(i);
        }
    }
    return 0;
}
}

SeleneTheme::SeleneTheme(QObject* parent)
    : QObject(parent)
{
    QSettings settings;
    ActiveTheme.store(themeIndexForId(settings.value(QStringLiteral("ui/theme"),
                                                        QStringLiteral("moonlit-orbit")).toString()));
}

const SelenePalette& SeleneTheme::palette()
{
    return Themes[static_cast<std::size_t>(ActiveTheme.load())].palette;
}

QString SeleneTheme::currentTheme() const
{
    return QLatin1String(Themes[static_cast<std::size_t>(currentThemeIndex())].id);
}

void SeleneTheme::setCurrentTheme(const QString& id)
{
    const int index = themeIndexForId(id);
    if (index == currentThemeIndex()) {
        return;
    }

    ActiveTheme.store(index);
    QSettings().setValue(QStringLiteral("ui/theme"), QLatin1String(Themes[index].id));
    emit themeChanged();
}

int SeleneTheme::currentThemeIndex() const
{
    return ActiveTheme.load();
}

QStringList SeleneTheme::themeIds() const
{
    QStringList ids;
    for (const auto& theme : Themes) {
        ids.append(QLatin1String(theme.id));
    }
    return ids;
}

QStringList SeleneTheme::themeNames() const
{
    QStringList names;
    for (const auto& theme : Themes) {
        names.append(QLatin1String(theme.name));
    }
    return names;
}

bool SeleneTheme::dark() const
{
    return Themes[static_cast<std::size_t>(currentThemeIndex())].dark;
}

void SeleneTheme::selectThemeAt(int index)
{
    if (index < 0 || index >= static_cast<int>(Themes.size())) {
        return;
    }
    setCurrentTheme(QLatin1String(Themes[static_cast<std::size_t>(index)].id));
}
