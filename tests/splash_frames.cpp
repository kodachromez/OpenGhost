// Test-only: the splash's presented frames, read back from the swapchain's
// backbuffer after each frame is rendered (what the window actually shows),
// each tagged with the splash's scene time at that frame's sync.
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QHash>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QPointer>
#include <QQmlApplicationEngine>
#include <QQuickItem>
#include <QQmlContext>
#include <QQmlExpression>
#include <QQmlProperty>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QThreadPool>
#include <QtTest>
#include <rhi/qrhi.h>
#include <QAnimationDriver>

#include "ghost.h"
#include "motion.h"

#include <algorithm>
#include <cmath>

namespace
{
QQuickItem *named(QQuickItem *item, const QString &name)
{
    if (item->objectName() == name)
        return item;
    for (auto *child : item->childItems())
        if (auto *found = named(child, name))
            return found;
    return nullptr;
}
} // namespace

int splashFrames(QQmlApplicationEngine &engine, const QString &output, int every)
{
    auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0));
    if (!window)
        return 1;
    QDir().mkpath(output);
    QPointer<QQuickItem> splash = named(window->contentItem(), QStringLiteral("splash"));
    if (!splash) {
        qCritical("No splash (reduced motion?)");
        return 1;
    }
    struct State {
        QMutex lock;
        QJsonArray frames;
        int frame = 0;
        double now = -1, ghostX = 0, ghostY = 0;
    } state;
    QThreadPool pool;
    pool.setMaxThreadCount(6);
    // GUI blocked: the values the frame about to render was synced from.
    QObject::connect(
        window, &QQuickWindow::beforeSynchronizing, window,
        [&] {
            if (!splash)
                return;
            state.now = splash->property("now").toReal();
            if (auto *ghost = named(splash, QStringLiteral("splashGhost"))) {
                state.ghostX = ghost->x();
                state.ghostY = ghost->y();
            }
        },
        Qt::DirectConnection);
    QObject::connect(
        window, &QQuickWindow::afterRendering, window,
        [&] {
            QRhi *rhi = window->rhi();
            QRhiSwapChain *chain = window->swapChain();
            if (!rhi || !chain || every <= 0)
                return;
            const int index = state.frame++;
            QJsonObject meta{{"frame", index},
                             {"now", state.now},
                             {"ghostX", state.ghostX},
                             {"ghostY", state.ghostY},
                             {"dpr", window->effectiveDevicePixelRatio()}};
            auto *result = new QRhiReadbackResult;
            const bool keep = state.now >= 0 && index % every == 0;
            result->completed = [&, rhi, result, meta, keep, index] {
                QImage image(reinterpret_cast<const uchar *>(result->data.constData()),
                             result->pixelSize.width(), result->pixelSize.height(),
                             QImage::Format_RGBA8888_Premultiplied);
                QJsonObject m = meta;
                m["width"] = result->pixelSize.width();
                m["height"] = result->pixelSize.height();
                m["format"] = int(result->format);
                if (keep) {
                    QImage copy = image.copy();
                    if (rhi->isYUpInFramebuffer())
                        copy.flip(Qt::Vertical);
                    const QString path =
                        output + QStringLiteral("/frame-%1.png").arg(index, 5, 10, QLatin1Char('0'));
                    m["file"] = path;
                    pool.start([copy, path] { copy.save(path); });
                }
                QMutexLocker locker(&state.lock);
                state.frames.append(m);
                delete result;
            };
            auto *batch = rhi->nextResourceUpdateBatch();
            batch->readBackTexture(QRhiReadbackDescription(), result);
            chain->currentFrameCommandBuffer()->resourceUpdate(batch);
        },
        Qt::DirectConnection);
    QElapsedTimer clock;
    clock.start();
    while (splash && clock.elapsed() < 8000)
        QTest::qWait(5);
    QTest::qWait(300);
    pool.waitForDone();
    QMutexLocker locker(&state.lock);
    QFile file(output + QStringLiteral("/frames.json"));
    if (!file.open(QIODevice::WriteOnly))
        return 1;
    file.write(QJsonDocument(state.frames).toJson());
    qInfo() << "Splash frames:" << state.frames.size() << "API" << window->rendererInterface()->graphicsApi()
            << "DPR" << window->effectiveDevicePixelRatio() << "size" << window->size();
    return splash ? 1 : 0;
}

