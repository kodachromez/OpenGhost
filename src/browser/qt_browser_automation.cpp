#include "qt_browser_automation.h"
#include <QBuffer>
#include <QFile>
#include <QFocusEvent>
#include <QGuiApplication>
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
void QtBrowserAutomation::capture(quint64 call, const Target &target, const QJsonObject &metrics,
                                  bool full, Done done)
{
    m_pending.insert(call, {target, std::move(done)});
    emulateFocus(item(target));
    auto *view = item(target);
    const int width = qRound(metrics["width"].toDouble()),
              height = qRound(metrics["height"].toDouble());
    // A Quick item grab captures painted viewport pixels. Resizing/scrolling the
    // guest to stitch a full-page image changes layout/events: not parity.
    if (full && (metrics["contentHeight"].toDouble() > height || metrics["top"].toDouble() != 0)) {
        QTimer::singleShot(0, this, [this, call] {
            complete(call, error("Qt's viewport grab cannot capture a tall page from the top "
                                 "without changing the page. Full-page capture is not supported.",
                                 "unavailable"));
        });
        return;
    }
    if (!view || !view->window() || width <= 0 || height <= 0 || width > 16384 || height > 16384) {
        QTimer::singleShot(0, this, [this, call] {
            complete(call, error("The page has no capturable viewport"));
        });
        return;
    }
    const int outputWidth = std::min(width, 1280);
    const QSize size(outputWidth, std::max(1, qRound(double(height) * outputWidth / width)));
    const auto grab = view->grabToImage(size);
    if (!grab) {
        QTimer::singleShot(
            0, this, [this, call] { complete(call, error("The page did not draw a screenshot")); });
        return;
    }
    // The render job remains owned by Qt; the completion is call-token guarded.
    connect(
        grab.data(), &QQuickItemGrabResult::ready, this,
        [this, grab, call, width, height, size] {
            if (!m_pending.contains(call))
                return;
            QImage image = grab->image();
            if (image.isNull()) {
                complete(call, error("The page did not draw a screenshot"));
                return;
            }
            if (image.size() != size)
                image = image.scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
            QByteArray bytes;
            QBuffer buffer(&bytes);
            buffer.open(QIODevice::WriteOnly);
            if (!image.save(&buffer, "JPEG", 82)) {
                complete(call, error("Could not encode the screenshot"));
                return;
            }
            complete(call,
                     {{"image", "data:image/jpeg;base64," + QString::fromLatin1(bytes.toBase64())},
                      {"width", image.width()},
                      {"height", image.height()},
                      {"scale", double(image.width()) / width},
                      {"pageWidth", width},
                      {"pageHeight", height},
                      {"truncated", false}});
        },
        Qt::SingleShotConnection);
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
