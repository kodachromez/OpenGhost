#pragma once

// The drawing engine behind diagram.h: OpenGhost 1.3's diagram.js (v1.3.0,
// 9072eaf) parsed, laid out and turned into a scene of keyed items, as
// upstream's scene functions return them, for diagram_paint.cpp to draw and
// DiagramImage to animate. Shared by the diagram_*.cpp files only.
//
// Porting conventions, one per upstream idiom:
// - A scene item ({ key, type, layer, order, props, fixed }) is a Spec. Its
//   props are the numbers that spring when a drawing changes (Props), its
//   fixed part what is drawn as is (Fixed). Field names are upstream's, but
//   for these clashes: a node's `head` (count of strong lines) is
//   `Fixed::headLines`, an edge's `head` (arrow kind) `Fixed::head`; a
//   candle's props `h`/`l` are `Props::hi`/`lo`; a files header's `size`
//   text is `Fixed::sizeText`; a files segment's `tone` (an rgb string from
//   file-kinds.js) is `Fixed::color`.
// - A tone is a slot of the series palette: 1..7 for 's1'..'s7', Mute (0)
//   for 'mute' (--s0), NoTone for none. `tones` is the ring of slots a
//   drawing takes them from (tonesOf), its first the section's colour.
// - `draw: true` on a line is Fixed::draw = 1; an edge's `draw: 900` is 900
//   (an edge's draw is its duration, as upstream's `fx.draw || …`).
// - Text widths are Ctx::textWidth (canvas measureText at size px, weight).
// - Untrusted input: every loop over source lines or derived counts is
//   bounded (Ctx::budget, the Max* limits); nothing may index out of range,
//   divide by zero into a NaN that reaches geometry, or recurse unbounded.
//   A scene that cannot be drawn returns std::nullopt (upstream's null).

#include "diagram.h"

#include <QColor>
#include <QFont>
#include <QFontMetricsF>
#include <QHash>
#include <QImage>
#include <QPainterPath>
#include <QPointF>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QVector>

#include <array>
#include <atomic>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>

