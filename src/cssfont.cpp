#include "cssfont.h"

#include <QFont>
#include <QFontDatabase>
#include <QFontInfo>
#include <QHash>
#include <QMutex>

#include <algorithm>
#include <cmath>

namespace cssfont
{
int weight(const QString &family, int weight)
{
    static QMutex mutex;
    static QHash<QPair<QString, int>, int> resolved;
    const QMutexLocker lock(&mutex);
    const auto key = qMakePair(family, weight);
    if (const auto found = resolved.constFind(key); found != resolved.cend())
        return *found;
    // A generic name (monospace) matches through fontconfig.
    const QString real = QFontInfo(QFont(family)).family();
    QList<int> faces;
    const auto styles = QFontDatabase::styles(real);
    for (const auto &style : styles) {
        if (!QFontDatabase::italic(real, style))
            faces.append(QFontDatabase::weight(real, style));
    }
    std::sort(faces.begin(), faces.end());
    // CSS Fonts 4 §5.2: 400–500 tries up to 500, then lighter, then
    // heavier; below 400 lighter first; above 500 heavier first.
    int lighter = -1, heavier = -1, upTo500 = -1;
    for (const int face : std::as_const(faces)) {
        if (face < weight)
            lighter = face;
        else if (face > weight && heavier < 0)
            heavier = face;
        if (face > weight && face <= 500 && upTo500 < 0)
            upTo500 = face;
    }
    int chosen = weight;
    if (!faces.isEmpty() && !faces.contains(weight)) {
        if (weight >= 400 && weight <= 500)
            chosen = upTo500 >= 0 ? upTo500 : lighter >= 0 ? lighter : heavier;
        else if (weight < 400)
            chosen = lighter >= 0 ? lighter : heavier;
        else
            chosen = heavier >= 0 ? heavier : lighter;
    }
    resolved.insert(key, chosen);
    return chosen;
}

namespace
{
// fontconfig's weight scale for an OpenType weight (FcWeightFromOpenType).
double fcWeight(int weight)
{
    static const int ot[] = {100, 200, 300, 350, 380, 400, 500, 600, 700, 800, 900};
    static const int fc[] = {0, 40, 50, 55, 75, 80, 100, 180, 200, 205, 210};
    if (weight <= ot[0])
        return fc[0];
    for (int i = 1; i < 11; ++i) {
        if (weight <= ot[i])
            return fc[i - 1] +
                   double(fc[i] - fc[i - 1]) * (weight - ot[i - 1]) / (ot[i] - ot[i - 1]);
    }
    return fc[10];
}
} // namespace

int nearest(const QString &family, int weight)
{
    static QMutex mutex;
    static QHash<QPair<QString, int>, int> resolved;
    const QMutexLocker lock(&mutex);
    const auto key = qMakePair(family, weight);
    if (const auto found = resolved.constFind(key); found != resolved.cend())
        return *found;
    const QString real = QFontInfo(QFont(family)).family();
    int chosen = weight;
    double best = INFINITY;
    for (const auto &style : QFontDatabase::styles(real)) {
        if (QFontDatabase::italic(real, style))
            continue;
        const int face = QFontDatabase::weight(real, style);
        const double d = std::abs(fcWeight(face) - fcWeight(weight));
        // A tie goes to the heavier face.
        if (d < best || (d == best && chosen < face)) {
            best = d;
            chosen = face;
        }
    }
    resolved.insert(key, chosen);
    return chosen;
}
} // namespace cssfont