// A deterministic splash: the animation driver, the frame clock and the
// Ghost's random choices are all stepped by the test, a frame every 1/240 s
// from a fixed start, as the reference capture steps OpenGhost's page; the
// window is grabbed at each requested scene time.
namespace
{
class StepDriver : public QAnimationDriver
{
  public:
    qint64 elapsed() const override { return qint64(m_ms); }
    void step(double ms)
    {
        // Paced, the frames are also that far apart in real time, for what
        // measures it (FrameAnimation.frameTime: QML springs).
        if (paced && real.isValid())
            while (real.nsecsElapsed() < (ms - m_ms) * 1e6) {
            }
        real.start();
        m_ms = ms;
        FrameClock::setTestTime(ms / 1000);
        advance();
    }
    double m_ms = 0;
    bool paced = false;
    QElapsedTimer real;
};
StepDriver *stepDriver = nullptr;
constexpr double StepStart = 1000;
// A frame every 1/240 s, or OPENGHOST_SPLASH_HZ's rate.
const double StepFrame = 1000.0 / (qEnvironmentVariableIntValue("OPENGHOST_SPLASH_HZ") > 0
                                       ? qEnvironmentVariableIntValue("OPENGHOST_SPLASH_HZ")
                                       : 240);
} // namespace

void prepareSplashSteps()
{
    FrameClock::setTestTime(StepStart / 1000);
    // Each Ghost draws from its own generator, seeded alike: the same LCG the
    // reference capture gives each ghost-thinking element.
    GhostItem::setTestRandom([](const GhostItem *ghost) {
        static QHash<const GhostItem *, quint32> seeds;
        quint32 &seed = seeds.contains(ghost) ? seeds[ghost] : (seeds[ghost] = 7u);
        seed = seed * 1664525u + 1013904223u;
        return seed / 4294967296.0;
    });
    stepDriver = new StepDriver;
    stepDriver->m_ms = StepStart;
    stepDriver->install();
}

int splashSteps(QQmlApplicationEngine &engine, const QString &output, const QString &times)
{
    auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0));
    if (!window || !stepDriver)
        return 1;
    QDir().mkpath(output);
    QPointer<QQuickItem> splash = named(window->contentItem(), QStringLiteral("splash"));
    if (!splash) {
        qCritical("No splash (reduced motion?)");
        return 1;
    }
    QList<double> wanted;
    for (const auto &part : times.split(QLatin1Char(','), Qt::SkipEmptyParts))
        wanted << part.toDouble();
    std::sort(wanted.begin(), wanted.end());
    // Let the window be exposed at its final size and scale, on the clock's start.
    if (!QTest::qWaitForWindowExposed(window))
        return 1;
    QTest::qWait(1500);
    double vt = StepStart;
    stepDriver->step(vt);
    // The scene starts once three frames came evenly (splash.js ready()).
    while (splash && splash->property("t0").toReal() < 0)
        stepDriver->step(vt += StepFrame);
    const double t0 = splash->property("t0").toReal();
    QJsonArray shots;
    for (double at : wanted) {
        const double target = t0 + at;
        while (splash && vt + StepFrame <= target + 1e-9)
            stepDriver->step(vt += StepFrame);
        if (splash && vt < target - 1e-9)
            stepDriver->step(vt = target);
        QCoreApplication::processEvents();
        const QImage image = window->grabWindow();
        const QString path = output + QStringLiteral("/native-%1.png").arg(qRound(at));
        image.save(path);
        shots.append(QJsonObject{{"t", at},
                                 {"now", splash ? splash->property("now").toReal() : -1},
                                 {"file", path},
                                 {"width", image.width()},
                                 {"height", image.height()}});
    }
    // Optionally hold the last state still, so the compositor's own output
    // of the same frame can be captured and compared.
    if (const int hold = qEnvironmentVariableIntValue("OPENGHOST_SPLASH_HOLD_MS"))
        QTest::qWait(hold);
    QFile file(output + QStringLiteral("/native.json"));
    if (!file.open(QIODevice::WriteOnly))
        return 1;
    file.write(QJsonDocument(shots).toJson());
    return 0;
}

