#pragma once

#include <QString>

namespace cssfont
{
// The weight of `family`'s face CSS Fonts 4 §5.2 picks for a requested
// weight: 400–500 tries up to 500, then lighter, then heavier; below 400
// lighter first; above 500 heavier first. Thread-safe.
int weight(const QString &family, int weight);
// The face fontconfig finds nearest on its own weight scale, a tie going to
// the heavier: what Chromium draws on Linux for a weight between faces
// (550 is Medium where there is no 600). Thread-safe.
int nearest(const QString &family, int weight);
} // namespace cssfont
