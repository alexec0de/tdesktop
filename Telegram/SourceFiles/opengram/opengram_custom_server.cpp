/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "opengram/opengram_custom_server.h"

#include "mtproto/mtproto_dc_options.h"
#include "logs.h"
#include "settings.h" // cWorkingDir()

#include <QtCore/QByteArray>
#include <QtCore/QDir>
#include <QtCore/QEventLoop>
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonParseError>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>

namespace Opengram {
namespace {

// Эндпойнт конфигурации и таймаут синхронного запроса.
// Держу таймаут небольшим: это блокирует старт приложения, поэтому
// 2 секунды — потолок, дальше падаем в кэш / built-in.
constexpr auto kConfigUrl = "https://api.opengra.me/v1/config";
constexpr auto kRequestTimeoutMs = 2000;

// Имя файла кэша в рабочей директории (cWorkingDir()). Сюда
// складываю последний успешно скачанный JSON — на случай оффлайна.
constexpr auto kCacheFileName = "opengram_dc_config.json";

[[nodiscard]] QString CacheFilePath() {
	// cWorkingDir() уже оканчивается на '/', но подстрахуюсь через QDir.
	return QDir(cWorkingDir()).filePath(kCacheFileName);
}

// Синхронно тяну тело ответа по HTTPS с жёстким таймаутом.
// Возвращаю пустой QByteArray при любой ошибке/таймауте.
[[nodiscard]] QByteArray FetchSync(const QString &url) {
	auto manager = QNetworkAccessManager();

	auto request = QNetworkRequest(QUrl(url));
	request.setAttribute(
		QNetworkRequest::RedirectPolicyAttribute,
		QNetworkRequest::NoLessSafeRedirectPolicy);

	auto loop = QEventLoop();
	auto reply = std::unique_ptr<QNetworkReply>(manager.get(request));
	if (!reply) {
		return QByteArray();
	}

	// Сам таймаут: если за kRequestTimeoutMs ответа нет — рву запрос
	// и выходим из локального event-loop'а.
	auto timer = QTimer();
	timer.setSingleShot(true);
	auto timedOut = false;
	QObject::connect(&timer, &QTimer::timeout, [&] {
		timedOut = true;
		reply->abort();
		loop.quit();
	});
	QObject::connect(
		reply.get(),
		&QNetworkReply::finished,
		[&] { loop.quit(); });

	timer.start(kRequestTimeoutMs);
	loop.exec();
	timer.stop();

	if (timedOut) {
		LOG(("Opengram: config request timed out"));
		return QByteArray();
	} else if (reply->error() != QNetworkReply::NoError) {
		LOG(("Opengram: config request failed: %1"
			).arg(reply->errorString()));
		return QByteArray();
	}
	return reply->readAll();
}

// Разбираю JSON формата:
//   {"version":int,"datacenters":[
//      {"id":int,"addresses":[{"ip":str,"port":int,"ipv6":bool}]}]}
// Беру по одному адресу на DC — первый не-ipv6. public_keys
// игнорирую (ключ уже вшит при сборке, его не меняю).
// Результат — строки формата ".tdesktop-endpoints": "dcId host port",
// ровно то, что умеет парсить DcOptions::loadFromFile().
[[nodiscard]] QStringList ParseEndpoints(const QByteArray &json) {
	auto error = QJsonParseError();
	const auto document = QJsonDocument::fromJson(json, &error);
	if (error.error != QJsonParseError::NoError || !document.isObject()) {
		LOG(("Opengram: config parse error: %1"
			).arg(error.errorString()));
		return QStringList();
	}
	const auto root = document.object();
	const auto datacenters = root.value(u"datacenters"_q).toArray();
	if (datacenters.isEmpty()) {
		LOG(("Opengram: config has no datacenters"));
		return QStringList();
	}

	auto result = QStringList();
	for (const auto &dcValue : datacenters) {
		const auto dc = dcValue.toObject();
		const auto id = dc.value(u"id"_q).toInt();
		if (id <= 0) {
			continue;
		}
		const auto addresses = dc.value(u"addresses"_q).toArray();
		for (const auto &addressValue : addresses) {
			const auto address = addressValue.toObject();
			if (address.value(u"ipv6"_q).toBool()) {
				continue; // Беру только IPv4-адреса.
			}
			const auto ip = address.value(u"ip"_q).toString();
			const auto port = address.value(u"port"_q).toInt();
			if (ip.isEmpty() || port <= 0) {
				continue;
			}
			// Формат строки совпадает с .tdesktop-endpoints:
			// "dcId host port" — loadFromFile() это и ждёт.
			result.push_back(
				QString::number(id) + ' ' + ip + ' '
				+ QString::number(port));
			break; // Один (первый годный) адрес на DC — достаточно.
		}
	}
	if (result.isEmpty()) {
		LOG(("Opengram: config produced no usable endpoints"));
	}
	return result;
}

// Записываю сырой JSON в кэш рабочей директории — на будущий оффлайн.
void WriteCache(const QByteArray &json) {
	auto file = QFile(CacheFilePath());
	if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		file.write(json);
		file.close();
	} else {
		LOG(("Opengram: could not write config cache to '%1'"
			).arg(CacheFilePath()));
	}
}

// Поднимаю последний удачно скачанный JSON из кэша.
[[nodiscard]] QByteArray ReadCache() {
	auto file = QFile(CacheFilePath());
	if (!file.open(QIODevice::ReadOnly)) {
		return QByteArray();
	}
	const auto data = file.readAll();
	file.close();
	return data;
}

// Применяю список эндпойнтов в DcOptions самым НЕинвазивным путём:
// пишу их во временный файл формата "dcId host port" и скармливаю
// штатному DcOptions::loadFromFile(). loadFromFile() сам делает
// setFromList() и выставляет _immutable = true. Это критично:
// _immutable блокирует перезапись адресов серверным help.getConfig
// (processFromList делает ранний выход при _immutable) и не даёт
// сохранять оверрайды в кэш аккаунта. Сам класс DcOptions я не
// меняю, приватный _immutable не трогаю руками.
[[nodiscard]] bool ApplyEndpoints(
		not_null<MTP::DcOptions*> dcOptions,
		const QStringList &endpoints) {
	if (endpoints.isEmpty()) {
		return false;
	}

	// Временный файл рядом с кэшем, в рабочей директории.
	const auto tmpPath = QDir(cWorkingDir()).filePath(
		u"opengram_dc_endpoints.tmp"_q);
	{
		auto file = QFile(tmpPath);
		if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
			LOG(("Opengram: could not write endpoints file '%1'"
				).arg(tmpPath));
			return false;
		}
		const auto payload = (endpoints.join('\n') + '\n').toUtf8();
		file.write(payload);
		file.close();
	}