// Qt's warnings of a colour component outside [0, 1] (QColor::setAlphaF and
// the like), counted while the check runs.
namespace
{
int invalidColours = 0;
QtMessageHandler previousHandler = nullptr;
void countInvalidColours(QtMsgType type, const QMessageLogContext &context, const QString &message)
{
    if (message.startsWith(QLatin1String("QColor::")) && message.contains(QLatin1String("invalid")))
        ++invalidColours;
    previousHandler(type, context, message);
}
} // namespace

// The splash against splash.js's own timeline, frame by frame at 240 Hz: the
// scene's start, the flight's transform as splash.js writes it (lean eased a
// fifth of the way each frame, toFixed rounding), the word starting on the
// landing's frame, the blink and the opening. With a GPU scene graph, also
// the rendered frames: the mist at .6 of the window's own pixels, no
// afterimage of the Ghost where it was, and a frame redrawn alike.
namespace
{
struct FlightPose {
    double q, x, y, vx, vy, scale, presence;
};
// splash.js spot() and pose().
FlightPose flightPose(double t, double w, double h)
{
    const auto clamp01 = [](double v) { return std::min(1.0, std::max(0.0, v)); };
    const auto spot = [&](double u) {
        const double angle = (128 + 335 * u) * M_PI / 180, r = 0.56 * w * std::pow(1 - u, 1.3);
        return QPointF(std::cos(angle) * r, std::sin(angle) * r * h / w * 1.15);
    };
    const double q = clamp01((t - 220) / 1650), u = 1 - std::pow(1 - q, 2.2);
    const QPointF here = spot(u), ahead = spot(std::min(1.0, u + 0.01));
    const double pace = 2.2 * std::pow(1 - q, 1.2) / 1650 / 0.01;
    const double a = clamp01(q / 0.16);
    return {q,
            here.x(),
            here.y(),
            (ahead.x() - here.x()) * pace,
            (ahead.y() - here.y()) * pace,
            0.42 + 0.58 * (1 - std::pow(1 - u, 3)),
            a * a * (3 - 2 * a)};
}
double toFixed(double v, int places)
{
    return QString::number(v, 'f', places).toDouble();
}
} // namespace

