#pragma once

#include <QPointer>
#include <QQuickItem>
#include <QtQml/qqmlregistration.h>

// browser-panel.js yieldKeys: a press anywhere outside the browser panel takes
// the keyboard back from its pages, so typing after clicking the chat never
// lands in a site. Qt Quick moves focus only to a focusable item, so a press
// on the transcript would otherwise leave a page focused. Presses inside the
// panel, and focus held outside it, are left alone. Nothing is consumed.
class BrowserFocus : public QQuickItem
{
    Q_OBJECT
    QML_ELEMENT
    // The panel; a guest page is any item under it named "browserGuest".
    Q_PROPERTY(QQuickItem *panel READ panel WRITE setPanel NOTIFY panelChanged)

  public:
    explicit BrowserFocus(QQuickItem *parent = nullptr);
    QQuickItem *panel() const { return m_panel; }
    void setPanel(QQuickItem *panel);
    // Whether `item` is (inside) one of the panel's guest pages.
    static bool inGuest(const QQuickItem *item, const QQuickItem *panel);

  signals:
    void panelChanged();
    void yielded(); // the keyboard left a page for the app

  protected:
    void itemChange(ItemChange change, const ItemChangeData &value) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

  private:
    void press(QPointF scene);
    QPointer<QQuickItem> m_panel;
    QPointer<QQuickWindow> m_window;
};