namespace diagram
{
using Re = QRegularExpression;
constexpr auto I = Re::CaseInsensitiveOption;
constexpr double NaN = std::numeric_limits<double>::quiet_NaN();

/* Constants (diagram.js) */

struct TextSet {
    double size, line;
    int weight;
};
constexpr TextSet TEXT{12.5, 17, 500};
struct NodeSet {
    double padX, padY, maxWidth, minWidth, radius, detail, part;
};
constexpr NodeSet NODE{14, 9, 180, 52, 8, 15, 3};
struct FlowGapSet {
    double rank, rankSide, node, label;
};
constexpr FlowGapSet FLOW_GAP{48, 58, 24, 12};
struct ArrowSet {
    double length, half;
};
constexpr ArrowSet ARROW{6.5, 3.4};
constexpr double BULGE = 34, TALL = 90;
struct SweepSet {
    int order, place;
};
constexpr SweepSet SWEEPS{12, 24};
struct EnterSet {
    double node, label, fade, line, grow, arc, speed, drawMin, drawMax;
};
constexpr EnterSet ENTER{460, 340, 380, 620, 600, 860, 0.6, 220, 600};
constexpr double EXIT = 260;
struct StaggerSet {
    double reveal, live, cap;
};
constexpr StaggerSet STAGGER{220, 90, 1600};
constexpr double SPRING_K = 170, SPRING_C = 24;
constexpr double GLIDE_MOVE_K = 520, GLIDE_MOVE_C = 40, GLIDE_FADE_K = 320, GLIDE_FADE_C = 32;
constexpr double PAD = 18, PAD_FLUSH = 10, MIN_SCALE = 0.55, SIDE_FIT = 0.8;
struct TurnSet {
    double fit, gain;
};
constexpr TurnSet TURN{0.8, 0.1};
struct BesideSet {
    double tall, ratio, gain;
};
constexpr BesideSet BESIDE{460, 1.5, 0.72};
struct ClusterSet {
    double padX, head, padBottom, empty, min, radius;
};
constexpr ClusterSet CLUSTER{14, 34, 14, 22, 130, 12};
constexpr double STATUS_HEIGHT = 56;
struct PieSet {
    double radius, width, gap, legend, row, swatch;
    int max;
    double table, head;
};
constexpr PieSet PIE{56, 12, 0.55, 34, 26, 8, 7, 320, 32};
struct ChartSet {
    double plot, bar;
    int labeled, dots;
    double max, head;
};
constexpr ChartSet CHART{208, 22, 12, 12, 1160, 30};
struct SeqSet {
    double head, pad, gap, self;
};
constexpr SeqSet SEQ{34, 14, 132, 34};
struct CandleSet {
    double maxWidth, price, volume, gap, head, body, maxBody;
};
constexpr CandleSet CANDLE{1000, 236, 46, 12, 46, 0.6, 12};
struct TimelineSet {
    double col, minCol, line;
};
constexpr TimelineSet TIMELINE{200, 140, 17};
struct GanttSet {
    double row, section, bar, axis, maxWidth, label;
};
constexpr GanttSet GANTT{30, 28, 8, 28, 1000, 230};
struct MindSet {
    double gapX, gapY, branch;
};
constexpr MindSet MIND{46, 8, 14};
struct QuadSet {
    double max, min;
};
constexpr QuadSet QUAD{480, 280};
struct RadarSet {
    double min, max;
    int levels;
};
constexpr RadarSet RADAR{84, 132, 4};
struct CardSet {
    double head, stereo, row, padX, min;
};
constexpr CardSet CARD{34, 44, 22, 12, 140};
struct LedgerSet {
    double max, head, row, noted, bar, pitch;
};
constexpr LedgerSet LEDGER{720, 30, 28, 40, 6, 13};
struct MetricsSet {
    double max, head, min, gap;
    std::array<double, 4> sizes;
};
constexpr MetricsSet METRICS{1100, 32, 124, 28, {24, 20, 17, 15}};
struct FoodSet {
    double max, ring, width, row, meal, split;
};
constexpr FoodSet FOOD{720, 44, 9, 28, 30, 64};
struct FactsSet {
    double max, row;
};
constexpr FactsSet FACTS{720, 32};
struct CheckSet {
    double mark, gap;
};
constexpr CheckSet CHECK{10, 12};
struct OutlineSet {
    double indent, gap;
};
constexpr OutlineSet OUTLINE{22, 8};
struct MatchSet {
    double row, noted;
};
constexpr MatchSet MATCH{32, 46};
struct RangesSet {
    double max, head, row;
    std::array<double, 2> zone;
};
constexpr RangesSet RANGES{720, 30, 44, {0.3, 0.7}};
struct BoardSet {
    double min, max, gap, pad, head;
};
constexpr BoardSet BOARD{150, 250, 14, 10, 32};
struct StepsSet {
    double max, head, mark, gap;
};
constexpr StepsSet STEPS{720, 32, 11, 14};
struct SankeySet {
    double max, node, gap, lane;
};
constexpr SankeySet SANKEY{760, 8, 14, 46};
struct HeatSet {
    double cell, gap, row;
    int levels;
};
constexpr HeatSet HEAT{14, 3, 28, 4};
struct ScatterSet {
    double plot, max;
};
constexpr ScatterSet SCATTER{300, 720};
struct TreeSet {
    double max, low, high, gap;
};
constexpr TreeSet TREE{720, 220, 340, 2};
struct GitSet {
    double max, row, lane;
};
constexpr GitSet GIT{720, 28, 18};
struct CellsSet {
    double max, min, wide, height, gap;
};
constexpr CellsSet CELLS{720, 34, 120, 32, 3};
struct BracketSet {
    double card, gap, min, max;
};
constexpr BracketSet BRACKET{50, 14, 124, 200};
constexpr double DAY = 86400000;
struct ToolsSet {
    double width, height, gap, reach;
};
constexpr ToolsSet TOOLS{68, 36, 10, 24};
constexpr double NEAR = 22;
// Small capitals are spaced by this much of their size.
constexpr double TRACK = 0.06;
// A drawing is laid out in these sizes and shown larger, as the chat's type
// (16 px) is to 14, within [1, 1.3].
struct TypeSet {
    double base, min, max;
};
constexpr TypeSet TYPE{14, 1, 1.3};

// How text is set, [size, weight] (diagram.js FONT).
struct Font {
    double size;
    int weight;
};
namespace FONT
{
constexpr Font title{13.5, 650}, eyebrow{10.5, 650}, tick{11, 450}, value{11, 600},
    small{11.5, 500}, legend{12, 500}, amount{12, 600}, cardTitle{12.5, 650}, cardSub{10.5, 500},
    cardText{12, 500}, cardMeta{11, 450}, cardBadge{9.5, 650}, message{12, 500}, note{12, 450},
    period{13, 650}, event{12.5, 450}, task{12.5, 500}, point{12, 500}, figure{24, 650},
    unit{12.5, 450}, center{20, 650}, row{12.5, 500}, strong{12.5, 650}, step{13, 500},
    change{11.5, 600}, cell{12, 400}, detail{11.5, 450};
}

// Bounds for untrusted source.
constexpr int MaxLines = 4000, MaxNodes = 300, MaxEdges = 600, MaxGroups = 64, MaxGroupDepth = 12,
              MaxVerts = 6000, MaxLabel = 400, MaxActors = 60, MaxEvents = 800, MaxFrameDepth = 24,
              MaxSlices = 120, MaxSeries = 16, MaxValues = 1000, MaxRows = 3000, MaxAverages = 8,
              MaxPeriods = 120, MaxPeriodEvents = 60, MaxTasks = 400, MaxMindNodes = 500,
              MaxMindDepth = 24, MaxPoints = 400, MaxAxes = 40, MaxCurves = 16, MaxMembers = 120,
              MaxLoop = 2000, MaxItems = 20000, MaxCells = 4000, MaxDepth = 24;
constexpr double MaxWidth = 4096, MaxHeight = 16384, MaxPixels = 16.0 * 1024 * 1024;

/* Tones */

enum Tone : int { NoTone = -1, Mute = 0 };
using Tones = QVector<int>; // Slots 1..7 in the order a drawing takes them.
// SERIES: s1 … s7.
Tones series();
// The colour of the series that comes at place i: a slot while there are
// slots, grey after that.
inline int slot(const Tones &tones, int i)
{
    return i >= 0 && i < tones.size() ? tones.at(i) : int(Mute);
}
// Several things side by side take the palette from its start.
inline Tones ring(const Tones &tones, int count) { return count > 1 ? series() : tones; }
inline int first(const Tones &tones) { return tones.isEmpty() ? 1 : tones.first(); }

/* Scene items */

enum class Type {
    View,
    Node,
    Edge,
    Label,
    Line,
    Bar,
    Area,
    Arc,
    Legend,
    Dot,
    Badge,
    Note,
    Frame,
    Cluster,
    Candle,
    Column,
    Span,
    Rect,
    Poly,
    Chip,
    Hit,
    Figure,
    Ribbon,
    WBox,
    WGlyph,
    FCard,
    FHead,
    FSeg,
    FRow
};
enum Layer : int { Back, Edges, Nodes, Labels, Front, Layers, DefaultLayer = -1 };
using Pts = QVector<double>; // x0,y0 then three points per cubic segment.

// The numbers of an item that spring from where they are to where they go.
struct Props {
    double x = 0, y = 0, w = 0, h = 0, x1 = 0, y1 = 0, x2 = 0, y2 = 0, r = 0, top = 0, base = 0,
           start = 0, sweep = 0, s = 0, o = 0, c = 0, hi = 0, lo = 0;
    // The view's own (DiagramView.viewSpec).
    double ox = 0, cw = 0, tx = 0, ty = 0;
    Pts pts, vs, band;
};
// Every scalar of Props, for the springs.
constexpr double Props::*PropScalars[] = {
    &Props::x,     &Props::y,  &Props::w,  &Props::h,   &Props::x1,   &Props::y1,
    &Props::x2,    &Props::y2, &Props::r,  &Props::top, &Props::base, &Props::start,
    &Props::sweep, &Props::s,  &Props::o,  &Props::c,   &Props::hi,   &Props::lo,
    &Props::ox,    &Props::cw, &Props::tx, &Props::ty};

// A table of a database or a class of code (cardOf).
struct CardRow {
    QString lead, text, meta;
    bool badge = false;
};
struct Card {
    double w = 0, h = 0, head = 0, leadW = 0;
    QString title, sub;
    QVector<CardRow> rows;
    int sep = -1;
    QString key;
};

// What an item draws as it is: every type's fixed fields in one record,
// upstream's names (see the conventions above for the renamed ones).
struct Fixed {
    QString cls;
    int tone = NoTone;
    QStringList lines;
    QString anchor = QStringLiteral("middle"), baseline = QStringLiteral("central");
    double lineHeight = 0; // 0: 16 (textBlock's default).
    double size = 0;       // A label's own font size (0: its class's).
    int weight = 0;
    bool pill = false, pop = false;
    double w = 0, h = 0;
    double rx = NaN; // NaN: the type's default.
    double rotate = 0;
    double draw = 0; // A line: drawn along (1); an edge: its drawing time.
    bool grow = false;
    // Node.
    QString shape = QStringLiteral("rect"), id;
    int headLines = 0;
    bool key = false;
    std::shared_ptr<const Card> card;
    QString cardKey;
    // Edge: its line, its ends, and the ids of the blocks it joins (a, b).
    QString style = QStringLiteral("solid"), head = QStringLiteral("none");
    bool both = false, hasEnds = false;
    QString endStart, endEnd; // `ends` when hasEnds.
    QString a, b;
    // Arc and poly.
    double cx = 0, cy = 0, r = NaN, width = 0, gap = 0;
    int index = -1;
    bool active = false;
    // Legend.
    QString label, value, share;
    double valueX = 0;
    // Badge.
    QString n;
    // Frame.
    QString kind;
    // Cluster.
    QString title;
    int depth = 0;
    // Span.
    bool milestone = false, back = false, mid = false;
    // Chip.
    QString text, mark = QStringLiteral("dot");
    // Figure.
    QString figure, unit;
    // Wireframe glyph.
    QString icon;
    // Files: header, segment, row.
    QString name, path, counts, sizeText;
    double nameW = 0, left = 0, right = 0, y = 0;
    QColor color;
    double plateX = 0, plateW = 0, iconX = 0, textX = 0, labelW = 0, dateX = 0, sizeX = 0, rule = 0;
    bool folder = false;
    QString note, when, amount;
};

struct Spec {
    Type type = Type::Label;
    QString key;
    int layer = DefaultLayer;
    double order = 0;
    Props props;
    std::optional<Props> initial;
    Fixed fixed;
};

// The layer a spec of this type goes to when it names none.
int layerOf(const Spec &spec);

/* What a scene says besides its items */

struct TipRow {
    QString name, value, cls;
    int tone = NoTone;
    QString mark; // dot (default), bar, line
};
struct Tip {
    QString title;
    QVector<TipRow> rows;
    QStringList hot; // Keys lit with it; empty: the item's own.
    bool silent = false;
};
// A chart read by the pointer's x: the nearest column's tip.
struct Probe {
    double x0 = 0, x1 = 0, y0 = 0, y1 = 0;
    QVector<double> xs;
    QVector<Tip> tips;
    QVector<QStringList> keys;
};
struct NearPoint {
    double x = 0, y = 0;
    QString key;
};
// A ring whose slices answer the pointer: the centre says the one in focus.
struct PieFocus {
    int count = 0, top = 0;
    QStringList values, labels;
};
struct Corner {
    double x = 0, h = 0, inset = 0;
};
using Hints = QHash<QString, double>;

struct Result {
    QString kind, dialect;
    double width = 0, height = 0, zoom = 1;
    bool flush = false, mobile = false, edges = false;
    std::optional<Corner> corner;
    QVector<Spec> items;
    QHash<QString, Tip> tips;
    QVector<NearPoint> near;
    std::optional<Probe> probe;
    std::optional<PieFocus> pie;
    Hints hints;
    QString sideways; // "", "beside" or the direction a scheme was turned from.
    QHash<QString, QString> labels;
};

/* Context */

struct Options; // diagram.h
struct CompileOptions {
    double width = 640; // The room on screen, logical px.
    double column = 0;  // The text column inside it (0: the whole width).
    double zoom = 1;    // Shown this much larger than laid out.
    QString kind;       // The fence's kind hint.
    Hints hints;        // A scheme's places from its last layout.
    QString sideways;
    QString family, mono;
    const std::atomic_bool *cancel = nullptr;
};

// A face at a CSS px size. Qt sets a font in whole pixels, so it is made at
// Scale times the size and measured and drawn scaled down, which keeps the
// fractional sizes (12.5 px) and advances Chromium uses.
struct Face {
    static constexpr double Scale = 8;
    QFont font; // At Scale × the size.
    QFontMetricsF metrics;
    double advance(const QString &text) const { return metrics.horizontalAdvance(text) / Scale; }
    double ascent() const { return metrics.ascent() / Scale; }
    double descent() const { return metrics.descent() / Scale; }
};

// Everything one compile shares: fonts measured on a 72 dpi device (so point
// sizes are CSS px), the first refusal, cancellation and a work budget.
class Ctx
{
  public:
    explicit Ctx(const CompileOptions &options);
    const Face &face(double size, int weight, double spacing = 0, bool mono = false,
                     bool tabular = false);
    double textWidth(const QString &text, double size = TEXT.size, int weight = TEXT.weight,
                     bool mono = false);
    double widthOf(const QString &text, Font font)
    {
        return textWidth(text, font.size, font.weight);
    }
    // Records why nothing is drawn (the first reason wins); returns false.
    bool fail(const QString &why);
    bool cancelled() const
    {
        return options.cancel && options.cancel->load(std::memory_order_relaxed);
    }
    // Counts work; false (and a refusal) once a drawing has done too much.
    bool budget(int units = 1);

