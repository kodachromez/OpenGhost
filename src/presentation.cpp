#include "presentation.h"

// Copied display markers used by RichDocument. No Ghosty transcript parser.
const QString &clippedNotice()
{
    static const QString text =
        QStringLiteral("\n\n[Display limit reached; additional text is omitted here.]");
    return text;
}

const QString &exhaustedNotice()
{
    static const QString text =
        QStringLiteral("[This run's display limit was reached; this text is not shown here.]");
    return text;
}
