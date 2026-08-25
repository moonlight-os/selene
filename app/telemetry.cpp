#include "telemetry.h"

#include <QCoreApplication>
#include <QDate>
#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSettings>
#include <QSysInfo>
#include <QTimer>

#ifndef TELEMETRY_URL
#define TELEMETRY_URL ""
#endif

namespace Telemetry {
namespace {
constexpr auto TelemetryEnabledKey = "telemetryEnabled";
constexpr auto CrashReportingEnabledKey = "crashReportingEnabled";
constexpr auto LastLaunchDayKey = "telemetryLastLaunchDay";
constexpr auto RunningKey = "telemetryRunning";

QJsonObject baseEvent(const char* kind)
{
    return {
        {QStringLiteral("project"), QStringLiteral("selene")},
        {QStringLiteral("version"), QCoreApplication::applicationVersion()},
        {QStringLiteral("os"), QSysInfo::productType().isEmpty() ? QStringLiteral("unknown") : QSysInfo::productType()},
        {QStringLiteral("arch"), QSysInfo::currentCpuArchitecture()},
        {QStringLiteral("kind"), QString::fromLatin1(kind)},
    };
}

void post(QNetworkAccessManager* network, QJsonObject payload)
{
    const QUrl endpoint(QStringLiteral(TELEMETRY_URL));
    if (!endpoint.isValid() || endpoint.scheme() != QStringLiteral("https")) {
        return;
    }
    QNetworkRequest request(endpoint.resolved(QUrl(QStringLiteral("api/v1/events"))));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    request.setTransferTimeout(5000);
#endif
    QNetworkReply* reply = network->post(request, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    QObject::connect(reply, &QNetworkReply::finished, reply, &QObject::deleteLater);
}
}

void start(QCoreApplication* app)
{
    if (QStringLiteral(TELEMETRY_URL).isEmpty()) {
        return;
    }

    QSettings settings;
    const bool telemetryEnabled = settings.value(TelemetryEnabledKey, true).toBool();
    const bool crashReportingEnabled = settings.value(CrashReportingEnabledKey, true).toBool();
    const bool previousRunWasUnclean = settings.value(RunningKey, false).toBool();

    settings.setValue(RunningKey, crashReportingEnabled);
    QObject::connect(app, &QCoreApplication::aboutToQuit, app, [] {
        QSettings cleanSettings;
        cleanSettings.setValue(RunningKey, false);
    });

    auto* network = new QNetworkAccessManager(app);
    QTimer::singleShot(0, app, [network, telemetryEnabled, crashReportingEnabled, previousRunWasUnclean] {
        if (crashReportingEnabled && previousRunWasUnclean) {
            QJsonObject crash = baseEvent("crash");
            crash.insert(QStringLiteral("crash"), QJsonObject{{QStringLiteral("reason"), QStringLiteral("unclean-exit")}});
            post(network, crash);
        }

        QSettings settings;
        const QString today = QDateTime::currentDateTimeUtc().date().toString(Qt::ISODate);
        if (telemetryEnabled && settings.value(LastLaunchDayKey).toString() != today) {
            settings.setValue(LastLaunchDayKey, today);
            post(network, baseEvent("launch"));
        }
    });
}
}
