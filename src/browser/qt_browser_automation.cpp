#include "qt_browser_automation.h"
#include <QBuffer>
#include <QFile>
#include <QFocusEvent>
#include <QGuiApplication>
#include <QFileInfo>
#include <QImage>
#include <QInputMethodEvent>
#include <QJsonArray>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QQuickItemGrabResult>
#include <QQuickWindow>
#include <QStyleHints>
#include <QTimer>
#include <QWheelEvent>
#include <algorithm>
#include <QWebEngineDownloadRequest>
#include <cmath>

static void observationResources() { Q_INIT_RESOURCE(browser_observe); }
namespace openghost
{
namespace
{
QJsonObject error(const QString &text, const QString &code = "browser_error")
{
    return {{"error", text}, {"code", code}};
}
QString json(const QJsonValue &value)
{
    const auto bytes = QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact);
    return QString::fromUtf8(bytes.mid(1, bytes.size() - 2));
}
// A direct-call argument as the reference interpolates it: absent stays undefined.
QString literal(const QJsonObject &args, const char *name)
{
    return args.contains(name) ? json(args[name]) : QStringLiteral("undefined");
}
QString observations()
{
    observationResources();
    QFile file(":/browser/observe.js");
    if (!file.open(QIODevice::ReadOnly))
        qFatal("Missing browser observations");
    return QString::fromUtf8(file.readAll());
}
} // namespace
struct QtBrowserAutomation::Capture {
    quint64 call = 0;
    Target target;
    QPointer<QQuickItem> view;
    int width = 0, viewport = 0, height = 0;
    QSize output;
    double zoom = 1, savedX = 0, savedY = 0;
    bool full = false, truncated = false, staged = false, frozen = false, restoring = false;
    bool moved = false; // a scroll to the origin was sent
    bool cancelled = false;
    int step = 0, polls = 0;
    QJsonObject result;
    QMetaObject::Connection lost;
};
namespace
{
constexpr int CaptureEdgeMax = 16384;         // device-independent pixels per edge
constexpr qint64 CapturePixelsMax = 64 << 20; // CSS pixels in a stretched guest
constexpr int CapturePolls = 100, CapturePollMs = 30, CaptureAnswerMs = 2000;
} // namespace
QtBrowserAutomation::~QtBrowserAutomation()
{
    // Never leave a guest stretched or covered by a capture this owner started.
    const auto calls = m_captures.keys();
    for (const auto call : calls) {
        m_captures[call]->cancelled = true;
        finishCapture(call);
    }
}
void QtBrowserAutomation::cancel(quint64 call)
{
    m_pending.remove(call);
    if (const auto shot = m_captures.value(call)) {
        // Dropping completion is not enough: undo this capture's own view state.
        shot->cancelled = true;
        restoreCapture(shot);
    }
}
void QtBrowserAutomation::attach(const QString &tab, int incarnation, QObject *view)
{
    m_guests[tab] = {incarnation, qobject_cast<QQuickItem *>(view)};
    connect(view, &QObject::destroyed, this, [this, tab, incarnation] {
        if (m_guests.value(tab).incarnation == incarnation)
            m_guests.remove(tab);
    });
}
QQuickItem *QtBrowserAutomation::item(const Target &target) const
{
    const auto guest = m_guests.value(target.tab);
    return guest.incarnation == target.incarnation ? guest.item.data() : nullptr;
}
void QtBrowserAutomation::complete(quint64 call, QJsonObject result)
{
    const auto found = m_pending.find(call);
    if (found == m_pending.end())
        return;
    auto pending = std::move(found.value());
    m_pending.erase(found);
    if (!item(pending.target))
        result = error("This browser guest is gone", "tab_gone");
    pending.done(std::move(result));
}
void QtBrowserAutomation::scriptResult(const QString &tab, int incarnation, const QString &callText,
                                       const QVariant &result)
{
    if (callText.startsWith(QLatin1String("capture:"))) {
        const auto step = m_steps.take(callText);
        const auto owner = callText.section(':', 1, 1).toULongLong();
        const auto shot = m_captures.value(owner);
        if (!step || !shot || shot->target.tab != tab || shot->target.incarnation != incarnation)
            return;
        const auto doc = QJsonDocument::fromJson(result.toString().toUtf8());
        step(doc.isObject() ? doc.object() : error("The page did not answer"));
        return;
    }
    const auto call = callText.toULongLong();
    const auto found = m_pending.constFind(call);
    if (found == m_pending.cend() || found->target.tab != tab ||
        found->target.incarnation != incarnation)
        return;
    const auto doc = QJsonDocument::fromJson(result.toString().toUtf8());
    complete(call, doc.isObject() ? doc.object() : error("The page did not answer"));
}
void QtBrowserAutomation::query(quint64 call, const Target &target, Query kind,
                                const QJsonObject &args, qint64 expires, Done done)
{
    m_pending.insert(call, {target, std::move(done)});
    emulateFocus(item(target));
    if (!item(target)) {
        QTimer::singleShot(0, this, [this, call] {
            complete(call, error("This browser guest is gone", "tab_gone"));
        });
        return;
    }
    static const QString library = observations();
    QString expression;
    switch (kind) {
    case Query::Snapshot:
        expression = "__og.snapshot(" + json(args) + ")";
        break;
    case Query::Has:
        expression = "({found:__og.has(" + json(args["text"]) + ")})";
        break;
    case Query::Quiet:
        expression = "({quiet:__og.quiet()})";
        break;
    case Query::Reveal:
        expression = "({revealed:__og.reveal(" + json(args["ref"]) + ")})";
        break;
    // Number()/String() conversions run in the page as the reference's do.
    case Query::Point:
        expression = "__og.point(Number(" + literal(args, "ref") + "))";
        break;
    case Query::Choose:
        expression = "({chosen:__og.choose(Number(" + literal(args, "ref") + "),String(" +
                     literal(args, "option") + "?\?''))})";
        break;
    case Query::Probe:
        expression = "({probed:true})";
        break;
    case Query::Metrics:
        // view*: the visual viewport (cssVisualViewport client size, no scrollbars).
        expression = "({width:innerWidth,height:innerHeight,contentHeight:(document."
                     "scrollingElement||document.documentElement).scrollHeight,top:scrollY,"
                     "left:scrollX,"
                     "viewWidth:visualViewport?visualViewport.width:innerWidth,"
                     "viewHeight:visualViewport?visualViewport.height:innerHeight})";
        break;
    case Query::Read:
        // Capture/cap first; parse the frozen string in an inert DOM, not live
        // innerText. The exact reference converter stays in this isolated world.
        expression = "(()=>{const source=document.documentElement?.outerHTML||'';const "
                     "html=source.slice(0,4*1024*1024);"
                     "return "
                     "{text:readable(html,location.href),url:location.href,sourceTruncated:source."
                     "length>html.length};})()";
        break;
    }
    const bool input = kind == Query::Reveal || kind == Query::Point || kind == Query::Choose ||
                       kind == Query::Probe;
    // Input never installs a ref map: a delayed run in a replacement document fails.
    const QString prepare = input ? "if(window.__og?.pageId!==" + json(target.page) +
                                        "||window.__og.navigating())throw Object.assign(new "
                                        "Error('The page changed. Take a "
                                        "new snapshot.'),{code:'stale_page'});"
                                  : "install(" + json(target.page) + ");";
    const QString source =
        "(()=>{try{if(Date.now()>" + QString::number(expires) +
        ")throw Object.assign(new Error('Browser operation expired'),{code:'timeout'});\n" +
        "if(window.__ogDocument?.key!==" + json(target.document) +
        ")throw Object.assign(new Error('The document changed. Take a new "
        "snapshot.'),{code:'stale_page'});\n" +
        library + "\n" + prepare + "return JSON.stringify(" + expression +
        ");"
        "}catch(e){return "
        "JSON.stringify({error:String(e?.message||e),code:e?.code||'browser_error'});}})()";
    emit script(target.tab, target.incarnation, QString::number(call), source);
}
void QtBrowserAutomation::navigate(const Target &target, const QString &verb, const QString &url)
{
    if (item(target))
        emit navigation(target.tab, target.incarnation, verb, url);
}
// desktop/browser.js screenshot(): CSS viewport width, and for a full page the
// content height capped at four viewports, captured from the page's top-left
// (CDP clip x=0,y=0 with captureBeyondViewport). Qt has no beyond-viewport
// grab, so a tall or scrolled capture briefly gives the guest the capture
// height: the visible panel shows a frozen copy of its pixels meanwhile, the
// page is scrolled to its origin, and size and scroll position are restored
// before the result is delivered. Pages can observe that as resize/scroll events.
void QtBrowserAutomation::capture(quint64 call, const Target &target, const QJsonObject &metrics,
                                  bool full, Done done)
{
    m_pending.insert(call, {target, std::move(done)});
    emulateFocus(item(target));
    auto *view = item(target);
    const int width = qRound(metrics["width"].toDouble()),
              viewport = qRound(metrics["height"].toDouble());
    const double content = metrics["contentHeight"].toDouble();
    const int height = full ? qRound(std::min(content, viewport * 4.0)) : viewport;
    const auto fail = [this, call](const QString &text, const QString &code = "browser_error") {
        QTimer::singleShot(0, this,
                           [this, call, text, code] { complete(call, error(text, code)); });
    };
    if (!view || !view->window() || width <= 0 || viewport <= 0 || height <= 0 ||
        width > CaptureEdgeMax || viewport > CaptureEdgeMax) {
        fail("The page has no capturable viewport");
        return;
    }
    auto shot = std::make_shared<Capture>();
    shot->call = call;
    shot->target = target;
    shot->view = view;
    shot->width = width;
    shot->viewport = viewport;
    shot->height = height;
    shot->full = full;
    shot->truncated = full && content > height;
    shot->zoom = std::max(0.01, view->property("zoomFactor").toDouble());
    const int outputWidth = std::min(width, 1280);
    shot->output = QSize(outputWidth, std::max(1, qRound(double(height) * outputWidth / width)));
    const bool origin = metrics["top"].toDouble() == 0 && metrics["left"].toDouble() == 0;
    if (!full || (height <= viewport && origin)) {
        // The painted viewport already is the requested region.
        m_captures.insert(call, shot);
        grab(shot);
        return;
    }
    if (qint64(width) * height > CapturePixelsMax || height * shot->zoom > CaptureEdgeMax) {
        fail(QStringLiteral("A full-page screenshot of %1×%2 is too large to capture.")
                 .arg(width)
                 .arg(height),
             "unavailable");
        return;
    }
    m_captures.insert(call, shot);
    shot->lost = connect(view, &QObject::destroyed, this, [this, call] { finishCapture(call); });
    QVariant frozen;
    QMetaObject::invokeMethod(view, "freezeCapture", Q_RETURN_ARG(QVariant, frozen));
    if (!frozen.toBool()) {
        shot->result = error("The page could not be held still for a full-page screenshot");
        finishCapture(call);
        return;
    }
    shot->frozen = true;
    shot->savedX = metrics["left"].toDouble();
    shot->savedY = metrics["top"].toDouble();
    // The cover must have drawn its copy before the guest changes.
    frames(shot, 2, 500, [this, shot] {
        // Restored even if its answer is lost to a cancellation.
        shot->moved = shot->savedX || shot->savedY;
        captureScript(shot,
                      "if(scrollX||scrollY)scrollTo({left:0,top:0,behavior:'instant'});return {};",
                      [this, shot](const QJsonObject &) {
                          shot->staged = true;
                          QMetaObject::invokeMethod(shot->view, "stretchCapture",
                                                    Q_ARG(QVariant, shot->height * shot->zoom));
                          shot->polls = 0;
                          stretched(shot);
                      });
    });
}
void QtBrowserAutomation::stretched(const std::shared_ptr<Capture> &shot)
{
    // Wait for the renderer to lay out the capture size at the origin, then
    // for two of its animation frames and two of the window's, so the grab
    // is of a frame drawn at that size rather than a scaled earlier one.
    captureScript(shot,
                  "if(scrollX||scrollY)scrollTo({left:0,top:0,behavior:'instant'});"
                  "const sized=Math.abs(innerHeight-" +
                      QString::number(shot->height) + ")<=1&&Math.abs(innerWidth-" +
                      QString::number(shot->width) +
                      ")<=1&&!scrollX&&!scrollY;"
                      "if(!sized){delete window.__ogShot;return {sized:false};}"
                      "if(window.__ogShot===undefined){window.__ogShot=0;"
                      "requestAnimationFrame(()=>requestAnimationFrame(()=>{window.__ogShot=1;}));}"
                      "return {sized:true,drawn:window.__ogShot===1};",
                  [this, shot](const QJsonObject &state) {
                      if (state["sized"].toBool() && state["drawn"].toBool()) {
                          frames(shot, 2, 500, [this, shot] { grab(shot); });
                          return;
                      }
                      if (++shot->polls >= CapturePolls) {
                          shot->result = error("The page did not draw at the full-page size");
                          restoreCapture(shot);
                          return;
                      }
                      QTimer::singleShot(CapturePollMs, this, [this, shot] {
                          if (m_captures.value(shot->call) == shot && !shot->restoring)
                              stretched(shot);
                      });
                  });
}
void QtBrowserAutomation::grab(const std::shared_ptr<Capture> &shot)
{
    auto *view = shot->view.data();
    const auto result =
        view ? view->grabToImage(
                   shot->staged ? shot->output
                                : QSize(shot->output.width(),
                                        std::max(1, qRound(double(shot->viewport) *
                                                           shot->output.width() / shot->width))))
             : QSharedPointer<QQuickItemGrabResult>();
    if (!result) {
        shot->result = error("The page did not draw a screenshot");
        restoreCapture(shot);
        return;
    }
    // The render job remains owned by Qt; the completion is call-token guarded.
    connect(
        result.data(), &QQuickItemGrabResult::ready, this,
        [this, result, shot] {
            if (m_captures.value(shot->call) != shot || shot->restoring)
                return;
            QImage image = result->image();
            if (image.isNull()) {
                shot->result = error("The page did not draw a screenshot");
                restoreCapture(shot);
                return;
            }
            if (!shot->staged) {
                const QSize painted(shot->output.width(),
                                    std::max(1, qRound(double(shot->viewport) *
                                                       shot->output.width() / shot->width)));
                if (image.size() != painted)
                    image = image.scaled(painted, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
                // A page shorter than its viewport: the capture ends at its content.
                image = image.copy(0, 0, image.width(),
                                   std::min(image.height(), shot->output.height()));
            } else if (image.size() != shot->output)
                image = image.scaled(shot->output, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
            QByteArray bytes;
            QBuffer buffer(&bytes);
            buffer.open(QIODevice::WriteOnly);
            if (!image.save(&buffer, "JPEG", 82)) {
                shot->result = error("Could not encode the screenshot");
                restoreCapture(shot);
                return;
            }
            shot->result = {
                {"image", "data:image/jpeg;base64," + QString::fromLatin1(bytes.toBase64())},
                {"width", image.width()},
                {"height", image.height()},
                {"scale", double(image.width()) / shot->width},
                {"pageWidth", shot->width},
                {"pageHeight", shot->height},
                {"truncated", shot->truncated}};
            restoreCapture(shot);
        },
        Qt::SingleShotConnection);
}
void QtBrowserAutomation::restoreCapture(const std::shared_ptr<Capture> &shot)
{
    if (shot->restoring)
        return;
    shot->restoring = true;
    ++shot->step; // late answers of the capture steps are void
    if (shot->staged)
        QMetaObject::invokeMethod(shot->view, "unstretchCapture");
    if (!shot->staged && !shot->moved) {
        thaw(shot);
        return;
    }
    shot->polls = 0;
    restored(shot);
}
void QtBrowserAutomation::restored(const std::shared_ptr<Capture> &shot)
{
    // Restore the scroll position once the page has its own viewport again,
    // and only in the same document: a replacement page keeps its own.
    captureScript(
        shot,
        "if(Math.abs(innerHeight-" + QString::number(shot->viewport) +
            ")>1)return {sized:false};"
            "scrollTo({left:" +
            QString::number(shot->savedX) + ",top:" + QString::number(shot->savedY) +
            ",behavior:'instant'});delete window.__ogShot;return {sized:true};",
        [this, shot](const QJsonObject &state) {
            if (state["sized"].toBool() || ++shot->polls >= CapturePolls) {
                thaw(shot);
                return;
            }
            QTimer::singleShot(CapturePollMs, this, [this, shot] {
                if (m_captures.value(shot->call) == shot)
                    restored(shot);
            });
        },
        [this, shot] { thaw(shot); });
}
void QtBrowserAutomation::thaw(const std::shared_ptr<Capture> &shot)
{
    if (!shot->frozen) {
        finishCapture(shot->call);
        return;
    }
    // Uncover only after the guest has drawn its restored frame.
    frames(shot, 2, 500, [this, shot] { finishCapture(shot->call); }, true);
}
void QtBrowserAutomation::finishCapture(quint64 call)
{
    const auto shot = m_captures.take(call);
    if (!shot)
        return;
    ++shot->step;
    disconnect(shot->lost);
    const auto prefix = QStringLiteral("capture:%1:").arg(call);
    for (auto it = m_steps.begin(); it != m_steps.end();)
        it = it.key().startsWith(prefix) ? m_steps.erase(it) : std::next(it);
    if (shot->view) {
        if (shot->staged && !shot->restoring)
            QMetaObject::invokeMethod(shot->view, "unstretchCapture");
        if (shot->frozen)
            QMetaObject::invokeMethod(shot->view, "thawCapture");
    }
    shot->frozen = shot->staged = false;
    if (!shot->cancelled)
        complete(call, shot->result.isEmpty() ? error("The page did not draw a screenshot")
                                              : shot->result);
}
void QtBrowserAutomation::frames(const std::shared_ptr<Capture> &shot, int count, int ms,
                                 std::function<void()> next, bool restoring)
{
    auto *window = shot->view ? shot->view->window() : nullptr;
    if (!window) {
        finishCapture(shot->call);
        return;
    }
    const int step = shot->step;
    auto left = std::make_shared<int>(count);
    auto fired = std::make_shared<bool>(false);
    auto go = [this, shot, step, fired, next, restoring] {
        if (*fired || m_captures.value(shot->call) != shot || shot->step != step ||
            (shot->restoring && !restoring))
            return;
        *fired = true;
        next();
    };
    auto *context = new QObject(this);
    connect(window, &QQuickWindow::frameSwapped, context, [window, left, go, context] {
        if (--*left > 0) {
            window->update();
            return;
        }
        context->deleteLater();
        go();
    });
    // A window that is not being drawn (minimized/unexposed) still settles.
    QTimer::singleShot(ms, context, [go, context] {
        context->deleteLater();
        go();
    });
    window->update();
}
void QtBrowserAutomation::captureScript(const std::shared_ptr<Capture> &shot, const QString &body,
                                        std::function<void(const QJsonObject &)> next,
                                        std::function<void()> stale)
{
    if (!shot->view) {
        finishCapture(shot->call);
        return;
    }
    const auto token = QStringLiteral("capture:%1:%2").arg(shot->call).arg(++shot->step);
    m_steps.insert(token, [this, shot, step = shot->step, next = std::move(next),
                           stale = std::move(stale)](const QJsonObject &answer) {
        if (m_captures.value(shot->call) != shot || shot->step != step)
            return;
        if (answer["code"].toString() == "stale_page") {
            // The page was replaced: leave the new document's scroll alone.
            if (stale) {
                stale();
                return;
            }
            shot->result = answer;
            restoreCapture(shot);
            return;
        }
        if (!answer["error"].toString().isEmpty()) {
            shot->result = answer;
            if (shot->restoring)
                thaw(shot);
            else
                restoreCapture(shot);
            return;
        }
        next(answer);
    });
    // A script sent while the document is being replaced may never answer.
    QTimer::singleShot(CaptureAnswerMs, this, [this, shot, token] {
        const auto step = m_steps.take(token);
        if (!step)
            return;
        step(error("The page is not responding"));
    });
    emit script(shot->target.tab, shot->target.incarnation, token,
                "(()=>{try{if(window.__ogDocument?.key!==" + json(shot->target.document) +
                    ")return JSON.stringify({error:'The document changed. Take a new "
                    "snapshot.',code:'stale_page'});\nreturn JSON.stringify((()=>{" +
                    body +
                    "})());}catch(e){return "
                    "JSON.stringify({error:String(e?.message||e),code:'browser_error'});}})()");
}
void QtBrowserAutomation::download(QObject *object)
{
    auto *request = qobject_cast<QWebEngineDownloadRequest *>(object);
    if (!request || request->isFinished())
        return;
    // The guest that asked, if it is a panel guest (not a popup window).
    const auto *view = request->property("view").value<QObject *>();
    QString tab;
    int incarnation = 0;
    for (auto it = m_guests.cbegin(); view && it != m_guests.cend(); ++it)
        if (it->item == view) {
            tab = it.key();
            incarnation = it->incarnation;
        }
    const auto id = ++m_downloads;
    const QString file = m_downloadStarting
                             ? m_downloadStarting(id, tab, incarnation, request->downloadFileName())
                             : QString();
    if (file.isEmpty()) {
        request->cancel();
        return;
    }
    // Reported once, whether it completes, is cancelled, fails or is dropped.
    auto ended = std::make_shared<bool>(false);
    const auto end = [this, id, ended](bool completed) {
        if (std::exchange(*ended, true))
            return;
        if (m_downloadEnded)
            m_downloadEnded(id, completed);
    };
    connect(request, &QWebEngineDownloadRequest::isFinishedChanged, this, [request, end] {
        if (request->isFinished())
            end(request->state() == QWebEngineDownloadRequest::DownloadCompleted);
    });
    connect(request, &QObject::destroyed, this, [end] { end(false); });
    const QFileInfo target(file);
    request->setDownloadDirectory(target.absolutePath());
    request->setDownloadFileName(target.fileName());
    request->accept();
}
namespace
{
// desktop/browser.js KEYS/keyOf() codes as Qt keys plus XKB (evdev + 8) scan
// codes, from which WebEngine derives the DOM `code`. Other single characters
// carry no Qt key or scan code, so `code` stays empty as in the reference.
struct Key {
    int qt;
    quint32 scan;
};
Key keyFor(const QString &code)
{
    static const QHash<QString, Key> named = {
        {"Enter", {Qt::Key_Return, 36}},    {"Tab", {Qt::Key_Tab, 23}},
        {"Escape", {Qt::Key_Escape, 9}},    {"Backspace", {Qt::Key_Backspace, 22}},
        {"Delete", {Qt::Key_Delete, 119}},  {"Space", {Qt::Key_Space, 65}},
        {"ArrowUp", {Qt::Key_Up, 111}},     {"ArrowDown", {Qt::Key_Down, 116}},
        {"ArrowLeft", {Qt::Key_Left, 113}}, {"ArrowRight", {Qt::Key_Right, 114}},
        {"PageUp", {Qt::Key_PageUp, 112}},  {"PageDown", {Qt::Key_PageDown, 117}},
        {"Home", {Qt::Key_Home, 110}},      {"End", {Qt::Key_End, 115}}};
    if (const auto found = named.find(code); found != named.end())
        return *found;
    // US-layout XKB key codes for A–Z and 0–9.
    static const quint32 letters[26] = {38, 56, 54, 40, 26, 41, 42, 43, 31, 44, 45, 46, 58,
                                        57, 32, 33, 24, 27, 39, 28, 30, 55, 25, 53, 29, 52};
    if (code.size() == 4 && code.startsWith("Key") && code[3] >= 'A' && code[3] <= 'Z')
        return {Qt::Key_A + (code[3].unicode() - 'A'), letters[code[3].unicode() - 'A']};
    if (code.size() == 6 && code.startsWith("Digit") && code[5].isDigit()) {
        const int digit = code[5].digitValue();
        return {Qt::Key_0 + digit, quint32(digit ? 9 + digit : 19)};
    }
    return {0, 0};
}
Qt::KeyboardModifiers modifiersOf(int bits)
{
    Qt::KeyboardModifiers out;
    if (bits & 1)
        out |= Qt::AltModifier;
    if (bits & 2)
        out |= Qt::ControlModifier;
    if (bits & 4)
        out |= Qt::MetaModifier;
    if (bits & 8)
        out |= Qt::ShiftModifier;
    return out;
}
} // namespace
bool QtBrowserAutomation::eventFilter(QObject *watched, QEvent *event)
{
    // Emulation.setFocusEmulationEnabled: the page keeps acting focused while
    // the app's keyboard focus moves (blur(), giveBack, the chat). Only the
    // renderer's notification is withheld; Qt focus itself still moves.
    if (event->type() == QEvent::FocusOut && m_emulated.contains(watched))
        return true;
    return BrowserAutomation::eventFilter(watched, event);
}
QQuickItem *QtBrowserAutomation::emulateFocus(QQuickItem *view)
{
    // desktop/browser.js attach(): from a guest's first automated step on,
    // focus emulation stays on for that page, as the reference's debugger does.
    auto *to = receiver(view);
    if (!to || m_emulated.contains(to))
        return to;
    m_emulated.insert(to);
    connect(to, &QObject::destroyed, this, [this, to] { m_emulated.remove(to); });
    to->installEventFilter(this);
    if (!to->hasActiveFocus()) {
        QFocusEvent in(QEvent::FocusIn, Qt::OtherFocusReason);
        QCoreApplication::sendEvent(to, &in);
    }
    return to;
}
QQuickItem *QtBrowserAutomation::receiver(QQuickItem *view)
{
    // The view's own input item. Events sent to it reach the renderer as
    // trusted user input without window hit-testing, so the panel's driving
    // shield cannot intercept them (CDP input likewise bypasses the embedder).
    if (!view)
        return nullptr;
    for (auto *child : view->childItems())
        if (child->inherits("QtWebEngineCore::RenderWidgetHostViewQtDelegateItem"))
            return child;
    return nullptr;
}
void QtBrowserAutomation::input(quint64 call, const Target &target, const Input &event, Done done)
{
    m_pending.insert(call, {target, std::move(done)});
    auto *view = item(target);
    auto *to = emulateFocus(view);
    const auto answer = [this, call](QJsonObject result) {
        QTimer::singleShot(0, this, [this, call, result] { complete(call, result); });
    };
    if (!to) {
        answer(error("The page has no input target", view ? "unavailable" : "tab_gone"));
        return;
    }
    // Guest viewport CSS pixels to item pixels.
    const double zoom =
        view->property("zoomFactor").toDouble() > 0 ? view->property("zoomFactor").toDouble() : 1.0;
    const QPointF at = to->mapFromItem(view, QPointF(event.x, event.y) * zoom);
    using Kind = Input::Kind;
    switch (event.kind) {
    case Kind::Move:
    case Kind::Press:
    case Kind::Release: {
        // WebEngine counts clicks from event timestamps. A private clock far
        // from platform time keeps each sequence's explicit count: a new
        // sequence never joins a user's or an earlier step's click.
        if (event.kind == Kind::Move)
            m_clock += 1000000;
        const quint64 stamp = m_clock + quint64(std::max(0, event.count - 1));
        const auto type = event.kind == Kind::Move    ? QEvent::MouseMove
                          : event.kind == Kind::Press ? QEvent::MouseButtonPress
                                                      : QEvent::MouseButtonRelease;
        QMouseEvent mouse(type, at, to->mapToScene(at), to->mapToGlobal(at),
                          event.kind == Kind::Move ? Qt::NoButton : Qt::LeftButton,
                          event.kind == Kind::Press ? Qt::LeftButton : Qt::NoButton,
                          Qt::NoModifier);
        mouse.setTimestamp(stamp);
        QCoreApplication::sendEvent(to, &mouse);
        break;
    }
    case Kind::KeyDown:
    case Kind::KeyUp: {
        auto key = keyFor(event.code);
        // A chord's character has no text to name it: Qt's key for the
        // character does, as a platform keyboard would report it.
        if (!key.qt && event.text.isEmpty() && event.key.size() == 1)
            key.qt = event.key.at(0).toUpper().unicode();
        QKeyEvent press(event.kind == Kind::KeyDown ? QEvent::KeyPress : QEvent::KeyRelease, key.qt,
                        modifiersOf(event.modifiers), key.scan, 0, 0, event.text);
        QCoreApplication::sendEvent(to, &press);
        break;
    }
    case Kind::Text: {
        // The IME commit route: the same renderer insertion as Input.insertText.
        QInputMethodEvent commit;
        commit.setCommitString(event.text);
        QCoreApplication::sendEvent(to, &commit);
        break;
    }
    case Kind::Wheel: {
        // WebEngine turns a phase-less mouse wheel into CSS pixels as
        // angle/120 * wheelScrollLines * 20, reading the lines once per process
        // (as here). Pixel deltas are touchpad phases, not the reference wheel.
        static const int lines = std::max(1, QGuiApplication::styleHints()->wheelScrollLines());
        const int angle = -qRound(event.deltaY * 120.0 / (lines * 20.0));
        QWheelEvent wheel(at, to->mapToGlobal(at), QPoint(), QPoint(0, angle), Qt::NoButton,
                          Qt::NoModifier, Qt::NoScrollPhase, false);
        QCoreApplication::sendEvent(to, &wheel);
        break;
    }
    }
    answer({});
}
QPointer<QObject> QtBrowserAutomation::focused() const
{
    for (const auto &guest : m_guests)
        if (guest.item && guest.item->window())
            return guest.item->window()->activeFocusItem();
    return QGuiApplication::focusObject();
}
QString QtBrowserAutomation::giveBack(QObject *back)
{
    const auto owner = [this](const QQuickItem *item) {
        for (auto it = m_guests.cbegin(); it != m_guests.cend(); ++it)
            for (auto *at = item; at; at = at->parentItem())
                if (at == it->item)
                    return it.key();
        return QString();
    };
    auto *previous = qobject_cast<QQuickItem *>(back);
    for (const auto &guest : m_guests) {
        if (!guest.item || !guest.item->window())
            continue;
        auto *current = guest.item->window()->activeFocusItem();
        const auto lent = owner(current);
        if (lent.isEmpty())
            return {};
        if (!previous || previous == current || previous->window() != guest.item->window() ||
            !owner(previous).isEmpty())
            return {};
        // view.blur(); back.focus(). An ancestor scope that already holds focus
        // (the root, when nothing else had it) ignores forceActiveFocus, so the
        // outermost focused item below it on the chain gives the focus up.
        previous->forceActiveFocus(Qt::OtherFocusReason);
        if (previous->window()->activeFocusItem() != previous) {
            QQuickItem *chain = nullptr;
            for (auto *at = current; at && at != previous; at = at->parentItem())
                if (at->hasFocus())
                    chain = at;
            if (chain)
                chain->setFocus(false, Qt::OtherFocusReason);
        }
        return lent;
    }
    return {};
}
} // namespace openghost