    CompileOptions options;
    QString error;
    // What the kind is laid out for (compile sets them per kind, in the
    // drawing's own sizes): its ring of tones, the width it is laid out in
    // (the text column for a figure) and the room there is past it.
    Tones tones;
    double width = 640, room = 640;
    // Painting: text halos as filled outlines, by face and text (bounded).
    QHash<QString, QPainterPath> halos;
    // Painting: file icons drawn at the device's size (bounded).
    QHash<QString, QImage> icons;

  private:
    std::map<qint64, Face> faces;
    QHash<QString, double> widths;
    QImage probe;
    qint64 spent = 0;
};

/* Shared helpers (diagram.js, in its order) */

double clamp(double v, double lo, double hi);
inline double clamp01(double v) { return clamp(v, 0, 1); }
inline bool finite(double v) { return std::isfinite(v); }
double easeOut(double t);
double easeInOut(double t);

QStringList splitWhitespace(const QString &text);
QString cutUnits(const QString &text, int n);
QString trimEnd(QString s);
QString codePoint(qint64 code);
QString replaceEach(const QString &text, const Re &re, int group);
// Upper case, spaced a little, and measured with the spacing.
double capsWidth(Ctx &c, const QString &text);
QString caps(Ctx &c, const QString &text, double max = INFINITY);
QStringList wrap(Ctx &c, const QString &text, double max, double size = TEXT.size,
                 int weight = TEXT.weight);
QStringList wrap(Ctx &c, const QString &text, double max, Font font);
QString cleanLabel(const QString &text);
QStringList splitStatements(const QString &line);
QStringList splitList(const QString &text);
QString unquote(const QString &text);
double parseFloatJs(const QString &text);
double number(const QString &text);
struct Amount {
    double value = 0;
    QString before, after;
    bool isSigned = false;
};
std::optional<Amount> amount(const QString &text);
QString bare(const QString &raw);
QStringList cellsOf(const QString &line);
int indentOf(const QString &raw);
QStringList rowCells(const QString &line);
bool sumRow(const QString &label, double value, double sum);
QString sumName(const QString &word, const QStringList &lines);
// A value with its unit: 11 600 ₽, $1 200, 38,4 s, 12%, −4.
QString withUnit(double value, const QString &before, const QString &after);
// Numbers as Intl.NumberFormat writes them (en), with a true minus: two
// decimals at most, or three significant digits for a fraction of one.
QString format(double value);
// Intl.NumberFormat with these fraction digits (no true minus), as 1.2 had.
QString formatFixed(double value, int maxFrac = 2, int minFrac = 0, bool group = true);
// A true minus for a number kept as it was written.
QString minus(const QString &text);
QString truncate(Ctx &c, const QString &text, double max, double size = TEXT.size,
                 int weight = TEXT.weight);
inline QString truncate(Ctx &c, const QString &text, double max, Font font)
{
    return truncate(c, text, max, font.size, font.weight);
}
double textMax(Ctx &c, const QStringList &lines, double size = TEXT.size, int weight = TEXT.weight);
inline double textMax(Ctx &c, const QStringList &lines, Font font)
{
    return textMax(c, lines, font.size, font.weight);
}
// Spreads labels (their y) at least `min` apart within [lo, hi].
struct Spreading {
    double y = 0;
    int index = 0;
};
void spread(QVector<Spreading> &labels, double min, double lo, double hi);
// A legend in a row (chips): each entry a mark the way its series is drawn.
struct ChipEntry {
    QString text;
    int tone = NoTone;
    QString mark = QStringLiteral("dot");
};
struct ChipRow {
    QVector<Spec> items;
    double height = 0, width = 0;
};
ChipRow chips(Ctx &c, const QVector<ChipEntry> &entries, double x, double y, double maxWidth);
// The title of a drawing, on its first line, cut to the room it has.
struct Title {
    Spec item;
    double width = 0;
};
Title titleOf(Ctx &c, const QString &text, double max = INFINITY);
double niceCeil(double value);
int decimalsFor(double step);
double niceStep(double span);

int segmentCount(const Pts &pts);
Pts resample(const Pts &pts, int count);
Pts straight(double x1, double y1, double x2, double y2);
Pts polyline(const QVector<QPointF> &points);
Pts smoothPts(const QVector<QPointF> &points);
double roughLength(const Pts &pts);
Pts shiftPts(Pts pts, double dx, double dy);

// Dates (parseDate, parseDuration) in UTC milliseconds; NaN when unread.
double parseDate(const QString &text, const QString &fmt = {});
double parseDuration(const QString &text);
double utc(double year, double month, double day, double hour = 0, double minute = 0);
// Today at UTC midnight, as `new Date()` gives it (the gantt default start).
double today();

// Spec builders.
Spec item(Type type, const QString &key, double order, int layer = DefaultLayer);
Spec labelSpec(const QString &key, double order, double x, double y, const QStringList &lines,
               const QString &cls, const QString &anchor = QStringLiteral("middle"),
               int layer = DefaultLayer);
Spec lineSpec(const QString &key, double order, double x1, double y1, double x2, double y2,
              const QString &cls, bool draw = false, int layer = DefaultLayer);

// file-kinds.js: a name's kind, tone (rgb) and glyph; and its size written.
struct FileKind {
    QString kind, label, glyph;
    QColor tone;
};
FileKind describeFile(const QString &name);
QColor fileTone(const QString &name); // FileKinds.tones[name] ("gray" if unknown).
QString formatSize(double bytes);

/* Shared between kinds */

// A ledger's rows (parseBars, ledgerOf): an xychart turned on its side is one.
struct LedgerValue {
    double value = NaN;
    QString text;
};
struct LedgerRow {
    QString label, note;
    QVector<LedgerValue> values;
};
struct LedgerData {
    QString title;
    std::optional<QString> total;
    QStringList names;
    std::function<QString(double)> amount;
    QVector<LedgerRow> rows;
};
std::optional<Result> ledgerScene(Ctx &c, const LedgerData &data);

/* Kinds */

// The lines of a diagram without what is not drawn (clean).
struct Lines {
    QStringList lines;
    QString title; // A front-matter title a scheme draws over itself.
};
Lines clean(const QString &source);
// How many kinds the table holds (43), and whether a line opens one.
int kindCount();
bool knownHeader(const QString &line);
// clean, the kind's table and the sizes it is laid out in (compile).
std::optional<Result> compileResult(Ctx &c, const QString &source);

std::optional<Result> flowKind(Ctx &c, const Lines &lines, const QString &dialect);
std::optional<Result> sequenceKind(Ctx &c, const Lines &lines);
std::optional<Result> pieKind(Ctx &c, const Lines &lines);
std::optional<Result> xyKind(Ctx &c, const Lines &lines);
std::optional<Result> candleKind(Ctx &c, const Lines &lines);
std::optional<Result> timelineKind(Ctx &c, const Lines &lines);
std::optional<Result> ganttKind(Ctx &c, const Lines &lines);
std::optional<Result> mindKind(Ctx &c, const Lines &lines);
std::optional<Result> quadrantKind(Ctx &c, const Lines &lines);
std::optional<Result> radarKind(Ctx &c, const Lines &lines);
std::optional<Result> wireframeKind(Ctx &c, const Lines &lines);
std::optional<Result> filesKind(Ctx &c, const Lines &lines);
std::optional<Result> metricsKind(Ctx &c, const Lines &lines);
std::optional<Result> barsKind(Ctx &c, const Lines &lines);
std::optional<Result> rangesKind(Ctx &c, const Lines &lines);
std::optional<Result> boardKind(Ctx &c, const Lines &lines);
std::optional<Result> stepsKind(Ctx &c, const Lines &lines);
std::optional<Result> journeyKind(Ctx &c, const Lines &lines);
std::optional<Result> waterfallKind(Ctx &c, const Lines &lines);
std::optional<Result> funnelKind(Ctx &c, const Lines &lines);
std::optional<Result> sankeyKind(Ctx &c, const Lines &lines);
std::optional<Result> heatKind(Ctx &c, const Lines &lines);
std::optional<Result> scatterKind(Ctx &c, const Lines &lines);
std::optional<Result> treemapKind(Ctx &c, const Lines &lines);
std::optional<Result> gitKind(Ctx &c, const Lines &lines);
std::optional<Result> arrayKind(Ctx &c, const Lines &lines);
std::optional<Result> bracketKind(Ctx &c, const Lines &lines);
std::optional<Result> nutritionKind(Ctx &c, const Lines &lines);
std::optional<Result> factsKind(Ctx &c, const Lines &lines);
std::optional<Result> checklistKind(Ctx &c, const Lines &lines);
std::optional<Result> changesKind(Ctx &c, const Lines &lines);
std::optional<Result> outlineKind(Ctx &c, const Lines &lines);
std::optional<Result> matchesKind(Ctx &c, const Lines &lines);
std::optional<Result> wordsKind(Ctx &c, const Lines &lines);
std::optional<Result> glossKind(Ctx &c, const Lines &lines);
std::optional<Result> formsKind(Ctx &c, const Lines &lines);
std::optional<Result> recipeKind(Ctx &c, const Lines &lines);
std::optional<Result> partsKind(Ctx &c, const Lines &lines);
std::optional<Result> settingsKind(Ctx &c, const Lines &lines);
std::optional<Result> routeKind(Ctx &c, const Lines &lines);
} // namespace diagram
