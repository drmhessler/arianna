// SPDX-FileCopyrightText: 2022 Carl Schwan <carl@carlschwan.eu>
// SPDX-License-Identifier: LGPL-2.1-only or LGPL-3.0-only or LicenseRef-KDE-Accepted-LGPL

#include <QCommandLineParser>
#include <QFontDatabase>
#include <QNetworkProxy>
#include <QNetworkProxyFactory>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QtWebEngineQuick>

#include <QApplication>

#include <KAboutData>
#include <KConfig>
#include <KCrash>
#include <KDBusService>
#include <KLocalizedContext>
#include <KLocalizedQmlContext>
#include <KLocalizedString>
#include <KWindowConfig>
#include <KWindowSystem>
#include <QStringLiteral>

#include <QCoreApplication>
#include <QTimer>
#include <csignal>
#include <memory>

#include <KConfigGroup>
#include <QCommandLineOption>
#include <QUuid>

#include <QWebEngineProfile>
#include <QWebEngineUrlRequestInfo>
#include <QWebEngineUrlRequestInterceptor>

#include "arianna-version.h"
#include "bookdatabase.h"
#include "bookserver.h"
#include "navigation.h"
#include <KConfig>
#include <KConfigGroup>
#include <QUuid>

class AriannaWebRequestInterceptor : public QWebEngineUrlRequestInterceptor
{
public:
    explicit AriannaWebRequestInterceptor(const QString &sessionToken, QObject *parent = nullptr)
        : QWebEngineUrlRequestInterceptor(parent)
        , m_sessionToken(sessionToken)
    {
    }

    void interceptRequest(QWebEngineUrlRequestInfo &info) override
    {
        const QUrl url = info.requestUrl();
        const QString scheme = url.scheme();

        if (scheme == QStringLiteral("qrc") || scheme == QStringLiteral("data") || scheme == QStringLiteral("blob") || scheme == QStringLiteral("about")) {
            return;
        }

        const bool localBookServer = (scheme == QStringLiteral("http") || scheme == QStringLiteral("https"))
            && (url.host() == QStringLiteral("127.0.0.1") || url.host() == QStringLiteral("localhost")) && (url.port() == 45961 || url.port() == 45962);

        if (localBookServer) {
            info.setHttpHeader(QByteArrayLiteral("X-Arianna-Session-Token"), m_sessionToken.toUtf8());
            return;
        }

        if (scheme == QStringLiteral("https") && url.host() == QStringLiteral("cdn.jsdelivr.net")) {
            return;
        }

        qWarning() << "Blocked WebEngine request:" << url << "initiator:" << info.initiator();
        info.block(true);
    }

private:
    QString m_sessionToken;
};

static void handleUnixSignal(int)
{
    QTimer::singleShot(0, QCoreApplication::instance(), [] {
        QCoreApplication::quit();
    });
}

static QString persistentServerToken()
{
    KConfig config(QStringLiteral("ariannarc"));
    KConfigGroup group(&config, QStringLiteral("BookServer"));

    QString token = group.readEntry(QStringLiteral("ServerToken"), QString());

    if (token.isEmpty()) {
        token = QUuid::createUuid().toString(QUuid::WithoutBraces);
        group.writeEntry(QStringLiteral("ServerToken"), token);
        group.sync();
    }

    return token;
}

static bool hasBookServerOnlyArgument(int argc, char *argv[])
{
    for (int i = 1; i < argc; ++i) {
        if (QString::fromLocal8Bit(argv[i]) == QStringLiteral("--bookserver")) {
            return true;
        }
    }

    return false;
}

