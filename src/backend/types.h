#pragma once

// Semantic OpenGhost 1.3 boundary, derived from reference/openghost, NOT wire DTOs
// from the source Qt port. QtCore values only: no QML, widgets, agent or transport.
#include <QJsonObject>
#include <QMap>
#include <QStringList>
#include <QVector>
#include <optional>
#include <utility>
#include <variant>

namespace openghost
{
using RequestId = quint64; // Local call correlation, never a session/turn/RPC identity.
using Sequence = qint64;   // Wire adapter must validate 0..2^53-1 before construction.

struct Error {
    QString code, message;
    std::optional<QString> provider, action; // Named codes/actions remain extensible.
    std::optional<bool> retryable;
    std::optional<int> status;
};
enum class PermissionMode { Ask, Auto, Full };
enum class TurnStatus { Done, Cancelled, Error };
struct ModelSelection {
    QString provider, model;
    std::optional<QString> thinking;
};
struct Model {
    QString id, provider, name;
    std::optional<double> contextWindow;
    std::optional<bool> vision;
    QStringList thinkingLevels;
    std::optional<QString> defaultThinking;
};
struct AuthMethod {
    enum class Kind { ApiKey, OAuth };
    Kind kind = Kind::ApiKey;
    QString label;
    std::optional<QString> hint, url, placeholder, action;
};
struct ProviderStatus {
    bool connected = false;
    std::optional<bool> checking, waiting, keySaved;
    struct Account {
        std::optional<QString> email, plan;
    };
    std::optional<Account> account;
    std::optional<std::variant<Error, QString>> error;
};
struct Provider {
    QString id, name;
    std::optional<QString> group;
    std::optional<bool> limits;
    QVector<AuthMethod> methods;
    ProviderStatus status;
};
struct LimitWindow {
    double seconds = 0, used = 0, resets = 0;
};
struct AccountLimits {
    std::optional<QString> plan;
    QVector<LimitWindow> windows;
    struct ModelWindows {
        QString name;
        QVector<LimitWindow> windows;
    };
    struct Balance {
        QString currency;
        double total = 0;
    };
    struct Credits {
        std::optional<bool> unlimited;
        std::optional<QString> balance;
    };
    QVector<ModelWindows> models;
    QVector<Balance> balances;
    std::optional<Credits> credits;
};

// Model input and display cache MUST remain distinct. Previews cannot be resent.
struct Attachment {
    enum class Kind { Image, Text, Pdf, Video, File };
    QString id, name, mime;
    qint64 size = 0;
    Kind kind = Kind::File;
    std::optional<QString> note, path, dataUrl, text;
    std::optional<int> width, height;
    std::optional<bool> truncated;
    struct Video {
        double duration = 0;
        int width = 0, height = 0;
    };
    std::optional<Video> video;
};
struct DisplayAttachment {
    QString name;
    std::optional<qint64> size;
    std::optional<bool> image;
    std::optional<QString> url, note;
    std::optional<int> width, height;
    struct Pasted {
        std::optional<QString> preview;
        std::optional<int> lines;
    };
    struct Video {
        std::optional<double> duration;
        std::optional<QString> poster;
    };
    std::optional<Pasted> pasted;
    std::optional<Video> video;
};
struct Input {
    QString text;
    QVector<Attachment> attachments;
};
struct DisplayInput {
    QString text;
    QVector<DisplayAttachment> attachments;
};
struct ContextFile {
    enum class Kind { Text, Image, File };
    QString id, name;
    qint64 size = 0;
    Kind kind = Kind::File;
    std::optional<QString> path, text, dataUrl;
    std::optional<bool> truncated;
    std::optional<int> width, height;
};
struct UserContext {
    QString instructions;
    QVector<ContextFile> files;
};

struct BrowserState {
    enum class Status { Unavailable, Empty, Lazy, Loading, Ready, Failed, Gone };
    enum class Control { Agent, User };
    bool available = false, open = false;
    Status status = Status::Unavailable;
    Control control = Control::Agent;
    // Observations are not authentication; callers cannot promote them to it.
    static constexpr bool signedInVerified = false;
    struct Tab {
        int n = 0;
        QString tabId, state;
        bool loading = false;
        Sequence revision = 0;
        QString title, url;
        bool active = false;
    };
    struct SignInObservation {
        QString host;
        double at = 0;
    };
    QVector<Tab> tabs;
    QVector<SignInObservation> signedIn;
};
struct HostContext {
    std::optional<BrowserState> browser;
}; // null = no panel
struct HostToolSchema {
    QString name, description;
    QJsonObject parameters;
};
struct SideContext {
    QString parent;
    bool parentBusy = false, moved = false;
};
struct SessionParams {
    ModelSelection selection;
    PermissionMode permissionMode = PermissionMode::Ask;
    QString cwd, title;
    UserContext userContext;
    HostContext host;
    std::optional<SideContext> side;
};
struct Capabilities {
    bool authProviders = false, manualCompaction = false, sessionDelete = false;
    bool sessionRecovery = false, usageLimits = false;
    bool runtimePlugins = false; // initialize.capabilities.plugins.runtime
    QJsonObject extensions;
};
struct BackendInfo {
    QString name, version;
    std::optional<QString> platform;
};
struct Initialize {
    QString protocolVersion = QStringLiteral("0.1"), connectionId;
    struct Client {
        QString name, version, platform, locale;
    } client;
    QVector<HostToolSchema> tools;
    QString renderGuide;
    bool localPaths = false;
};
struct Initialized {
    QString protocolVersion = QStringLiteral("0.1");
    std::optional<BackendInfo> backend;
    Capabilities capabilities;
};
// Runtime plugins are backend-owned, global state, not host tools or a session's
// advertised tool list. IDs and state names stay extensible. No local policy.
struct Plugin {
    QString id, name, description;
    bool builtIn = false, enabled = false;
    QString state;
    bool available = false;
    qint64 activeCalls = 0;
    QString interruption;
    bool hooks = false;
    QStringList tools;
};
struct PluginsList {}; // plugin.list({})
struct EnablePlugin { // plugin.enable({pluginId})
    QString pluginId;
};
struct DisablePlugin { // plugin.disable({pluginId})
    QString pluginId;
};
struct PluginsListed {
    QVector<Plugin> plugins;
    std::optional<QString> error;
};
struct PluginUpdated {
    Plugin plugin;
    bool changed = false;
    QString persisted;
    std::optional<QString> warning;
};
struct PluginChanged { // plugin.changed {plugin}, not session-scoped
    Plugin plugin;
    // Not a wire field: the port stamps the Initialize.connectionId of the
    // connection that delivered the event. Events from any other connection
    // (late or duplicated after a replacement) are ignored, never merged.
    QString connectionId;
};
struct ModelsList {};
struct ProvidersList {};
struct SetKey {
    QString provider;
    std::optional<QString> key;
};
struct Login {
    QString provider;
};
struct CancelLogin {
    QString provider;
};
struct Logout {
    QString provider;
};
struct AnswerLogin { // A LoginStep's prompt, answered (never retained)
    QString provider, promptId, value;
};
struct GetAccountLimits {
    QString provider;
};
struct StartTurn {
    QString sessionId;
    std::optional<QString> sessionVersion; // absent = create-only, not upsert
    QString clientTurnId;
    Input input;
    SessionParams params;
};
struct StartAccepted {
    QString turnId, sessionVersion;
    std::optional<ModelSelection> selection = std::nullopt; // canonical choice, when reported
};
struct RetryTurn {
    QString sessionId, sessionVersion, clientTurnId, failedTurnId;
    SessionParams params; // deliberately no input
};
struct RetryAccepted {
    QString turnId;
    std::optional<ModelSelection> selection = std::nullopt;
};
struct SteerTurn {
    QString sessionId, turnId, clientInputId;
    Input input;
    HostContext host;
};
struct SteerAccepted {
    bool accepted = false;
};
struct CancelTurn {
    QString sessionId, turnId;
};
struct GetSession {
    QString sessionId;
    std::optional<QString> clientTurnId;
};
struct ConfigureSession {
    QString sessionId, sessionVersion;
    std::optional<QString> clientTurnId, model, provider, thinking;
    std::optional<PermissionMode> permissionMode;
};
struct SessionConfigured {
    std::optional<QString> model, provider;
    // Outer absent = unchanged; inner absent = canonical null/clear.
    std::optional<std::optional<QString>> thinking;
    std::optional<PermissionMode> permissionMode;
};
struct CompactSession {
    QString sessionId, sessionVersion, clientTurnId;
};
struct Compacted {
    bool ok = false;
};
struct DeleteSession {
    QString sessionId;
};
struct Shutdown {};
using Command = std::variant<Initialize, ModelsList, ProvidersList, SetKey, Login, CancelLogin,
                             Logout, AnswerLogin, GetAccountLimits, StartTurn, RetryTurn, SteerTurn, CancelTurn,
                             GetSession, ConfigureSession, CompactSession, DeleteSession, Shutdown,
                             PluginsList, EnablePlugin, DisablePlugin>;

// Identity is separate from payload. Optional turn/message/client IDs are NOT
// permission to apply an event to whichever turn happens to be on screen.
struct EventIdentity {
    QString sessionId;
    Sequence seq = 0;
    std::optional<QString> turnId, messageId, clientTurnId;
};
struct TurnStarted {};
struct MessageStarted {
    QString role = QStringLiteral("assistant");
    std::optional<QString> model;
};
struct MessageDelta {
    QString text;
};
struct MessageCompleted {
    std::optional<QString> text, finishReason;
};
// The model's visible thinking for its message: more of it, or (`replace`) all
// of it as the backend finally has it. Never a provider's signature or an
// encrypted payload; a redacted part is said to be redacted.
struct ReasoningDelta {
    QString text;
    bool replace = false;
};
// A tool call the model made, announced once its arguments are complete (never
// from partial argument JSON). Not proof that it ran. Calls of one assistant
// message are announced in that message's order.
struct ToolStarted {
    QString toolCallId, name;
    std::optional<QString> title;
    // The arguments as the model sent them: compact JSON text, bounded for
    // display (a clipped text is no longer valid JSON). Absent: not known.
    std::optional<QString> arguments = std::nullopt;
};
// Live output of a running call. Activity state only, not assistant text.
struct ToolProgress {
    QString toolCallId;
    QJsonObject detail;
    // What the call has output so far: the whole output so far (a snapshot,
    // as Pi's tools report it) unless `append`. Absent: no visible output.
    std::optional<QString> output = std::nullopt;
    bool append = false;
};
// A call's result. `isError` absent: the outcome was not reported (never
// read as success). A result for a call never announced carries its name.
struct ToolCompleted {
    QString toolCallId;
    QJsonObject detail;
    std::optional<QString> name = std::nullopt, output = std::nullopt;
    std::optional<bool> isError = std::nullopt;
    bool saved = false; // The backend's saved result; it settles a live one.
};
enum class Decision { Allow, Deny };
struct ApprovalResolved {
    QString approvalId;
    Decision decision = Decision::Deny;
};
struct CompactionStarted {
    std::optional<QString> reason;
};
struct CompactionCompleted {
    bool ok = false;
};
struct Usage {
    QString provider, model;
    std::optional<QString> modelName;
    double input = 0, cached = 0, written = 0, output = 0;
    std::optional<double> requests;
    struct Context {
        double used = 0, window = 0;
    };
    std::optional<Context> context;
}; // increments, never cumulative; replay must not charge the local ledger
struct InputAccepted {
    QString clientInputId;
    std::optional<DisplayInput> input;
};
struct SessionUpdated {
    std::optional<QString> title;
};
struct TurnCompleted {
    TurnStatus status = TurnStatus::Done;
    std::optional<QString> finishReason;
    std::optional<Error> error;
};
using EventPayload =
    std::variant<TurnStarted, MessageStarted, MessageDelta, MessageCompleted, ReasoningDelta,
                 ToolStarted, ToolProgress, ToolCompleted, ApprovalResolved, CompactionStarted,
                 CompactionCompleted, Usage, InputAccepted, SessionUpdated, TurnCompleted>;
struct SessionEvent {
    EventIdentity identity;
    EventPayload payload;
};
struct RecoveredTurn {
    QString clientTurnId, turnId;
    std::optional<DisplayInput> input;
    QVector<SessionEvent> events;
};
struct ExistingSession {
    QString sessionVersion;
    Sequence revision = 0;
    std::optional<RecoveredTurn> turn;
};
struct MissingSession {};
using SessionRecovery = std::variant<MissingSession, ExistingSession>;
struct AuthChanged {
    QString provider;
    ProviderStatus status;
}; // invalidation, reread
struct ModelsChanged {
    std::optional<QString> provider;
};
// The backend's current sign-in step while Login or SetKey is pending: "waiting",
// "prompt" (text input), "select" or "device_code". A promptId wants AnswerLogin.
struct LoginStep {
    struct Choice {
        QString id, label;
    };
    QString provider, type, message;
    std::optional<QString> promptId, url, userCode, placeholder;
    bool secret = false;
    QVector<Choice> options, links; // links: id = URL
};
struct Log {
    QString level, message;
};
using GlobalEvent = std::variant<AuthChanged, ModelsChanged, Log, PluginChanged, LoginStep>;
struct Null {};
using Reply = std::variant<Null, Initialized, QVector<Model>, QVector<Provider>, ProviderStatus,
                           std::optional<AccountLimits>, StartAccepted, RetryAccepted,
                           SteerAccepted, SessionRecovery, SessionConfigured, Compacted,
                           PluginsListed, PluginUpdated>;
using Result = std::variant<Reply, Error>;

struct ApprovalPresentation {
    QString kind, title;
    std::optional<QString> effect;
    std::optional<bool> badge;
    struct Place {
        QString kind, label, title;
    };
    QVector<Place> places;
    std::optional<QString> code, removed, added, quote, reveal;
};
// A decision the asker offers on an approval card beyond plain Allow and Deny
// (Pi's plugin-permissions: allow once, allow for the session, allow both
// directions for the session, deny, deny with a reason), with its shortcut key.
struct ApprovalAction {
    QString id, label, detail, key;
};
struct ApprovalRequest {
    QString sessionId, turnId, approvalId, toolCallId, tool;
    QJsonObject args;
    std::optional<ApprovalPresentation> presentation;
    // Empty: the card offers Allow and Deny alone.
    QVector<ApprovalAction> actions{};
    bool doublePressToConfirm = false; // a shortcut arms first, commits on a second press
    // A subagent's session grant: for that subagent alone, or the whole session.
    std::optional<std::pair<QString, QString>> scopes{}; // {subagent, session} labels
};
struct ApprovalAnswer {
    Decision decision = Decision::Deny;
    std::optional<QString> reason; // OpenGhost's own (why a card was dismissed)
    // The card action chosen (an ApprovalAction id), when one was; empty for a
    // plain Allow or Deny. It only says how wide an Allow is or why a Deny was
    // given: `decision` stays the answer.
    QString action{};
    std::optional<QString> note{}; // deny with a reason: what the agent is told
    QString scope{};               // a subagent's session grant: "subagent" or "session"
};
struct HostToolRequest {
    QString sessionId, turnId, toolCallId, name;
    QJsonObject args;
};
struct HostToolResult {
    struct Text {
        QString text;
    };
    struct Image {
        QString dataUrl;
        std::optional<QString> label;
    };
    QVector<std::variant<Text, Image>> content;
    bool isError = false;
    enum class Status { Ok, Error, Cancelled, HandedBack } status = Status::Ok;
    std::optional<QString> reason;
    std::optional<QJsonObject> data; // refs, page/tab IDs, download/read/screenshot metadata
};
using ReverseRequest = std::variant<ApprovalRequest, HostToolRequest>;
using ReverseResult = std::variant<ApprovalAnswer, HostToolResult, Error>;

// Frontend-owned preferences, not an invented settings RPC or model history.
struct Preferences {
    ModelSelection model;
    std::optional<QString> preferredThinking;
    PermissionMode mode = PermissionMode::Ask;
    UserContext userContext;
    // Frontend plugins the user turned on or off, by plugin ID (absent: the
    // plugin's default). Choices for plugins not registered now are kept.
    QMap<QString, bool> frontendPlugins;
    // Their on/off options the user set, by plugin ID then option key (absent:
    // the option's default). Kept like the choices above.
    QMap<QString, QMap<QString, bool>> frontendPluginOptions;
};
} // namespace openghost
