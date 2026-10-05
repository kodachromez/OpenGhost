#include "window.h"
#include "platform/platform.h"
#include "rich.h"

#include <QClipboard>
#include <QGuiApplication>
#include <QMimeData>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QQmlNetworkAccessManagerFactory>
#include <QQuickStyle>
#include <QQuickTextDocument>
#include <algorithm>

// OpenGhost's native window allows no QML image/font/style networking.
namespace
{
class DenyAll final : public QQmlNetworkAccessManagerFactory
{
    class Manager final : public QNetworkAccessManager
    {
      public:
        using QNetworkAccessManager::QNetworkAccessManager;

      protected:
        QNetworkReply *createRequest(Operation operation, const QNetworkRequest &,
                                     QIODevice *data) override
        {
            return QNetworkAccessManager::createRequest(operation, QNetworkRequest(), data);
        }
    };

  public:
    QNetworkAccessManager *create(QObject *parent) override
    {
        auto *manager = new Manager(parent);
        manager->setAutoDeleteReplies(true);
        return manager;
    }
};
} // namespace

QQmlNetworkAccessManagerFactory *denyNetwork()
{
    static DenyAll factory;
    return &factory;
}

void selectControlsStyle()
{
    if (qEnvironmentVariableIsEmpty("QT_QUICK_CONTROLS_STYLE"))
        QQuickStyle::setStyle(QStringLiteral("Fusion"));
}

WindowController::WindowController(QObject *parent) : QObject(parent)
{
    registerNativeTypes();
    Account account;
    account.catalogError = account.providersError = backendUnavailable();
    account.providersLoaded = true; // No discovery is pending; the error is terminal.
    m_settings.apply(account);
}

void WindowController::close() { emit closeRequested(); }

void WindowController::newChat()
{
    m_transcript.reset({});
    emit conversationReplaced({});
}

// These clipboard and selection helpers are retained from window.cpp.
bool WindowController::pasteRefused() const
{
    const QMimeData *data = QGuiApplication::clipboard()->mimeData(QClipboard::Clipboard);
    if (!data)
        return false;
    if (data->hasImage())
        return true;
    const auto urls = data->urls();
    return std::any_of(urls.cbegin(), urls.cend(),
                       [](const QUrl &url) { return url.isLocalFile(); });
}

void WindowController::copy(const QString &text)
{
    if (!text.isEmpty())
        QGuiApplication::clipboard()->setText(text, QClipboard::Clipboard);
}

QString WindowController::selectedText(QQuickTextDocument *document, int start, int end) const
{
    return document ? InlineFormat::selected(document->textDocument(), start, end) : QString();
}

void WindowController::copySelection(QQuickTextDocument *document, int start, int end)
{
    copy(selectedText(document, start, end));
}

bool WindowController::openExternal(const QString &url) { return platform::openLink(url, false); }

bool WindowController::openLink(const QString &url) { return platform::openLink(url, true); }
