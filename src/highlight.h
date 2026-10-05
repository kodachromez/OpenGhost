#pragma once

#include "markdown.h"

// Code highlighting as style runs over the exact source (openghost/
// highlight.js): the source is never changed, only described.
namespace highlight
{
enum Token : quint8 {
    Plain,
    Keyword,
    String,
    Number,
    Comment,
    Function,
    Type,
    Property,
    Inserted,
    Deleted,
    ArtLine,  // Box-drawing runs in text diagrams.
    ArtArrow, // Arrows in text diagrams.
};

// Runs (format = Token) for `source` in language `lang` (a fence label).
QVector<markdown::Run> code(const QString &source, const QString &lang);
// Runs for a box-drawing text diagram.
QVector<markdown::Run> art(const QString &source);

// Highlighting that a longer source can continue (a growing fence): runs
// before `resume`, a line start the scan passed outside any token with the
// next few characters already known, are the same for every source that
// extends this one, so only the rest is scanned again.
struct Resumable {
    QVector<markdown::Run> runs;
    int resume = 0;
};
// As code() and art(); `previous` highlighted a source this one extends,
// in the same language (else pass {}).
Resumable code(const QString &source, const QString &lang, const Resumable &previous);
Resumable art(const QString &source, const Resumable &previous);

// Characters this thread's scans have examined (tests: scan work is linear).
qint64 work();
} // namespace highlight
