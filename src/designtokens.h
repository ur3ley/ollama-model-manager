#ifndef DESIGNTOKENS_H
#define DESIGNTOKENS_H

#include <QString>

// Single source of truth for the hub's colours and shapes. Widgets build their
// style sheets from these values instead of hard-coding hex strings, so a later
// visual pass (or a dark theme) changes one file.
namespace tokens {

inline const QString Accent = QStringLiteral("#2563eb");
inline const QString AccentSoft = QStringLiteral("#c7d2fe");
inline const QString Text = QStringLiteral("#111827");
inline const QString Muted = QStringLiteral("#6b7280");
inline const QString Border = QStringLiteral("#e5e7eb");
inline const QString Surface = QStringLiteral("#ffffff");
inline const QString Card = QStringLiteral("#f7f8fa");
inline const QString CardHover = QStringLiteral("#f1f3f6");
inline const QString CardSelected = QStringLiteral("#eef4ff");
inline const QString Danger = QStringLiteral("#b91c1c");

inline const int CardRadius = 10;
inline const int ControlRadius = 8;

inline QString comboStyle()
{
    return QStringLiteral("QComboBox { background:%1; border:1px solid %2;"
                          " border-radius:%3px; padding:7px 10px; }")
            .arg(Card, Border).arg(ControlRadius);
}

inline QString primaryButtonStyle()
{
    return QStringLiteral("QPushButton { background:%1; color:white; border:none;"
                          " border-radius:%2px; padding:8px 18px; font-weight:600; }"
                          "QPushButton:disabled { background:%3; }")
            .arg(Accent).arg(ControlRadius).arg(AccentSoft);
}

inline QString ghostButtonStyle()
{
    return QStringLiteral("QPushButton { background:%1; color:%2; border:1px solid %3;"
                          " border-radius:%4px; padding:7px 14px; }")
            .arg(Surface, Text, Border).arg(ControlRadius);
}

inline QString progressStyle()
{
    return QStringLiteral("QProgressBar { border:1px solid %1; border-radius:6px;"
                          " background:#f3f4f6; height:14px; text-align:center;"
                          " font-size:10px; }"
                          "QProgressBar::chunk { background:%2; border-radius:5px; }")
            .arg(Border, Accent);
}

inline QString chipStyle(const QString &kind)
{
    QString colors;
    if (kind == QLatin1String("vision"))
        colors = QStringLiteral("background:#eef0ff;color:#4f46e5;");
    else if (kind == QLatin1String("thinking"))
        colors = QStringLiteral("background:#f3e8ff;color:#9333ea;");
    else if (kind == QLatin1String("tools"))
        colors = QStringLiteral("background:#e8f1ff;color:#1d4ed8;");
    else if (kind == QLatin1String("embedding"))
        colors = QStringLiteral("background:#ecfdf5;color:#047857;");
    else if (kind == QLatin1String("gguf"))
        colors = QStringLiteral("background:#f1f5f9;color:#475569;");
    else
        colors = QStringLiteral("background:#f3f4f6;color:#4b5563;");
    return QStringLiteral("QLabel { %1 border-radius:9px; padding:2px 9px; font-size:11px; }")
            .arg(colors);
}

} // namespace tokens

#endif // DESIGNTOKENS_H
