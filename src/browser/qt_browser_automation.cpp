#include "qt_browser_automation.h"
#include <QBuffer>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QQuickItemGrabResult>
#include <QQuickWindow>
#include <QTimer>
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
    case Query::Metrics:
        expression = "({width:innerWidth,height:innerHeight,contentHeight:(document."
                     "scrollingElement||document.documentElement).scrollHeight,top:scrollY})";
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
    const QString prepare = kind == Query::Reveal
                                ? "if(window.__og?.pageId!==" + json(target.page) +
                                      ")throw Object.assign(new Error('The page changed. Take a "
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
void QtBrowserAutomation::wheel(quint64 call, const Target &target, double amount, Done done)
{
    m_pending.insert(call, {target, std::move(done)});
    Q_UNUSED(amount)
    // Direct item delivery is not Chromium wheel delivery (focused prototype).
    // Window delivery hits app chrome when the guest is covered/closed. Do not
    // silently substitute scrollBy: it skips trusted wheel handlers/containers.
    QTimer::singleShot(0, this, [this, call] {
        complete(call, error("Targeted native wheel delivery is not yet supported. Use a fresh "
                             "snapshot ref to reveal an element.",
                             "unavailable"));
    });
}
} // namespace openghost
