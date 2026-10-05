#pragma once

#include "presentation.h"

// The next run's model choice over OpenGhost's catalog. Explicit choices also request
// saved defaults through the client; adopting session metadata never does.
// OpenGhost's backend owns persistence. The selection never changes a running turn.
class Settings final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QStringList providerIds READ providerIds NOTIFY changed)
    Q_PROPERTY(QStringList providerNames READ providerNames NOTIFY changed)
    Q_PROPERTY(QString provider READ provider NOTIFY changed)
    Q_PROPERTY(QStringList modelIds READ modelIds NOTIFY changed)
    Q_PROPERTY(QStringList modelNames READ modelNames NOTIFY changed)
    Q_PROPERTY(QString model READ model NOTIFY changed)
    Q_PROPERTY(QString modelLabel READ modelLabel NOTIFY changed)
    Q_PROPERTY(QVariantList choices READ choices NOTIFY changed)
    Q_PROPERTY(QStringList levels READ levels NOTIFY changed)
    Q_PROPERTY(QString thinking READ thinking NOTIFY changed)
    Q_PROPERTY(bool invalid READ invalid NOTIFY changed)
    Q_PROPERTY(bool canSave READ canSave NOTIFY changed)
    Q_PROPERTY(QString defaultsText READ defaultsText NOTIFY changed)
    Q_PROPERTY(QString catalogError READ catalogError NOTIFY changed)
    Q_PROPERTY(QString notice READ notice NOTIFY changed)
    Q_PROPERTY(bool noticeError READ noticeError NOTIFY changed)
    Q_PROPERTY(bool saving READ saving NOTIFY changed)
    Q_PROPERTY(QVariantList providers READ providers NOTIFY providersChanged)
    Q_PROPERTY(bool providersLoaded READ providersLoaded NOTIFY providersChanged)
    Q_PROPERTY(QString providersError READ providersError NOTIFY providersChanged)
    Q_PROPERTY(QVariantMap login READ login NOTIFY providersChanged)

  public:
    using QObject::QObject;

    // OpenGhost's latest catalog, defaults and provider state.
    void apply(const Account &account);
    // A conversation's or OpenGhost's default model: selected as given, thinking resolved.
    void use(const Selection &selection);
    Selection selection() const;

    QStringList providerIds() const { return m_providerIds; }
    QStringList providerNames() const;
    QString provider() const { return m_provider; }
    QStringList modelIds() const;
    QStringList modelNames() const;
    QString model() const { return m_selected.model; }
    QString modelLabel() const;
    // The model stage's rows: every available model, grouped by provider in
    // catalog order (provider, providerName, id, name, imageInput).
    QVariantList choices() const;
    QStringList levels() const;
    QString thinking() const { return m_selected.thinking; }
    bool invalid() const { return m_invalid; }
    bool canSave() const;
    QString defaultsText() const;
    QString catalogError() const { return m_account.catalogError; }
    QString notice() const { return m_notice.isEmpty() ? m_account.notice : m_notice; }
    bool noticeError() const { return m_notice.isEmpty() ? m_account.noticeError : true; }
    bool saving() const { return m_account.saving; }
    QVariantList providers() const { return m_account.providers; }
    bool providersLoaded() const { return m_account.providersLoaded; }
    QString providersError() const { return m_account.providersError; }
    QVariantMap login() const { return m_account.login; }

    // A provider switch waits for an explicit model: no fallback model.
    Q_INVOKABLE void chooseProvider(const QString &provider);
    // A model of the listed provider, or of any provider (the model stage).
    Q_INVOKABLE void chooseModel(const QString &id);
    Q_INVOKABLE void choose(const QString &provider, const QString &id);
    Q_INVOKABLE bool chooseThinking(const QString &level);

  signals:
    void changed();
    // Only when the provider rows or the login step change, so an unrelated
    // change never rebuilds a row holding a half-typed answer.
    void providersChanged();
    void modelWanted(); // The model menu opens after a provider switch.
    void chosen(Selection selection); // Validated user choice, never apply()/use().

  private:
    friend class SettingsTest;
    const ModelInfo *find(const QString &provider, const QString &id) const;
    QString resolve(const ModelInfo &model, const QString &wanted) const;
    QString name(const QString &provider) const;
    void resolveThinking();

    Account m_account;
    QStringList m_providerIds; // Providers with an available model.
    Selection m_selected;      // Empty model: none chosen.
    QString m_provider;        // The provider whose models the menu lists.
    QString m_carry;           // Thinking carried across a provider switch.
    QString m_notice;          // A local refusal, shown until the next change.
    bool m_invalid = false;
    bool m_used = false; // OpenGhost's defaults were adopted once.
};
