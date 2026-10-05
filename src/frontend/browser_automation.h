#pragma once
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <functional>

namespace openghost
{
// Browser-owned engine seam. No transport, backend, DevTools or page objects in
// tool contracts. Implementations never reply inline. cancel drops completion,
// not already-issued effects. The host rechecks its lease at every continuation.
class BrowserAutomation : public QObject
{
    Q_OBJECT
  public:
    using QObject::QObject;
    struct Target {
        QString tab, page;
        int incarnation = 0;
        QString document; // isolated-world document incarnation, not a DOM revision
    };
    enum class Query { Snapshot, Has, Read, Metrics, Quiet, Reveal, Point, Choose, Probe };
    // One trusted user-input event for the guest, in guest viewport CSS pixels.
    // Keys use desktop/browser.js keyOf()'s key/code/virtual key/text/modifier
    // bits (alt 1, control 2, meta 4, shift 8); Text is an IME-style commit
    // (CDP Input.insertText), not per-character keys. A press's `count` is its
    // click count within one sequence; Wheel carries the CSS-pixel deltaY.
    struct Input {
        enum class Kind { Move, Press, Release, KeyDown, KeyUp, Text, Wheel } kind = Kind::Move;
        double x = 0, y = 0, deltaY = 0;
        int count = 1, vk = 0, modifiers = 0;
        QString key, code, text;
    };
    using Done = std::function<void(QJsonObject)>;
    virtual void attach(const QString &tab, int incarnation, QObject *view) = 0;
    virtual void query(quint64 call, const Target &, Query, const QJsonObject &args, qint64 expires,
                       Done) = 0;
    virtual void navigate(const Target &, const QString &verb, const QString &url) = 0;
    virtual void capture(quint64 call, const Target &, const QJsonObject &metrics, bool full,
                         Done) = 0;
    virtual void input(quint64 call, const Target &, const Input &, Done) = 0;
    virtual void cancel(quint64 call) = 0;
    // browser-panel.js run/giveBack: the app's focused object at receipt, and
    // after a step the keyboard returns there from a guest it moved into.
    // Returns the guest tab the keyboard was lent from, or empty.
    virtual QPointer<QObject> focused() const { return {}; }
    virtual QString giveBack(QObject *back)
    {
        Q_UNUSED(back)
        return {};
    }
};
} // namespace openghost
