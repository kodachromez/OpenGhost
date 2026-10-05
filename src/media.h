#pragma once

#include <QSize>
#include <QString>
#include <QStringList>

// Pictures and videos shown in a reply (OpenGhost's media-embed.js and
// media-slider.js): which addresses are videos, which pictures load by
// themselves, what a video link's words say, and the shape a stack of
// pictures takes. Pure; nothing here loads anything.
namespace media
{
// media-embed.js LOAD_TIMEOUT: a picture that has not come by then is lost.
constexpr int LoadTimeout = 9000;
// What YouTube sends in place of a preview that does not exist is this wide.
constexpr int NoThumb = 120;
// media-slider.js FRAME, RATIO.
constexpr int FrameWidth = 380, FrameHeight = 340;
constexpr double SingleRatio[2] = {0.5, 2.2}, ManyRatio[2] = {0.75, 1.6};
constexpr double FallbackRatio = 4.0 / 3.0, RatioMatch = 0.02;

// The eleven-character id of a YouTube video link, or empty.
QString videoId(const QString &url);

// A picture from where search engines keep their previews or from
// Wikimedia's library loads by itself; any other waits for a click.
bool trusted(const QString &url);

// http(s) only: the addresses a picture may come from at all.
bool fetchable(const QString &url);

// Where a load may be sent on: a trusted picture only on to another trusted
// place; one the person asked for, on to any http(s) address.
bool redirectAllowed(const QString &to, bool consented);

// The host without "www.", as a held picture names it.
QString hostOf(const QString &url);

// The previews a video card tries, widest first.
QStringList thumbnails(const QString &id);

// The words of a link to a video may carry who made it and how long it is,
// parted by dots: Title · Channel · 4:40. A bare address has none.
struct VideoWords {
    QString title, by, time;
};
VideoWords videoWords(const QString &text, const QString &url);

// The ratio a stack shows its pictures at, its first picture's clamped to
// what one picture or several may take.
double stackRatio(double raw, bool many);
// The stack's width for that ratio, within FRAME.
int stackWidth(double ratio);
// Whether a picture of its own `size` needs a blurred backdrop to fill a
// frame of `ratio` (it does not when the shapes match).
bool needsBackdrop(QSize size, double ratio);
} // namespace media
