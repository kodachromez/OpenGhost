import QtQuick
import QtQuick.Controls
import QtQuick.Effects
import OpenGhost.Native

// Settings → Usage (OpenGhost 1.2's settings-usage.js): the tokens sent and
// written back today, over 7 and 30 days and all the time, each split by
// provider; a column for each of the last 30 days (or a month, turned back
// with the arrows) with a highlight and tip on the sidebar's springs; then a
// section for every provider in use. OpenGhost's disconnected usage preview
// has no stored usage, plan limits or account balance to show.
Column {
    id: page
    required property var frontend
    // Shown and open: the page comes in (rises, counts up, bars grow).
    property bool active: false
    readonly property var usage: frontend.usage
    readonly property var settings: frontend.settings
    readonly property var periods: [["Today", 1], ["7 days", 7], ["30 days", 30], ["All time", 0]]
    // What the chart shows: "" for the last 30 days, or a month (YYYY-MM).
    property string scope: ""
    property int revision: 0
    // 0 → 1 as the page comes in or a month turns (enter, flip).
    property real enter: 1
    property real flip: 1
    width: parent ? parent.width : 0

    Connections {
        target: page.usage
        function onChanged() { page.revision++ }
    }
    onActiveChanged: {
        if (!active)
            return
        scope = ""
        revision++
        if (Theme.reducedMotion)
            return
        entering.restart()
    }
    NumberAnimation on enter {
        id: entering
        running: false
        from: 0
        to: 1
        duration: 1400
    }
    NumberAnimation on flip {
        id: flipping
        running: false
        from: 0
        to: 1
        duration: 700
    }

    // The providers' order: OpenGhost's known providers, catalog, then other usage.
    readonly property var model: {
        revision
        const all = usage.totals(0)
        // Known providers in reference order, then the catalog, then the rest.
        const first = ["openai-codex", "chatgpt", "openai", "anthropic", "deepseek"]
        const listed = settings.providers.map(p => p.id).filter(id => !first.includes(id))
        const known = first.filter(id => all[id] || connected(id))
        const used = Object.keys(all).filter(id => !first.includes(id) && !listed.includes(id)).sort()
        const order = known.concat(listed, used)
        const shown = order.filter(id => (all[id] && all[id].tokens) || connected(id))
        const scopes = [""].concat(monthsBack())
        const month = scope ? usage.between(scope + "-01", scope + "-31") : null
        const days = scope ? usage.month(scope) : usage.daily(30)
        return { order: order, shown: shown, scopes: scopes, month: month, days: days,
                 totals: periods.map(p => usage.totals(p[1])) }
    }

    // A provider's colour, as 1.2's TONES: ChatGPT turquoise, OpenAI lilac,
    // Anthropic orange, DeepSeek blue; the rest take the other tones in turn.
    readonly property var known: ({ "openai-codex": 1, chatgpt: 1, openai: 0, anthropic: 5, deepseek: 2 })
    function tone(id) {
        if (known[id] !== undefined)
            return Theme.tones[known[id]]
        const spare = [3, 4, 6, 0, 1, 2, 5]
        const others = model.order.filter(p => known[p] === undefined)
        return Theme.tones[spare[Math.max(0, others.indexOf(id)) % spare.length]]
    }
    readonly property var names: ({ "openai-codex": "ChatGPT", chatgpt: "ChatGPT", openai: "OpenAI API",
                                    anthropic: "Anthropic", deepseek: "DeepSeek",
                                    "claude-code": "Claude Code" })
    function nameOf(id) {
        const listed = settings.providers.find(p => p.id === id)
        return listed ? listed.name : names[id] ?? id
    }
    function connected(id) {
        const listed = settings.providers.find(p => p.id === id)
        return !!(listed && listed.connected)
    }
    function sum(map) {
        let total = 0
        for (const id in map)
            total += map[id].tokens
        return total
    }
    // Months with a count, back from this one, newest first.
    function monthsBack() {
        const months = usage.months()
        if (!months.length)
            return []
        const out = []
        let [year, month] = usage.thisMonth.split("-").map(Number)
        while (out.length < 240) {
            const key = year + "-" + String(month).padStart(2, "0")
            if (key < months[0])
                break
            out.push(key)
            if (--month < 1) {
                month = 12
                year--
            }
        }
        return out
    }
    function step(delta) {
        const at = model.scopes.indexOf(scope)
        const next = model.scopes[at + delta]
        if (next === undefined)
            return
        scope = next
        chart.pointed = -1
        if (!Theme.reducedMotion)
            flipping.restart()
    }
    // Intl's compact English notation: 999, 1.2K, 12.3K, 1.2M.
    function compact(n) {
        n = Math.round(n)
        if (n < 1000)
            return String(n)
        const units = [[1e12, "T"], [1e9, "B"], [1e6, "M"], [1e3, "K"]]
        for (let i = 0; i < units.length; ++i) {
            const [size, unit] = units[i]
            if (n >= size || (i < units.length - 1 && Math.round(n / units[i + 1][0] * 10) / 10 >= 1000 && n >= size / 1000 * 999.95)) {
                const value = Math.round(n / size * 10) / 10
                return String(value).replace(/\.0$/, "") + unit
            }
        }
        return String(n)
    }
    function full(n) {
        return Number(n).toLocaleString(Qt.locale("en_US"), "f", 0)
    }
    function dateOf(day) {
        const [y, m, d] = day.split("-").map(Number)
        return new Date(y, m - 1, d)
    }
    function monthName(key) {
        return Qt.formatDate(dateOf(key + "-01"), "MMMM yyyy")
    }
    // The page's rise (usage-rise): up 8 px out of a 4 px blur, 0.6 s after
    // 60 ms per item; a turned month's words 0.45 s.
    function riseAt(k) {
        const t = Math.max(0, Math.min(1, (enter * 1400 - k * 60) / 600))
        return Theme.bezier(0.32, 0.72, 0, 1, t)
    }
    function flipAt() {
        return Theme.bezier(0.32, 0.72, 0, 1, Math.max(0, Math.min(1, flip * 700 / 450)))
    }
    // Growth (usage-grow-x/-y) after `delay` ms over `length` ms.
    function grow(clock, total, delay, length) {
        return Theme.bezier(0.32, 0.72, 0, 1, Math.max(0, Math.min(1, (clock * total - delay) / length)))
    }

    UsageText {
        width: parent.width
        text: "Usage reported during this app session. Fake backend counts are fixtures, not billing."
        size: 13.5
        line: 20
        color: Theme.secondary
        wrapMode: Text.Wrap
    }
    // .settings-lead's 4 px collapses into the card's 18.
    Item { width: 1; height: 18 }

    // .usage-summary: padding 18/18/14, radius 18, the composer's colours.
    Rectangle {
        id: summary
        width: parent.width
        height: summaryColumn.height + 32
        radius: 18
        color: Theme.composerBorder
        Rectangle {
            anchors.fill: parent
            anchors.margins: 1
            radius: 17
            color: Theme.composerBg
        }
        Column {
            id: summaryColumn
            x: 18
            y: 18
            width: parent.width - 36
            // The four periods: name, value (counting up) and the split.
            Row {
                spacing: 20
                Repeater {
                    model: page.periods
                    delegate: Item {
                        id: period
                        required property var modelData
                        required property int index
                        readonly property var totals: page.model.totals[index]
                        readonly property real total: page.sum(totals)
                        readonly property real rise: page.riseAt(index)
                        width: (summaryColumn.width - 60) / 4
                        height: 64.75
                        opacity: rise
                        layer.enabled: rise < 1
                        layer.effect: MultiEffect { blurEnabled: true; blurMax: 16; autoPaddingEnabled: true; blur: Math.min(1, 4 * (1 - period.rise) / (0.27 * 16)) }
                        transform: Translate { y: 8 * (1 - period.rise) }
                        UsageText {
                            text: period.modelData[0]
                            size: 12.5
                            cssWeight: 500
                            color: Theme.secondary
                        }
                        UsageText {
                            y: 24.75
                            width: parent.width
                            objectName: "usagePeriod-" + period.index
                            // The big numbers run up to their value (0.7 s, ease-out).
                            text: page.compact(period.total * (1 - Math.pow(1 - Math.min(1, page.enter * 1400 / 700), 3)))
                            size: 24
                            line: 28
                            cssWeight: 650
                            font.letterSpacing: -0.48
                            font.features: { "tnum": 1 }
                            color: Theme.text
                            elide: Text.ElideRight
                        }
                        // .usage-split: 4 px, 2 px apart, each as wide as its share.
                        Row {
                            id: split
                            y: 60.75
                            width: parent.width
                            height: 4
                            spacing: 2
                            readonly property var parts: page.model.order.filter(id => period.totals[id] && period.totals[id].tokens)
                            readonly property real grown: page.grow(page.enter, 1400, 180, 800)
                            Rectangle {
                                visible: split.parts.length === 0
                                width: parent.width
                                height: 4
                                radius: 2
                                color: Theme.alpha(Theme.fg, 0.07)
                            }
                            Repeater {
                                model: split.parts
                                delegate: Rectangle {
                                    required property string modelData
                                    readonly property real share: period.totals[modelData].tokens / Math.max(1, period.total)
                                    width: Math.max(4, (split.width - 2 * (split.parts.length - 1)) * share)
                                    height: 4
                                    radius: 2
                                    color: page.tone(modelData)
                                    transform: Scale { xScale: split.grown }
                                }
                            }
                        }
                    }
                }
            }
            // .usage-range: what the chart shows, its total and the arrows.
            Item {
                width: parent.width
                height: 22 + 21
                opacity: page.flipAt()
                transform: Translate { y: 8 * (1 - page.flipAt()) }
                layer.enabled: page.flipAt() < 1
                layer.effect: MultiEffect { blurEnabled: true; blurMax: 16; autoPaddingEnabled: true; blur: Math.min(1, 4 * (1 - page.flipAt()) / (0.27 * 16)) }
                UsageText {
                    id: rangeTitle
                    y: 22
                    text: page.scope ? page.monthName(page.scope) : "Last 30 days"
                    size: 14
                    cssWeight: 600
                    color: Theme.text
                }
                UsageText {
                    x: rangeTitle.implicitWidth + 10
                    y: 22 + (21 - 19.5) / 2
                    readonly property real total: page.model.days.reduce((s, d) => s + Object.values(d.providers).reduce((a, b) => a + b, 0), 0)
                    text: page.compact(total) + " tokens"
                    size: 13
                    color: Theme.secondary
                    font.features: { "tnum": 1 }
                }
                Row {
                    x: parent.width + 6 - width
                    y: 22 + (21 - 28) / 2
                    spacing: 2
                    NavButton {
                        objectName: "usageNavEarlier"
                        later: false
                        enabled: page.model.scopes.indexOf(page.scope) < page.model.scopes.length - 1
                        Accessible.name: "Earlier"
                        onClicked: page.step(1)
                    }
                    NavButton {
                        objectName: "usageNavLater"
                        later: true
                        enabled: page.model.scopes.indexOf(page.scope) > 0
                        Accessible.name: "Later"
                        onClicked: page.step(-1)
                    }
                }
            }
            Item { width: 1; height: 14 }
            // .usage-chart: a column per day, 84 px tall, 3 px apart.
            Item {
                id: chart
                objectName: "usageChart"
                property int pointed: -1
                readonly property var days: page.model.days
                readonly property var sums: days.map(d => Object.values(d.providers).reduce((a, b) => a + b, 0))
                readonly property real peak: Math.max(1, ...sums)
                readonly property real pitch: (width + 3) / Math.max(1, days.length)
                width: parent.width
                height: 84 + 8 + 17.25
                // The highlight and the tip spring to the day pointed at
                // (520/40); coming in, they appear there and fade up (320/32).
                Spring { id: glideX; k: 520; c: 40; within: 0.01; slower: 0.05 }
                Spring { id: glideW; k: 520; c: 40; within: 0.01; slower: 0.05 }
                Spring { id: tipX; k: 520; c: 40; within: 0.01; slower: 0.05 }
                Spring { id: shown; goal: chart.pointed >= 0 ? 1 : 0; k: 320; c: 32; within: 0.01; slower: 0.05 }
                onPointedChanged: {
                    if (pointed < 0)
                        return
                    const x = pointed * pitch, w = pitch - 3 + 4
                    const at = Math.max(0, Math.min(width - tip.width, x + (pitch - 3) / 2 - tip.width / 2))
                    for (const [spring, goal] of [[glideX, x], [glideW, w], [tipX, at]]) {
                        if (shown.value < 0.02) {
                            spring.goal = goal
                            spring.snap()
                        } else {
                            spring.goal = goal
                        }
                    }
                }
                Rectangle {
                    objectName: "usageGlide"
                    x: glideX.value - 2
                    y: -6
                    width: Math.max(0, glideW.value)
                    height: 96
                    radius: 8
                    color: Theme.alpha(Theme.fg, 0.06)
                    opacity: Math.max(0, Math.min(1, shown.value))
                }
                Repeater {
                    model: chart.days
                    delegate: Item {
                        id: day
                        required property var modelData
                        required property int index
                        readonly property real total: chart.sums[index]
                        readonly property real h: total ? Math.max(0.04, total / chart.peak) : 0
                        // Coming in, the days rise one after another (0.7 s
                        // after 120 + 16 ms each); a turned month's 0.55 s after 10 ms each.
                        readonly property real grown: Math.min(page.grow(page.enter, 1400, 120 + index * 16, 700),
                                                               page.grow(page.flip, 700, index * 10, 550))
                        objectName: "usageDay-" + index
                        x: index * chart.pitch
                        width: chart.pitch - 3
                        height: 84
                        Rectangle {
                            visible: !day.total
                            y: 84 - 3
                            width: parent.width
                            height: 3
                            radius: 2
                            color: Theme.alpha(Theme.fg, 0.08)
                        }
                        Item {
                            id: bar
                            visible: day.total > 0
                            y: 84 - height
                            width: parent.width
                            height: 84 * day.h
                            transform: Scale { origin.y: bar.height; yScale: day.grown }
                            // Radius 4 4 2 2; each provider's share, 1.5 px apart.
                            layer.enabled: true
                            layer.effect: MultiEffect {
                                maskEnabled: true
                                maskSource: barMask
                            }
                            Rectangle {
                                id: barMask
                                anchors.fill: parent
                                topLeftRadius: 4
                                topRightRadius: 4
                                bottomLeftRadius: 2
                                bottomRightRadius: 2
                                visible: false
                                layer.enabled: true
                            }
                            Column {
                                anchors.bottom: parent.bottom
                                width: parent.width
                                spacing: 1.5
                                readonly property var parts: page.model.order.filter(id => day.modelData.providers[id]).reverse()
                                Repeater {
                                    model: parent.parts
                                    delegate: Rectangle {
                                        required property string modelData
                                        readonly property int n: parent.parts.length
                                        width: parent.width
                                        height: Math.max(2, (bar.height - 1.5 * (n - 1)) * day.modelData.providers[modelData] / Math.max(1, day.total))
                                        color: page.tone(modelData)
                                    }
                                }
                            }
                        }
                    }
                }
                // The day under the pointer is the column nearest it anywhere
                // over the chart, the gaps between columns included.
                MouseArea {
                    x: -4
                    y: -10
                    width: parent.width + 8
                    height: 84 + 34
                    hoverEnabled: true
                    onPositionChanged: mouse => {
                        const k = Math.floor((mouse.x - 4) / (chart.width / Math.max(1, chart.days.length)))
                        chart.pointed = Math.max(0, Math.min(chart.days.length - 1, k))
                    }
                    onExited: chart.pointed = -1
                }
                // .usage-chart-axis: the first day and Today (or the month's last).
                UsageText {
                    y: 84 + 8
                    text: chart.days.length ? Qt.formatDate(page.dateOf(chart.days[0].day), "MMM d") : ""
                    size: 11.5
                    color: Theme.tertiary
                }
                UsageText {
                    x: parent.width - implicitWidth
                    y: 84 + 8
                    text: page.scope && chart.days.length
                          ? Qt.formatDate(page.dateOf(chart.days[chart.days.length - 1].day), "MMM d") : "Today"
                    size: 11.5
                    color: Theme.tertiary
                }
                // .usage-tip: what each provider used that day, above the highlight.
                Item {
                    id: tip
                    objectName: "usageTip"
                    readonly property var day: chart.pointed >= 0 ? chart.days[chart.pointed] : null
                    x: tipX.value
                    y: -6 - height + 4 * (1 - Math.max(0, Math.min(1, shown.value)))
                    z: 3
                    width: Math.max(140, tipColumn.implicitWidth + 24)
                    height: tipColumn.implicitHeight + 17
                    opacity: Math.max(0, Math.min(1, shown.value))
                    visible: opacity > 0
                    BoxShadow {
                        anchors.fill: parent
                        radius: 14
                        blur: 34
                        offsetY: 14
                        color: Theme.alpha("black", 0.45 * Theme.shadow)
                    }
                    Rectangle {
                        anchors.fill: parent
                        radius: 14
                        color: Qt.tint(Theme.composerBg, Theme.tipBg)
                        border.width: 1
                        border.color: Theme.alpha(Theme.fg, 0.08)
                    }
                    Column {
                        id: tipColumn
                        x: 12
                        y: 8
                        UsageText {
                            text: tip.day ? Qt.formatDate(page.dateOf(tip.day.day), "ddd, MMM d") : ""
                            size: 12.5
                            line: 20
                            cssWeight: 500
                            color: Theme.secondary
                        }
                        UsageText {
                            text: !tip.day ? "" : chart.sums[chart.pointed] ? page.full(chart.sums[chart.pointed]) + " tokens" : "Nothing that day"
                            size: 12.5
                            line: 20
                            cssWeight: 600
                            color: Theme.text
                        }
                        Repeater {
                            model: tip.day ? page.model.order.filter(id => tip.day.providers[id]) : []
                            delegate: Item {
                                required property string modelData
                                width: Math.max(tipColumn.width, tipName.implicitWidth + 15 + 16 + tipValue.implicitWidth)
                                implicitWidth: tipName.implicitWidth + 15 + 16 + tipValue.implicitWidth
                                height: 20
                                Rectangle {
                                    y: 6
                                    width: 8
                                    height: 8
                                    radius: 4
                                    color: page.tone(modelData)
                                }
                                UsageText {
                                    id: tipName
                                    x: 15
                                    text: page.nameOf(modelData)
                                    size: 12.5
                                    line: 20
                                    cssWeight: 500
                                    color: Theme.text
                                }
                                UsageText {
                                    id: tipValue
                                    x: parent.width - implicitWidth
                                    text: page.compact(tip.day.providers[modelData])
                                    size: 12.5
                                    line: 20
                                    cssWeight: 600
                                    color: Theme.text
                                    font.features: { "tnum": 1 }
                                }
                            }
                        }
                    }
                }
            }
            Item { width: 1; height: 14 }
            // .usage-foot: the legend (each provider's share of the chart)
            // and since when the count runs, at the right when both fit on
            // one line, else on the next (8 px apart).
            Item {
                readonly property bool together: legend.childrenRect.width + 16 + since.implicitWidth <= width
                width: parent.width
                height: together ? Math.max(legend.height, since.height) : legend.height + 8 + since.height
                opacity: page.flipAt()
                transform: Translate { y: 8 * (1 - page.flipAt()) }
                layer.enabled: page.flipAt() < 1
                layer.effect: MultiEffect { blurEnabled: true; blurMax: 16; autoPaddingEnabled: true; blur: Math.min(1, 4 * (1 - page.flipAt()) / (0.27 * 16)) }
                Flow {
                    id: legend
                    width: parent.width
                    spacing: 14
                    readonly property var shownTotals: page.scope ? page.model.month : page.usage.totals(30)
                    Repeater {
                        model: page.model.order.filter(id => parent.shownTotals[id] && parent.shownTotals[id].tokens)
                        delegate: Row {
                            required property string modelData
                            spacing: 6
                            height: 18.75
                            Rectangle {
                                anchors.verticalCenter: parent.verticalCenter
                                width: 8
                                height: 8
                                radius: 4
                                color: page.tone(modelData)
                            }
                            UsageText {
                                text: page.nameOf(modelData)
                                size: 12.5
                                color: Theme.secondary
                            }
                            UsageText {
                                text: page.compact(parent.parent.shownTotals[modelData].tokens)
                                size: 12.5
                                cssWeight: 600
                                color: Theme.text
                                font.features: { "tnum": 1 }
                            }
                        }
                    }
                }
                UsageText {
                    id: since
                    readonly property real at: { page.revision; return page.usage.since }
                    x: parent.together ? parent.width - implicitWidth : 0
                    y: parent.together ? 0 : legend.height + 8
                    text: at ? "Counted since " + Qt.formatDate(new Date(at), "MMMM d, yyyy")
                             : "Counting starts with your next message"
                    size: 12.5
                    color: Theme.tertiary
                }
            }
        }
    }
    Item { width: 1; height: 8 }

    // A section for every provider in use (.usage-provider).
    Repeater {
        model: page.model.shown
        delegate: Column {
            id: section
            required property string modelData
            required property int index
            readonly property bool on: page.connected(modelData)
            readonly property var all: page.model.totals[3][modelData]
            readonly property var month: page.scope ? (page.model.month[modelData] || null) : null
            readonly property real rise: page.riseAt(3)
            objectName: "usageProvider-" + modelData
            width: page.width
            opacity: rise
            transform: Translate { y: 8 * (1 - section.rise) }
            layer.enabled: rise < 1
            layer.effect: MultiEffect { blurEnabled: true; blurMax: 16; autoPaddingEnabled: true; blur: Math.min(1, 4 * (1 - section.rise) / (0.27 * 16)) }
            // + .provider: a .07 line; its 6 px margin collapses into the
            // 20 px below the section before. Padding 20 px on top.
            Rectangle {
                visible: section.index > 0
                width: parent.width
                height: 1
                color: Theme.alpha(Theme.fg, 0.07)
            }
            Item {
                width: parent.width
                height: 20 + 22.5
                UsageText {
                    y: 20
                    text: page.nameOf(section.modelData)
                    size: 15
                    cssWeight: 600
                    font.letterSpacing: -0.15
                    color: Theme.text
                }
                // .provider-state: a quiet pill, "Not connected".
                Rectangle {
                    visible: !section.on
                    x: parent.width - width
                    y: 20 + (22.5 - height) / 2
                    width: stateText.implicitWidth + 20
                    height: 22
                    radius: 11
                    color: Theme.alpha(Theme.fg, 0.06)
                    UsageText {
                        id: stateText
                        x: 10
                        y: 3
                        text: "Not connected"
                        size: 12
                        line: 16
                        cssWeight: 600
                        color: Theme.secondary
                    }
                }
            }
            Column {
                width: parent.width
                // A turned month's numbers come in anew (usage-rise, 0.45 s).
                opacity: (section.on ? 1 : 0.55) * page.flipAt()
                transform: Translate { y: 8 * (1 - page.flipAt()) }
                layer.enabled: page.flipAt() < 1
                layer.effect: MultiEffect { blurEnabled: true; blurMax: 16; autoPaddingEnabled: true; blur: Math.min(1, 4 * (1 - page.flipAt()) / (0.27 * 16)) }
                readonly property var totals: page.scope ? section.month : section.all
                readonly property bool empty: !totals || !totals.tokens
                // .usage-empty: margin 14 0 20.
                UsageText {
                    visible: parent.empty
                    topPadding: 14 + Theme.halfLeading(font, line)
                    width: parent.width
                    text: page.scope ? "Nothing in " + page.monthName(page.scope) + "." : "No usage yet: it shows up here once you chat with these models."
                    size: 13
                    line: 18
                    color: Theme.secondary
                    wrapMode: Text.Wrap
                }
                Item { visible: parent.empty; width: 1; height: 20 }
                // .usage-stats: four numbers, 16 px below the name.
                Item { visible: !parent.empty; width: 1; height: 16 }
                Row {
                    visible: !parent.empty
                    spacing: 20
                    readonly property var cells: page.scope
                        ? [["Tokens", section.month?.tokens ?? 0], ["Input", section.month?.input ?? 0],
                           ["Output", section.month?.output ?? 0], ["Requests", section.month?.requests ?? 0, true]]
                        : page.periods.map((p, i) => [p[0], (page.model.totals[i][section.modelData] || {}).tokens || 0])
                    Repeater {
                        model: parent.cells
                        delegate: Column {
                            required property var modelData
                            width: (page.width - 60) / 4
                            spacing: 2
                            UsageText {
                                text: modelData[0]
                                size: 12.5
                                cssWeight: 500
                                color: Theme.secondary
                            }
                            UsageText {
                                text: modelData[2] ? page.full(modelData[1]) : page.compact(modelData[1])
                                size: 16
                                cssWeight: 600
                                color: Theme.text
                                font.features: { "tnum": 1 }
                            }
                        }
                    }
                }
                // .usage-facts: one model is named among them; input, cache,
                // output and requests, " · " apart.
                UsageText {
                    readonly property var t: parent.totals
                    readonly property var models: t ? Object.keys(t.models).sort((a, b) => t.models[b] - t.models[a]) : []
                    readonly property int share: t && t.input ? Math.round(t.cached / t.input * 100) : 0
                    visible: !parent.empty && text.length > 0
                    topPadding: 12 + Theme.halfLeading(font, line)
                    width: parent.width
                    text: !t ? "" : [
                        ...(models.length === 1 ? [page.usage.nameOf(models[0])] : []),
                        ...(page.scope ? [] : ["Input " + page.compact(t.input)]),
                        ...(share ? [share + "% of it from cache"] : []),
                        ...(page.scope ? [] : ["Output " + page.compact(t.output),
                                               page.full(t.requests) + (t.requests === 1 ? " request" : " requests")])
                    ].join("  ·  ")
                    size: 12.5
                    line: 18
                    color: Theme.secondary
                    wrapMode: Text.Wrap
                }
                Item {
                    visible: !parent.empty && modelsColumn.count < 2
                    width: 1
                    height: 20
                }
                // .usage-models: every model used, the busiest first, with
                // its share of the provider's tokens.
                Item { visible: modelsColumn.count > 1; width: 1; height: 14 }
                Column {
                    id: modelsColumn
                    readonly property var t: parent.totals
                    readonly property var ids: t ? Object.keys(t.models).sort((a, b) => t.models[b] - t.models[a]) : []
                    readonly property int count: parent.empty ? 0 : ids.length
                    visible: count > 1
                    width: parent.width
                    spacing: 8
                    Repeater {
                        model: modelsColumn.count > 1 ? modelsColumn.ids : []
                        delegate: Item {
                            required property string modelData
                            readonly property real share: modelsColumn.t.models[modelData] / Math.max(1, modelsColumn.t.tokens)
                            width: modelsColumn.width
                            height: 19.5
                            UsageText {
                                width: Math.min(190, implicitWidth)
                                text: page.usage.nameOf(modelData)
                                size: 13
                                color: Theme.text
                                elide: Text.ElideRight
                            }
                            UsageText {
                                id: modelValue
                                x: parent.width - width
                                width: Math.max(44, implicitWidth)
                                horizontalAlignment: Text.AlignRight
                                text: page.compact(modelsColumn.t.models[modelData])
                                size: 13
                                color: Theme.secondary
                                font.features: { "tnum": 1 }
                            }
                            Rectangle {
                                x: 190 + 14
                                y: (19.5 - 4) / 2
                                width: parent.width - 190 - 14 - 14 - modelValue.width
                                height: 4
                                radius: 2
                                color: Theme.alpha(Theme.fg, 0.06)
                                Rectangle {
                                    width: parent.width * share
                                    height: 4
                                    radius: 2
                                    color: Theme.alpha(page.tone(section.modelData), 0.75)
                                    transform: Scale { xScale: page.grow(page.enter, 1400, 180, 800) }
                                }
                            }
                        }
                    }
                }
                Item { visible: modelsColumn.count > 1; width: 1; height: 20 }
            }
        }
    }

    component UsageText: Text {
        property real size: 13
        property real line: size * 1.5
        property int cssWeight: 400
        font.pointSize: Theme.points(size)
        font.weight: Theme.weight(cssWeight)
        lineHeightMode: Text.FixedHeight
        lineHeight: line
        topPadding: Theme.halfLeading(font, line)
        bottomPadding: -Theme.halfLeading(font, line)
        height: topPadding - Theme.halfLeading(font, line) + Math.max(1, lineCount) * line
        textFormat: Text.PlainText
    }

    // .usage-nav: a 28 px round arrow, a .07 fill under the pointer, .9
    // pressed, .3 when there is nowhere to go.
    component NavButton: AbstractButton {
        id: nav
        property bool later
        width: 28
        height: 28
        hoverEnabled: true
        focusPolicy: Qt.StrongFocus
        opacity: enabled ? 1 : 0.3
        scale: down ? 0.9 : 1
        Behavior on scale { NumberAnimation { duration: 200; easing.type: Easing.Bezier; easing.bezierCurve: Theme.motion } }
        background: Rectangle {
            radius: 14
            color: nav.enabled && (nav.hovered || nav.visualFocus) ? Theme.alpha(Theme.fg, 0.07) : "transparent"
            Behavior on color { ColorAnimation { duration: 200 } }
        }
        contentItem: Item {
            PathIcon {
                anchors.centerIn: parent
                width: 13
                height: 13
                name: "chevron-back"
                color: nav.enabled && (nav.hovered || nav.visualFocus) ? Theme.text : Theme.secondary
                transform: Scale { origin.x: 6.5; xScale: nav.later ? -1 : 1 }
            }
        }
    }
}