	const auto ok = dcOptions->loadFromFile(tmpPath);

	// Временный файл больше не нужен — loadFromFile() уже всё считал.
	QFile::remove(tmpPath);

	if (ok) {
		LOG(("Opengram: applied %1 custom DC endpoint(s)"
			).arg(endpoints.size()));
	} else {
		LOG(("Opengram: loadFromFile() rejected custom endpoints"));
	}
	return ok;
}

} // namespace

void ApplyCustomServerConfig(not_null<MTP::DcOptions*> dcOptions) {
	// Шаг 1: пробую свежий конфиг по сети. Успех -> обновляю кэш.
	auto json = FetchSync(QString::fromUtf8(kConfigUrl));
	if (!json.isEmpty()) {
		const auto endpoints = ParseEndpoints(json);
		if (!endpoints.isEmpty() && ApplyEndpoints(dcOptions, endpoints)) {
			WriteCache(json);
			return;
		}
	}

	// Шаг 2: сеть не помогла — пробую последний кэш (оффлайн-сценарий).
	json = ReadCache();
	if (!json.isEmpty()) {
		const auto endpoints = ParseEndpoints(json);
		if (!endpoints.isEmpty() && ApplyEndpoints(dcOptions, endpoints)) {
			LOG(("Opengram: applied DC config from cache"));
			return;
		}
	}

	// Шаг 3: ни сети, ни кэша — молча остаёмся на built-in адресах
	// (вшитый kBuiltInDcs[] в mtproto_dc_options.cpp). Это штатный
	// fallback, старт приложения продолжается без помех.
	LOG(("Opengram: using built-in DC addresses (no custom config)"));
}

} // namespace Opengram
