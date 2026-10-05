#pragma once

#include "diagram_engine.h"

#include <QPainterPath>
#include <QRectF>

class QPainter;

// The dg-* rules of OpenGhost 1.3's styles.css and diagram.js's item types
// (TYPES.apply): how each scene item draws, at any point of its entrance.
namespace diagram
{
// An item as shown now: its spec, the props it has reached and how far it
// has come in (0 … 1), and whether the pointer lights it (.is-hot).
struct Live {
    Spec spec;
    Props cur;
    double appear = 1;
    bool hot = false;
};

// What lights the whole drawing: a probed chart dims what is not hot
// (.is-probing), a hovered block lights its arrows (.is-lighting, `lit` its
// id), and the rows' shared highlight (.dg-glide).
struct Look {
    bool probing = false;
    QString lit;
    QString hovered;     // The block under the pointer (.dg-node:hover).
    bool segHot = false; // A files segment is hot: the others step back.
    QRectF glide;
    double glideOpacity = 0;
};

// Where an item answers the pointer, in drawing coordinates.
struct Hit {
    QString key;
    QPainterPath shape;
    bool node = false; // A .dg-node: hovering it lights its arrows.
};

// Paints the items in drawing coordinates, layer by layer in their order.
// `texts` gets each line of text's box in device-independent coordinates of
// the painter (its world transform applied), `hits` what answers the pointer.
void paintItems(QPainter &painter, Ctx &fonts, const QVector<const Live *> &items, const Look &look,
                const QString &kind, QVector<QRectF> *texts = nullptr,
                QVector<Hit> *hits = nullptr);
} // namespace diagram
