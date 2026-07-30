// SPDX-FileCopyrightText: 2022 Carl Schwan <carl@carlschwan.eu>
// SPDX-License-Identifier: LGPL-2.1-only or LGPL-3.0-only or LicenseRef-KDE-Accepted-LGPL

#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QFontDatabase>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkProxyFactory>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QThread>
#include <QUrl>
#include <QtWebEngineQuick>

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
#include <QWebEngineUrlScheme>

#include "arianna-version.h"
#include "bookdatabase.h"
#include "bookserver.h"
#include "navigation.h"
#include <KConfig>
#include <KConfigGroup>
#include <QUuid>
#include <qlogging.h>

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

        if (scheme == QStringLiteral("qrc") || scheme == QStringLiteral("data") || scheme == QStringLiteral("blob") || scheme == QStringLiteral("about")
            || scheme == QStringLiteral("epub")) {
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

static QUrl bookServerSessionUrl()
{
    return QUrl(QStringLiteral("http://127.0.0.1:45961/session"));
}

static QUrl bookServerReadOnlyBooksUrl()
{
    return QUrl(QStringLiteral("http://127.0.0.1:45961/read-only-books"));
}

static QByteArray waitForNetworkReply(QNetworkReply *reply, bool *ok, int timeoutMs = 1000, bool warnOnFailure = true)
{
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);

    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);

    timeout.start(timeoutMs);
    loop.exec();

    if (!timeout.isActive()) {
        reply->abort();
        if (ok) {
            *ok = false;
        }
        reply->deleteLater();
        return {};
    }

    const bool success = reply->error() == QNetworkReply::NoError;
    const QByteArray body = success ? reply->readAll() : QByteArray();

    if (!success && warnOnFailure) {
        qWarning() << "BookServer session request failed:" << reply->errorString();
    }

    if (ok) {
        *ok = success;
    }

    reply->deleteLater();
    return body;
}

static QString requestBookServerSessionToken(const QString &serverToken, bool warnOnFailure = true)
{
    QNetworkAccessManager manager;
    QNetworkRequest request(bookServerSessionUrl());
    request.setHeader(QNetworkRequest::ContentTypeHeader, QByteArrayLiteral("application/json"));
    request.setRawHeader(QByteArrayLiteral("X-Arianna-Server-Token"), serverToken.toUtf8());

    auto *reply = manager.post(request, QByteArrayLiteral("{}"));
    bool ok = false;
    const QByteArray body = waitForNetworkReply(reply, &ok, 1000, warnOnFailure);
    if (!ok) {
        return {};
    }

    const QJsonDocument document = QJsonDocument::fromJson(body);
    const QString sessionToken = document.object().value(QStringLiteral("sessionToken")).toString();
    if (sessionToken.isEmpty()) {
        qWarning() << "BookServer did not return a session token";
    }

    return sessionToken;
}

static bool unregisterBookServerSessionToken(const QString &sessionToken)
{
    if (sessionToken.isEmpty()) {
        return true;
    }

    QNetworkAccessManager manager;
    QNetworkRequest request(bookServerSessionUrl());
    request.setRawHeader(QByteArrayLiteral("X-Arianna-Session-Token"), sessionToken.toUtf8());

    auto *reply = manager.deleteResource(request);
    bool ok = false;
    waitForNetworkReply(reply, &ok);
    return ok;
}