int splashCheck(QQmlApplicationEngine &engine)
{
    auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0));
    if (!window || !stepDriver)
        return 1;
    QPointer<QQuickItem> splash = named(window->contentItem(), QStringLiteral("splash"));
    QPointer<QQuickItem> ghost = splash ? named(splash, QStringLiteral("splashGhost")) : nullptr;
    if (!splash || !ghost) {
        qCritical("No splash (reduced motion?)");
        return 1;
    }
    int failures = 0;
    const auto check = [&failures](bool ok, const QString &what) {
        if (!ok) {
            qCritical().noquote() << "FAIL:" << what;
            ++failures;
        }
    };
    const auto value = [&](const char *name) { return splash->property(name).toReal(); };
    const bool gpu = window->rendererInterface()->graphicsApi() != QSGRendererInterface::Software;
    // The clock stands still meanwhile: let the window take its final size
    // and scale (a compositor's maximize, fractional scale) first.
    if (!QTest::qWaitForWindowExposed(window))
        return 1;
    QTest::qWait(1500);
    double vt = StepStart;
    int frames = 0;
    const auto step = [&] {
        stepDriver->step(vt += StepFrame);
        ++frames;
        // Animations started this frame register through queued calls.
        QCoreApplication::processEvents();
    };
    stepDriver->step(vt);
    ++frames;
    while (splash && value("t0") < 0 && frames < 100)
        step();
    check(frames == 3, QStringLiteral("scene starts on the third even frame (was %1)").arg(frames));
    qInfo() << "Splash check: renderer" << window->rendererInterface()->graphicsApi() << "DPR"
            << window->effectiveDevicePixelRatio() << "size" << window->size();
    const double w = splash->width(), h = splash->height(), dpr = window->effectiveDevicePixelRatio();

    if (gpu) {
        auto *mist = splash->findChild<QQuickItem *>(QStringLiteral("splashMist"));
        const QSize texture = mist ? QQmlProperty::read(mist, QStringLiteral("layer.textureSize")).toSize()
                                   : QSize();
        const QSize expected(qRound(w * dpr * 0.6), qRound(h * dpr * 0.6));
        check(texture == expected, QStringLiteral("mist layer at .6 of the window's pixels: %1x%2, "
                                                  "expected %3x%4")
                                       .arg(texture.width()).arg(texture.height())
                                       .arg(expected.width()).arg(expected.height()));
    }

    previousHandler = qInstallMessageHandler(countInvalidColours);
    stepDriver->paced = true;
    // The splash's own alpha sources, before any item clamps them: each must
    // stay in [0, 1] on every frame (an item's opacity is clamped silently).
    const QString sources = QStringLiteral(
        "[presence, faded, flown, shifted, leaving, reveal, open, flight]"
        ".concat(Array.from({length: word.length}, (_, k) => letterAt(k)))"
        ".concat(Array.from({length: word.length}, (_, k) => sharpAt(k)))"
        ".concat(Array.from({length: word.length}, (_, k) => hazeAt(k)))");
    double lowest = 0, highest = 1;
    QSignalSpy finished(splash.data(), SIGNAL(finished()));
    double tilt = 0, landedFrame = -1, blinkedFrame = -1, openFrame = -1;
    int flown = 0;
    QImage early, late, again;
    QRectF earlyBox, lateBox;
    const auto ghostBox = [&] {
        const QRectF r = ghost->mapRectToScene(QRectF(0, 0, ghost->width(), ghost->height()));
        return QRectF(r.x() * dpr, r.y() * dpr, r.width() * dpr, r.height() * dpr);
    };
    while (splash && vt - StepStart < 8000) {
        const bool wasBlinked = splash->property("blinked").toBool();
        const bool wasOpen = value("openAt") >= 0;
        step();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        if (!splash || finished.count())
            break;
        const double now = value("now");
        const double landedAt = value("landedAt");
        {
            QQmlExpression read(qmlContext(splash), splash, sources);
            const QVariantList list = read.evaluate().toList();
            for (int i = 0; i < list.size(); ++i) {
                const double v = list[i].toDouble();
                if (v < lowest || v > highest)
                    qInfo().nospace() << "alpha source " << i << " = " << qSetRealNumberPrecision(17) << v << " at " << now;
                lowest = std::min(lowest, v);
                highest = std::max(highest, v);
            }
            check(!read.hasError(), QStringLiteral("alpha sources readable: %1").arg(read.error().toString()));
        }
        if (landedAt < 0) {
            const FlightPose p = flightPose(now, w, h);
            if (p.q > 0 && p.q < 1) {
                tilt += (std::max(-1.0, std::min(1.0, p.vx / 1.4)) * 16 - tilt) * 0.2;
                ++flown;
                check(std::abs(value("tilt") - tilt) < 1e-9,
                      QStringLiteral("lean eased a fifth per frame at %1 ms: %2, expected %3")
                          .arg(now).arg(value("tilt")).arg(tilt));
                check(value("flyX") == toFixed(p.x, 2) && value("flyY") == toFixed(p.y, 2) &&
                          value("flyScale") == toFixed(p.scale, 4) &&
                          value("presence") == toFixed(p.presence, 3),
                      QStringLiteral("flight transform as splash.js writes it at %1 ms").arg(now));
                check(std::abs(ghost->rotation() - toFixed(tilt, 2)) < 1e-9,
                      QStringLiteral("rotation to 2 places at %1 ms").arg(now));
            }
        } else if (landedFrame < 0) {
            landedFrame = now;
            check(landedAt == now && flightPose(now, w, h).q >= 1 &&
                      flightPose(now - StepFrame, w, h).q < 1,
                  QStringLiteral("lands on the first frame the flight is done (%1 ms)").arg(now));
            check(value("wordAt") == landedAt,
                  QStringLiteral("word and step aside start on the landing's frame: %1, landed %2")
                      .arg(value("wordAt")).arg(landedAt));
        }
        if (!wasBlinked && splash->property("blinked").toBool())
            blinkedFrame = now;
        if (!wasOpen && value("openAt") >= 0)
            openFrame = now;
        // Grab the Ghost in flight (out of the mist by 600 ms), then again
        // on the first frame it has moved clear of that place.
        if (gpu && early.isNull() && now >= 600) {
            early = window->grabWindow();
            earlyBox = ghostBox();
        } else if (gpu && !early.isNull() && late.isNull() && landedAt < 0 &&
                   !ghostBox().adjusted(-3, -3, 3, 3).intersects(earlyBox)) {
            late = window->grabWindow();
            again = window->grabWindow();
            lateBox = ghostBox();
        }
    }
    check(flown > 1500 / StepFrame, QStringLiteral("flight stepped frame by frame (%1 frames)").arg(flown));
    check(landedFrame >= 0 && blinkedFrame >= landedFrame + 200 &&
              blinkedFrame < landedFrame + 200 + StepFrame,
          QStringLiteral("blinks on the first frame 200 ms after landing (%1, landed %2)")
              .arg(blinkedFrame).arg(landedFrame));
    const double wordDone = 40 + 9 * 30 + 560 + 200;
    check(openFrame >= landedFrame + wordDone && openFrame < landedFrame + wordDone + StepFrame,
          QStringLiteral("opens on the first frame the word is done (%1)").arg(openFrame));
    check(finished.count() == 1, "splash finishes and hands off");
    check(lowest >= 0 && highest <= 1,
          QStringLiteral("every splash alpha source in [0, 1] on every frame (%1 … %2)")
              .arg(lowest).arg(highest));
    check(invalidColours == 0,
          QStringLiteral("no invalid colour (alpha) over the whole splash (%1)").arg(invalidColours));

    // The browser button's globe fill settles closed on a spring that passes
    // just below 0 at high frame rates (browser-toggle.js's open spring,
    // k 170, c 24): open the panel, close it, and watch every frame.
    auto *root = engine.rootObjects().value(0);
    auto *globe = named(window->contentItem(), QStringLiteral("globeGlyph"));
    if (root && globe && root->property("browser").value<QObject *>()) {
        const auto toggle = [&] {
            QQmlExpression e(qmlContext(root), root, QStringLiteral("window.browser.toggle()"));
            e.evaluate();
        };
        const int before = invalidColours;
        double dipped = 0, peak = 0;
        toggle();
        for (int i = 0; i * StepFrame < 1500; ++i) {
            step();
            peak = std::max(peak, globe->property("open").toReal());
        }
        toggle();
        for (int i = 0; i * StepFrame < 1500; ++i) {
            step();
            dipped = std::min(dipped, globe->property("open").toReal());
        }
        qInfo() << "Globe: highest open" << peak << "lowest open" << dipped;
        check(invalidColours == before,
              QStringLiteral("no invalid colour as the globe settles closed (%1)")
                  .arg(invalidColours - before));
    } else {
        check(false, "browser button's globe found");
    }

    if (gpu) {
        // The Ghost's earlier place, apart from where it is now: only the
        // mist (motes are a few pixels each), no afterimage of the Ghost.
        const auto white = [](const QImage &image, const QRect &area, const QRect &skip) {
            int count = 0, total = 0;
            for (int y = std::max(0, area.top()); y <= std::min(image.height() - 1, area.bottom()); ++y)
                for (int x = std::max(0, area.left()); x <= std::min(image.width() - 1, area.right()); ++x) {
                    if (skip.contains(x, y))
                        continue;
                    ++total;
                    const QRgb c = image.pixel(x, y);
                    count += std::min({qRed(c), qGreen(c), qBlue(c)}) > 235;
                }
            return std::pair(count, total);
        };
        const QRect was = earlyBox.toAlignedRect(), is = lateBox.adjusted(-3, -3, 3, 3).toAlignedRect();
        check(!late.isNull() && !was.intersects(is), "the Ghost moved clear of its earlier place");
        const auto [then, thenTotal] = white(early, was, QRect());
        const auto [left, leftTotal] = white(late, was, is);
        check(then > thenTotal / 4, QStringLiteral("the Ghost is drawn where it is (%1 of %2 px)")
                                        .arg(then).arg(thenTotal));
        check(left < 25, QStringLiteral("no afterimage where the Ghost was (%1 white px of %2)")
                             .arg(left).arg(leftTotal));
        check(late == again, "the same frame redrawn is identical (no stale or accumulated pixels)");
    }
    qInstallMessageHandler(previousHandler);
    qInfo() << "Splash check:" << (failures ? "FAILED" : "passed") << "frames" << frames
            << "at" << 1000 / StepFrame << "Hz";
    return failures ? 1 : 0;
}
