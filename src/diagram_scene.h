#pragma once

#include "diagram_paint.h"

#include <QHash>
#include <QStringList>
#include <QVector>

#include <functional>

// diagram.js's Scene: reconciles a drawing's keyed items with the ones it
// shows and animates them. A new item comes in after its stagger and over
// its type's entrance; one that stays springs from where it is to its new
// props; one that is gone fades out. Times are milliseconds on the caller's
// clock. GUI-free, so it can be driven by a frame clock or a test.
namespace diagram
{
class Scene
{
  public:
    // Takes a drawing's items (the view's first): `stagger` ms between
    // orders of fresh items; `instant` sets everything where it goes.
    void set(const QVector<Spec> &specs, double now, double stagger, bool instant);
    // One frame: springs and entrances advanced to `now`. True while
    // anything still moves or comes in or goes.
    bool tick(double now);
    // Everything where it goes at once: entrances done, springs at rest,
    // what was going gone (reduced motion turned on midway).
    void finish();
    // Changes an item's fixed part (a pie's focus) without animating.
    void patch(const QString &key, const std::function<void(Fixed &)> &change);
    // The items under the pointer (.is-hot).
    void setHot(const QStringList &keys);
    void clear();

    // What is shown, in its order (the view excluded).
    QVector<const Live *> shown() const;
    // The view's props as they stand, or nullptr before any drawing.
    const Props *view() const;
    const Live *find(const QString &key) const;
    bool empty() const { return m_items.isEmpty(); }
    bool busy() const { return m_busy; }

    // Each type's entrance in ms (TYPES[type].enter).
    static double enter(const Spec &spec, const Props &cur);

  private:
    struct Item {
        Live live;
        Props vel, to;
        double from = 0, goal = 1, begin = 0, dur = 0;
    };
    void retarget(Item &item, const Props &props);
    void destroy(int index);
    void reindex();

    QVector<Item> m_items;
    QHash<QString, int> m_index;
    double m_last = -1;
    bool m_busy = false;
};

// springTo: one prop toward its goal with a stiffness and damping over dt s.
bool springTo(double &x, double &v, double goal, double dt, double k = SPRING_K,
              double c = SPRING_C);
} // namespace diagram
