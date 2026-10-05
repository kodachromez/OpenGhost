#pragma once

#include <QColor>
#include <atomic>

// OpenGhost 1.3's two palettes (styles.css `:root` and `[data-theme="light"]`),
// for C++ painters and the QML Theme singleton. Painters read the one shown
// when they build; a theme change asks them to build again.
namespace theme
{
// Markdown's tones (.t-lilac … .t-green), in Markdown.TONES order. A message
// shuffles them; headings, quotes, tables and diagrams take them in turn.
constexpr int ToneCount = 7;

struct Palette {
    bool light;
    QColor fgBase; // --fg-rgb
    qreal primary, secondary, tertiary, quaternary;
    QColor appBg, chatBg, contourOuter, contourInner, composerBg, composerBorder;
    QColor muted, accent, onAccent; // --composer-fg/-accent/-on-accent
    QColor link, success, danger, warn, hover, rowActive, rowHover, selection, backdrop, tipBg,
        wellBg;
    QColor diffAdded, diffRemoved, noteBg, noteHoverBg, glow, paintShade;
    qreal shadow;   // --shadow: every drop shadow's strength is scaled by it
    QColor neutral; // .markdown's own --tone
    QColor tones[ToneCount];
    // Drawings (diagram.js): their ink's three strengths (--dg-ink, -2, -3)
    // and the series palette --s0 (what is left over) … --s7.
    qreal dgInk[3];
    QColor series[8];
    // Code highlighting (.hl-*); comments are the foreground at hlComment.
    QColor hlKeyword, hlString, hlNumber, hlFunction, hlType, hlProperty, hlInserted, hlDeleted;
    qreal hlComment;
    // --frame: the light that falls from the window's left corners onto
    // --app-bg, three radial gradients (FrameLight, in paint order from the
    // top: the top-left corner, the bottom-left one, the middle of the left
    // edge). Each is its colour at `alpha[i]` at `stop[i]` of the way out.
    struct FrameLight {
        QColor color;
        qreal alpha[3];
    };
    FrameLight frame[3];
    // The opening (--splash-*): the night it falls into; its mist, deep where
    // little light reaches and lit where much does; the light around the
    // Ghost, how much of it the trail takes on; the motes and how far the
    // edges sink into shade; the soft light standing in without shaders;
    // the name's glow.
    struct Splash {
        QColor night, mistDeep, mistLit, glow, core, aura, glowRgb;
        qreal mistAlpha, glowAlpha, trail, motes, shade;
    } splash;
};

// --frame's geometry, the same in both themes: radial-gradient(rx% ry% at
// cx% cy%, …) and each gradient's stops (its last is transparent at 1).
struct FrameShape {
    qreal rx, ry, cx, cy, stop[3];
};
constexpr FrameShape FrameShapes[3] = {{0.58, 0.72, 0, 0, {0, 0.38, 0.68}},
                                       {0.52, 0.64, 0, 1, {0, 0.38, 0.68}},
                                       {0.26, 0.36, 0.06, 0.5, {0, 0.5, 1}}};

inline QColor rgba(int r, int g, int b, qreal a)
{
    return QColor::fromRgbF(r / 255., g / 255., b / 255., a);
}

// :root (dark) and [data-theme="light"], token by token.
inline Palette makePalette(bool light)
{
    Palette p;
    p.light = light;
    p.fgBase = light ? QColor(0, 0, 0) : QColor(255, 255, 255);
    p.primary = 0.85;
    p.secondary = light ? 0.6 : 0.55;
    p.tertiary = light ? 0.32 : 0.25;
    p.quaternary = light ? 0.12 : 0.1;
    p.appBg = light ? QColor(240, 243, 248) : QColor(18, 19, 23);
    p.chatBg = light ? QColor(255, 255, 255) : QColor(25, 25, 25);
    p.contourOuter = light ? rgba(30, 48, 88, 0.1) : rgba(0, 0, 0, 0.5);
    p.contourInner = light ? QColor(255, 255, 255) : QColor(32, 32, 32);
    p.composerBg = light ? QColor(255, 255, 255) : QColor(30, 30, 30);
    p.composerBorder = light ? QColor(226, 226, 226) : QColor(34, 34, 34);
    p.muted = light ? QColor(118, 118, 118) : QColor(161, 161, 161);
    p.accent = light ? QColor(23, 23, 23) : QColor(250, 250, 250);
    p.onAccent = light ? QColor(255, 255, 255) : QColor(15, 15, 15);
    p.link = light ? QColor(22, 106, 214) : QColor(140, 190, 255);
    p.success = light ? QColor(22, 138, 70) : QColor(110, 205, 140);
    p.danger = light ? QColor(214, 48, 40) : QColor(255, 115, 105);
    p.warn = light ? QColor(200, 110, 0) : QColor(255, 180, 96);
    p.hover = light ? QColor(240, 240, 240) : QColor(36, 36, 36);
    p.rowActive = light ? QColor(255, 255, 255) : QColor(43, 46, 54);
    p.rowHover = light ? rgba(255, 255, 255, 0.5) : rgba(255, 255, 255, 0.055);
    // The selection wash (--selection-rgb) at Native's .35.
    p.selection = light ? QColor(40, 120, 235, 90) : QColor(130, 180, 245, 90);
    p.backdrop = rgba(0, 0, 0, light ? 0.2 : 0.5);
    p.tipBg = light ? rgba(255, 255, 255, 0.86) : rgba(36, 36, 38, 0.82);
    p.wellBg = rgba(0, 0, 0, light ? 0.04 : 0.28);
    p.diffAdded = light ? QColor(18, 120, 60) : QColor(192, 238, 206);
    p.diffRemoved = light ? QColor(176, 40, 32) : QColor(255, 188, 181);
    p.noteBg = light ? QColor(228, 228, 228) : QColor(60, 60, 60);
    p.noteHoverBg = light ? QColor(212, 212, 212) : QColor(80, 80, 80);
    p.glow = light ? QColor(0, 0, 0) : QColor(255, 255, 255);
    p.paintShade = light ? QColor(255, 255, 255) : QColor(0, 0, 0);
    p.shadow = light ? 0.32 : 1;
    // On white the tones go deeper, so text in them stays readable.
    p.neutral = light ? QColor(112, 112, 112) : QColor(150, 150, 150);
    const QColor tones[2][ToneCount] = {{{190, 160, 255},
                                         {72, 222, 200},
                                         {112, 172, 255},
                                         {255, 134, 192},
                                         {255, 212, 96},
                                         {255, 162, 98},
                                         {128, 218, 134}},
                                        {{128, 84, 226},
                                         {0, 146, 128},
                                         {36, 110, 226},
                                         {216, 48, 124},
                                         {222, 156, 0},
                                         {230, 106, 20},
                                         {30, 150, 70}}};
    for (int k = 0; k < ToneCount; ++k)
        p.tones[k] = tones[light][k];
    p.dgInk[0] = light ? 0.9 : 0.92;
    p.dgInk[1] = 0.64;
    p.dgInk[2] = light ? 0.56 : 0.48;
    const QColor series[2][8] = {{{122, 122, 122},
                                  {72, 133, 223},
                                  {202, 103, 25},
                                  {9, 148, 133},
                                  {134, 99, 198},
                                  {185, 137, 24},
                                  {215, 95, 148},
                                  {58, 143, 76}},
                                 {{140, 140, 140},
                                  {28, 101, 200},
                                  {202, 91, 4},
                                  {11, 142, 127},
                                  {109, 60, 179},
                                  {191, 134, 16},
                                  {209, 65, 134},
                                  {7, 120, 47}}};
    for (int k = 0; k < 8; ++k)
        p.series[k] = series[light][k];
    p.hlKeyword = light ? QColor(136, 72, 214) : QColor(198, 162, 255);
    p.hlString = light ? QColor(28, 126, 56) : QColor(150, 222, 150);
    p.hlNumber = light ? QColor(192, 88, 0) : QColor(255, 172, 112);
    p.hlFunction = light ? QColor(22, 100, 208) : QColor(122, 182, 255);
    p.hlType = light ? QColor(158, 104, 0) : QColor(255, 214, 112);
    p.hlProperty = light ? QColor(0, 126, 116) : QColor(112, 220, 206);
    p.hlInserted = light ? QColor(28, 126, 56) : QColor(128, 218, 134);
    p.hlDeleted = light ? QColor(200, 44, 44) : QColor(255, 128, 128);
    p.hlComment = light ? 0.45 : 0.38;
    p.splash.night = light ? QColor(243, 244, 250) : QColor(13, 14, 22);
    p.splash.mistDeep = light ? QColor(200, 203, 230) : QColor(26, 30, 48);
    p.splash.mistLit = light ? QColor(255, 255, 255) : QColor(150, 168, 240);
    p.splash.glow = light ? QColor(124, 116, 228) : QColor(150, 170, 255);
    p.splash.core = light ? QColor(88, 80, 204) : QColor(232, 238, 255);
    p.splash.aura = light ? rgba(166, 186, 242, 0.4) : rgba(86, 104, 172, 0.34);
    p.splash.glowRgb = light ? QColor(92, 118, 230) : QColor(150, 172, 255);
    p.splash.mistAlpha = light ? 0.75 : 0.55;
    p.splash.glowAlpha = light ? 0.42 : 0.5;
    p.splash.trail = light ? 0.9 : 0.35;
    p.splash.motes = light ? 0 : 1;
    p.splash.shade = light ? 0.07 : 0.5;
    if (light) {
        p.frame[0] = {QColor(198, 213, 236), {0.9, 0.48, 0.13}};
        p.frame[1] = {QColor(210, 207, 238), {0.85, 0.42, 0.11}};
        p.frame[2] = {QColor(250, 252, 255), {0.75, 0.28, 0}};
    } else {
        p.frame[0] = {QColor(62, 76, 106), {0.5, 0.26, 0.08}};
        p.frame[1] = {QColor(58, 55, 94), {0.42, 0.2, 0.06}};
        p.frame[2] = {QColor(50, 64, 82), {0.2, 0.07, 0}};
    }
    return p;
}

inline const Palette &palette(bool light)
{
    static const Palette dark = makePalette(false), bright = makePalette(true);
    return light ? bright : dark;
}

// The theme on screen, process-wide; painters on worker threads read it too.
inline std::atomic_bool lightShown{false};
inline const Palette &current() { return palette(lightShown.load(std::memory_order_relaxed)); }

inline QColor fg(qreal opacity)
{
    QColor color = current().fgBase;
    color.setAlphaF(opacity);
    return color;
}
} // namespace theme