int main(int argc, char *argv[])
{
    QNetworkProxyFactory::setUseSystemConfiguration(true);
    const bool requestedBookServerOnly = hasBookServerOnlyArgument(argc, argv);

    std::unique_ptr<QCoreApplication> app;
    if (requestedBookServerOnly) {
        app = std::make_unique<QCoreApplication>(argc, argv);
    } else {
        QtWebEngineQuick::initialize();
        auto guiApp = std::make_unique<QApplication>(argc, argv);

        // Default to org.kde.desktop style unless the user forces another style
        if (qEnvironmentVariableIsEmpty("QT_QUICK_CONTROLS_STYLE")) {
            QQuickStyle::setStyle(QStringLiteral("org.kde.desktop"));
        }

#ifdef Q_OS_WINDOWS
        if (AttachConsole(ATTACH_PARENT_PROCESS)) {
            freopen("CONOUT$", "w", stdout);
            freopen("CONOUT$", "w", stderr);
        }

        QApplication::setStyle(QStringLiteral("breeze"));
        auto font = guiApp->font();
        font.setPointSize(10);
        guiApp->setFont(font);
#endif
        app = std::move(guiApp);
    }

    KLocalizedString::setApplicationDomain(QByteArrayLiteral("arianna"));

    KAboutData about(QStringLiteral("arianna"),
                     i18n("Arianna"),
                     QStringLiteral(ARIANNA_VERSION_STRING),
                     i18n("EPub Reader"),
                     KAboutLicense::GPL_V3,
                     i18n("2022 Niccolò Venerandi <niccolo@venerandi.com>"));
    about.addAuthor(i18n("Niccolò Venerandi"), i18n("Maintainer"), QStringLiteral("niccolo@venerandi.com"));
    about.addAuthor(i18n("Carl Schwan"), i18n("Maintainer"), QStringLiteral("carl@carlschwan.eu"));
    about.setTranslator(i18nc("NAME OF TRANSLATORS", "Your names"), i18nc("EMAIL OF TRANSLATORS", "Your emails"));
    about.setOrganizationDomain("kde.org");
    about.setBugAddress("https://bugs.kde.org/describecomponents.cgi?product=arianna");

    KAboutData::setApplicationData(about);
    KCrash::initialize();

    QCommandLineParser parser;
    parser.setApplicationDescription(i18n("Epub reader"));
    parser.addPositionalArgument(QStringLiteral("file"), i18n("Epub file to open"));
    QCommandLineOption bookServerOnlyOption(QStringLiteral("bookserver"), i18n("Start only the local book server without opening the reader UI"));
    parser.addOption(bookServerOnlyOption);

    about.setupCommandLine(&parser);
    parser.process(*app);
    about.processCommandLine(&parser);

    const bool bookServerOnly = parser.isSet(bookServerOnlyOption);
    const QString serverToken = persistentServerToken();
    std::signal(SIGINT, handleUnixSignal);
    std::signal(SIGTERM, handleUnixSignal);

    if (bookServerOnly) {
        BookServer bookServer(serverToken);
        if (!bookServer.isRunning()) {
            return 1;
        }

        qWarning() << "Arianna BookServer running without UI";

        return QCoreApplication::exec();
    }
    auto *webProfile = QWebEngineProfile::defaultProfile();
    webProfile->setHttpCacheType(QWebEngineProfile::NoCache);
    webProfile->setPersistentCookiesPolicy(QWebEngineProfile::NoPersistentCookies);
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextObject(new KLocalizedQmlContext(&engine));
    engine.rootContext()->setContextProperty(QStringLiteral("applicationFilePath"), QCoreApplication::applicationFilePath());
    engine.rootContext()->setContextProperty(QStringLiteral("serverToken"), serverToken);
    engine.rootContext()->setContextProperty(QStringLiteral("bookServerPort"), 45961);
    engine.loadFromModule("org.kde.arianna", "Main");
    if (engine.rootObjects().isEmpty()) {
        return -1;
    }
    QGuiApplication::setWindowIcon(QIcon::fromTheme(QStringLiteral("org.kde.arianna")));

    KDBusService service(KDBusService::Multiple);

    BookServer bookServer(serverToken);
    auto navigation = engine.singletonInstance<Navigation *>("org.kde.arianna", "Navigation");
    QString sessionToken = serverToken;
    if (bookServer.isRunning()) {
        sessionToken = navigation->bookServerToken();
        bookServer.addSessionToken(sessionToken);
        qDebug() << "Arianna BookServer running with UI, session token:" << sessionToken;
    } else {
        qDebug() << "Using already running Arianna BookServer";
    }
    webProfile->setUrlRequestInterceptor(new AriannaWebRequestInterceptor(sessionToken, webProfile));

    QObject::connect(&service,
                     &KDBusService::activateRequested,
                     &engine,
                     [&engine, navigation](const QStringList &arguments, const QString & /*workingDirectory*/) {
                         const auto rootObjects = engine.rootObjects();
                         for (auto obj : rootObjects) {
                             auto view = qobject_cast<QQuickWindow *>(obj);
                             if (view) {
                                 KWindowSystem::updateStartupId(view);
                                 KWindowSystem::activateWindow(view);

                                 if (arguments.count() > 1) {
                                     const auto entry = BookDatabase::self().loadEntry(arguments[1]);
                                     if (entry) {
                                         Q_EMIT navigation->openBook(arguments[1], entry->locations, entry->currentLocation, *entry);
                                     } else {
                                         Q_EMIT navigation->openBook(arguments[1], {}, {}, BookEntry{});
                                     }
                                 }
                                 return;
                             }
                         }
                     });

    const QStringList args = parser.positionalArguments();
    if (!args.isEmpty()) {
        const auto entry = BookDatabase::self().loadEntry(args[0]);
        if (entry) {
            Q_EMIT navigation->openBook(args[0], entry->locations, entry->currentLocation, *entry);
        } else {
            Q_EMIT navigation->openBook(args[0], {}, {}, BookEntry{});
        }
    }

    return QCoreApplication::exec();
}
