#pragma once
#include "backend/fake_backend.h"
#include <algorithm>

// Scripted semantic backend. Only plugin requests are held for explicit replies;
// chat/catalog calls use the existing fake. No live transport or backend policy.
class PluginBackend final : public openghost::Backend
{
  public:
    openghost::FakeBackend fake{nullptr, 0};
    bool runtimePlugins = true;
    QVector<openghost::Command> commands;
    QVector<openghost::RequestId> ids, cancelled; // parallel to commands; cancelRequest calls
    QVector<QPair<openghost::RequestId, openghost::Command>> waiting;
    PluginBackend()
    {
        connect(&fake, &Backend::replied, this,
                [this](openghost::RequestId id, openghost::Result result) {
                    if (auto *reply = std::get_if<openghost::Reply>(&result))
                        if (auto *hello = std::get_if<openghost::Initialized>(reply))
                            hello->capabilities.runtimePlugins = runtimePlugins;
                    emit replied(id, result);
                });
        connect(&fake, &Backend::sessionEvent, this, &Backend::sessionEvent);
        connect(&fake, &Backend::globalEvent, this, &Backend::globalEvent);
    }
    void request(openghost::RequestId id, const openghost::Command &command) override
    {
        commands.append(command);
        ids.append(id);
        if (std::holds_alternative<openghost::PluginsList>(command) ||
            std::holds_alternative<openghost::EnablePlugin>(command) ||
            std::holds_alternative<openghost::DisablePlugin>(command))
            waiting.append({id, command});
        else
            fake.request(id, command);
    }
    template <class T> int count() const
    {
        return int(std::count_if(commands.cbegin(), commands.cend(),
                                 [](const auto &c) { return std::holds_alternative<T>(c); }));
    }
    template <class T> openghost::RequestId next() const
    {
        for (const auto &call : waiting)
            if (std::holds_alternative<T>(call.second))
                return call.first;
        return 0;
    }
    template <class T> T command() const
    {
        for (const auto &call : waiting)
            if (std::holds_alternative<T>(call.second))
                return std::get<T>(call.second);
        Q_UNREACHABLE();
    }
    void finish(openghost::RequestId id, const openghost::Result &result)
    {
        waiting.removeIf([id](const auto &call) { return call.first == id; });
        emit replied(id, result);
    }
    void publish(const openghost::Plugin &p) { emit globalEvent(openghost::PluginChanged{p}); }
    void disconnectBackend()
    {
        emit closed(
            openghost::Error{"backend_unavailable", "Fixture disconnected", {}, {}, {}, {}});
    }
    void cancelRequest(openghost::RequestId id) override
    {
        cancelled.append(id);
        fake.cancelRequest(id);
    }
    template <class T> openghost::RequestId last() const
    {
        for (auto i = commands.size() - 1; i >= 0; --i)
            if (std::holds_alternative<T>(commands.at(i)))
                return ids.at(i);
        return 0;
    }
    void answer(openghost::RequestId id, const openghost::ReverseResult &r) override
    {
        fake.answer(id, r);
    }
    void browserChanged(const openghost::BrowserState &) override {}
};
inline openghost::Plugin plugin(QString id = QStringLiteral("vendor.future"),
                                QString state = QStringLiteral("disabled"), bool available = true)
{
    openghost::Plugin p;
    p.id = id;
    p.name = QStringLiteral("Future plugin");
    p.description = QStringLiteral("Backend-provided description");
    p.state = state;
    p.enabled = state == "enabled";
    p.available = available;
    return p;
}
inline openghost::Reply listed(QVector<openghost::Plugin> plugins,
                               std::optional<QString> error = {})
{
    return openghost::PluginsListed{std::move(plugins), error};
}
inline openghost::Reply updated(const openghost::Plugin &p, std::optional<QString> warning = {},
                                QString persisted = QStringLiteral("saved"))
{
    return openghost::PluginUpdated{p, true, persisted, warning};
}
