// Settings → Providers' sign-in form against PiBackend and tests/pi/pi (no
// network or credentials): a cancelled sign-in's late step and end never touch
// the sign-in that replaced it.
#include "backend/pi_backend.h"
#include "settings.h"
#include "window.h"
#include <QtTest>

using namespace openghost;

class SignInTest final : public QObject
{
    Q_OBJECT

  private slots:
    void initTestCase()
    {
        qputenv("PATH", QByteArray(OPENGHOST_FAKE_PI_DIR ":") + qgetenv("PATH"));
        qputenv("FAKE_PI_PROVIDERS", "p");
    }

    void cancelThenSignInAgainKeepsTheNewForm()
    {
        PiBackend backend;
        WindowController window(&backend, QString());
        QTRY_VERIFY(window.ready());
        auto *settings = window.settings();
        window.refreshProviders();
        QTRY_VERIFY(!settings->providers().isEmpty());
        window.login(QStringLiteral("p"), QStringLiteral("oauth"));
        QTRY_VERIFY(!settings->login().value("promptId").toString().isEmpty());
        const auto first = settings->login();
        // Cancel, and at once sign in again: the first flow's stale step and its
        // (cancelled) end arrive after the second began.
        window.cancelLogin(first.value("id").toString());
        window.login(QStringLiteral("p"), QStringLiteral("oauth"));
        const auto second = settings->login().value("id").toString();
        QVERIFY(!second.isEmpty() && second != first.value("id").toString());
        QTRY_VERIFY(!settings->login().value("promptId").toString().isEmpty());
        QTest::qWait(300); // Every late record of the first flow has arrived.
        const auto form = settings->login();
        QCOMPARE(form.value("id").toString(), second);
        QCOMPARE(form.value("type").toString(), QStringLiteral("prompt"));
        QVERIFY(form.value("promptId").toString() != first.value("promptId").toString());
        QVERIFY(form.value("promptId").toString() != QStringLiteral("late"));
        QVERIFY(!window.status().contains(QStringLiteral("cancelled")));
        // Answering finishes this form's own flow.
        window.answerLogin(second, form.value("promptId").toString(), QStringLiteral("123"));
        QTRY_VERIFY(settings->login().isEmpty());
    }
};

QTEST_MAIN(SignInTest)
#include "sign_in.moc"