static QString registerReadOnlyBook(const QString &sessionToken, const QString &fileName)
{
    if (sessionToken.isEmpty() || fileName.isEmpty()) {
        return {};
    }

    QNetworkAccessManager manager;
    QNetworkRequest request(bookServerReadOnlyBooksUrl());
    request.setHeader(QNetworkRequest::ContentTypeHeader, QByteArrayLiteral("application/json"));
    request.setRawHeader(QByteArrayLiteral("X-Arianna-Session-Token"), sessionToken.toUtf8());

    QJsonObject payload;
    payload.insert(QStringLiteral("fileName"), fileName);

    auto *reply = manager.post(request, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    bool ok = false;
    const QByteArray body = waitForNetworkReply(reply, &ok);
    if (!ok) {
        return {};
    }

    const QJsonDocument document = QJsonDocument::fromJson(body);
    const QString identifier = document.object().value(QStringLiteral("identifier")).toString();
    if (identifier.isEmpty()) {
        qWarning() << "BookServer did not return a read-only book identifier";
    }

    return identifier;
}

static bool startDetachedBookServer()
{
    const bool started = QProcess::startDetached(QCoreApplication::applicationFilePath(), {QStringLiteral("--bookserver")});
    if (!started) {
        qWarning() << "Unable to start detached Arianna BookServer";
    }

    return started;
}

static QString ensureBookServerSessionToken(const QString &serverToken)
{
    bool triedStartingServer = false;

    for (int attempt = 0; attempt < 30; ++attempt) {
        const QString sessionToken = requestBookServerSessionToken(serverToken, false);
        if (!sessionToken.isEmpty()) {
            return sessionToken;
        }

        if (!triedStartingServer) {
            triedStartingServer = true;
            startDetachedBookServer();
        }

        QThread::msleep(100);
    }

    return {};
}

static bool hasOptionArgument(int argc, char *argv[], const QStringList &optionNames)
{
    for (int i = 1; i < argc; ++i) {
        const QString argument = QString::fromLocal8Bit(argv[i]);
        if (argument == QStringLiteral("--")) {
            return false;
        }

        if (optionNames.contains(argument)) {
            return true;
        }
    }

    return false;
}

static bool hasBookServerOnlyArgument(int argc, char *argv[])
{
    return hasOptionArgument(argc, argv, {QStringLiteral("--bookserver")});
}

static bool hasEarlyExitArgument(int argc, char *argv[])
{
    return hasOptionArgument(argc,
                             argv,
                             {
                                 QStringLiteral("-h"),
                                 QStringLiteral("--help"),
                                 QStringLiteral("--help-all"),
                                 QStringLiteral("-v"),
                                 QStringLiteral("--version"),
                                 QStringLiteral("--author"),
                                 QStringLiteral("--license"),
                             });
}

static QString localFilePathFromArgument(const QString &argument, const QString &workingDirectory = {})
{
    if (argument.isEmpty()) {
        return {};
    }

    const QUrl url(argument);
    if (url.isLocalFile()) {
        return QFileInfo(url.toLocalFile()).absoluteFilePath();
    }

    QFileInfo fileInfo(argument);
    if (fileInfo.isRelative() && !workingDirectory.isEmpty()) {
        fileInfo = QFileInfo(QDir(workingDirectory), argument);
    }

    return fileInfo.absoluteFilePath();
}

static bool argumentsRequestReadOnly(const QStringList &arguments)
{
    for (int i = 1; i < arguments.size(); ++i) {
        const QString &argument = arguments.at(i);
        if (argument == QStringLiteral("--")) {
            return false;
        }
        if (argument == QStringLiteral("--read-only") || argument == QStringLiteral("--open-read-only")) {
            return true;
        }
    }

    return false;
}

static QString fileArgumentFromActivationArguments(const QStringList &arguments)
{
    bool endOfOptions = false;
    for (int i = 1; i < arguments.size(); ++i) {
        const QString &argument = arguments.at(i);
        if (!endOfOptions && argument == QStringLiteral("--")) {
            endOfOptions = true;
            continue;
        }
        if (!endOfOptions && (argument == QStringLiteral("--read-only") || argument == QStringLiteral("--open-read-only"))) {
            continue;
        }
        if (!endOfOptions && argument.startsWith(QStringLiteral("--"))) {
            continue;
        }

        return argument;
    }

    return {};
}

static void
openBookArgument(Navigation *navigation, const QString &argument, const QString &workingDirectory = {}, bool readOnly = false, const QString &sessionToken = {})
{
    const QString fileName = localFilePathFromArgument(argument, workingDirectory);
    if (fileName.isEmpty()) {
        return;
    }

    if (readOnly) {
        const QString identifier = registerReadOnlyBook(sessionToken, fileName);
        if (identifier.isEmpty()) {
            qWarning() << "Unable to register read-only book with the Arianna BookServer:" << fileName;
            return;
        }

        BookEntry readOnlyEntry;
        const QFileInfo fileInfo(fileName);
        readOnlyEntry.filename = fileName;
        readOnlyEntry.filetitle = fileInfo.fileName();
        readOnlyEntry.title = fileInfo.completeBaseName();
        readOnlyEntry.uniqueIdentifier = identifier;

        Q_EMIT navigation->openBook(fileName, {}, {}, readOnlyEntry, true);
        return;
    }

    const auto entry = BookDatabase::self().loadEntry(fileName);
    if (entry) {
        Q_EMIT navigation->openBook(fileName, entry->locations, entry->currentLocation, *entry, false);
    } else {
        Q_EMIT navigation->openBook(fileName, {}, {}, BookEntry{}, false);
    }
}

int main(int argc, char *argv[])
{
    // QWebEngineUrlScheme scheme("epub");
    // scheme.setSyntax(QWebEngineUrlScheme::Syntax::Host);
    // scheme.setFlags(QWebEngineUrlScheme::SecureScheme
    //               | QWebEngineUrlScheme::LocalScheme
    //               | QWebEngineUrlScheme::LocalAccessAllowed);
    // scheme.setDefaultPort(0);
    // QWebEngineUrlScheme::registerScheme(scheme);

    const bool requestedBookServerOnly = hasBookServerOnlyArgument(argc, argv);
    const bool exitsBeforeOpeningUi = hasEarlyExitArgument(argc, argv);
    if (!requestedBookServerOnly && !exitsBeforeOpeningUi) {
        QtWebEngineQuick::initialize();
    }

    QNetworkProxyFactory::setUseSystemConfiguration(true);

    std::unique_ptr<QCoreApplication> app;
    if (requestedBookServerOnly || exitsBeforeOpeningUi) {
        app = std::make_unique<QCoreApplication>(argc, argv);
    } else {
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
    QCommandLineOption readOnlyOption(QStringList{QStringLiteral("read-only"), QStringLiteral("open-read-only")},
                                      i18n("Open the book without adding it to the library"));
    parser.addOption(bookServerOnlyOption);
    parser.addOption(readOnlyOption);

    about.setupCommandLine(&parser);
    parser.process(*app);
    about.processCommandLine(&parser);

    const bool bookServerOnly = parser.isSet(bookServerOnlyOption);
    const bool readOnly = parser.isSet(readOnlyOption);
    const QString serverToken = persistentServerToken();
    std::signal(SIGINT, handleUnixSignal);
    std::signal(SIGTERM, handleUnixSignal);

    if (bookServerOnly) {
        BookServer bookServer(serverToken, true);
        if (!bookServer.isRunning()) {
            return 1;
        }
        return QCoreApplication::exec();
    }

    const QString sessionToken = ensureBookServerSessionToken(serverToken);
    if (sessionToken.isEmpty()) {
        qWarning() << "Unable to register this reader with the Arianna BookServer";
        return 1;
    }

    // webProfile->installUrlSchemeHandler("epub", new EpubSchemeHandler(webProfile));
    QObject::connect(app.get(), &QCoreApplication::aboutToQuit, app.get(), [sessionToken] {
        unregisterBookServerSessionToken(sessionToken);
    });

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextObject(new KLocalizedQmlContext(&engine));
    engine.rootContext()->setContextProperty(QStringLiteral("applicationFilePath"), QCoreApplication::applicationFilePath());
    engine.rootContext()->setContextProperty(QStringLiteral("serverToken"), serverToken);
    engine.rootContext()->setContextProperty(QStringLiteral("bookServerSessionToken"), sessionToken);
    engine.rootContext()->setContextProperty(QStringLiteral("bookServerPort"), 45961);
    engine.loadFromModule("org.kde.arianna", "Main");
    if (engine.rootObjects().isEmpty()) {
        return -1;
    }
    QGuiApplication::setWindowIcon(QIcon::fromTheme(QStringLiteral("org.kde.arianna")));

    KDBusService service(KDBusService::Multiple);

    auto *navigation = engine.singletonInstance<Navigation *>("org.kde.arianna", "Navigation");
    if (navigation) {
        navigation->setWebEngineSessionToken(sessionToken);
    }
    qDebug() << "Arianna BookServer session registered:" << sessionToken;

    QObject::connect(&service,
                     &KDBusService::activateRequested,
                     &engine,
                     [&engine, navigation, sessionToken](const QStringList &arguments, const QString &workingDirectory) {
                         const auto rootObjects = engine.rootObjects();
                         for (auto obj : rootObjects) {
                             auto view = qobject_cast<QQuickWindow *>(obj);
                             if (view) {
                                 KWindowSystem::updateStartupId(view);
                                 KWindowSystem::activateWindow(view);

                                 const QString fileArgument = fileArgumentFromActivationArguments(arguments);
                                 if (!fileArgument.isEmpty()) {
                                     openBookArgument(navigation, fileArgument, workingDirectory, argumentsRequestReadOnly(arguments), sessionToken);
                                 }
                                 return;
                             }
                         }
                     });

    const QStringList args = parser.positionalArguments();
    if (!args.isEmpty()) {
        openBookArgument(navigation, args[0], {}, readOnly, sessionToken);
    }

    return QCoreApplication::exec();
}
