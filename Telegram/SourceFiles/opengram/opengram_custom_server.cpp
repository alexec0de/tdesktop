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

// Дефолты. Их можно переопределить на лету, без пересборки, через
// локальный файл opengram_settings.json в рабочей директории — см.
// LoadSettings(). Сюда же кладётся дефолтный файл на первом запуске,
// чтобы было что и где править.
constexpr auto kDefaultConfigUrl = "https://api.opengra.me/v1/config";
constexpr auto kDefaultTimeoutMs = 6000;
constexpr auto kDefaultDcIp = "195.34.237.232";
constexpr auto kDefaultDcPort = 4430;
// Базовый URL апдейтера (совпадает с дефолтом в localstorage.cpp,
// чтобы поведение без правки файла не менялось). "/current" — за
// апдейтером.
constexpr auto kDefaultUpdateUrl = "https://opengra.me";

// Имя файла кэша в рабочей директории (cWorkingDir()). Сюда
// складываю последний успешно скачанный JSON — на случай оффлайна.
constexpr auto kCacheFileName = "opengram_dc_config.json";
// Файл с настройками клиента (URL/таймаут/built-in DC) рядом с кэшем.
constexpr auto kSettingsFileName = "opengram_settings.json";

[[nodiscard]] QString CacheFilePath() {
	// cWorkingDir() уже оканчивается на '/', но подстрахуюсь через QDir.
	return QDir(cWorkingDir()).filePath(kCacheFileName);
}

[[nodiscard]] QString SettingsFilePath() {
	return QDir(cWorkingDir()).filePath(QString::fromUtf8(kSettingsFileName));
}

// Настройки клиента, читаемые на старте из opengram_settings.json.
// Любое поле необязательно — отсутствует/кривое → берётся дефолт.
struct Settings {
	QString configUrl = QString::fromUtf8(kDefaultConfigUrl);
	int timeoutMs = kDefaultTimeoutMs;
	// Строки формата "dcId ip port" — то, что ест ApplyEndpoints().
	// Пусто → крайний fallback на компилированные kBuiltInDcs[].
	QStringList builtinEndpoints;
	// Базовый URL апдейтера ("/current" дописывает сам апдейтер).
	// Пусто → берётся дефолт в Local::readAutoupdatePrefixRaw.
	QString updateUrl;
};

[[nodiscard]] Settings LoadSettings() {
	auto result = Settings();

	auto file = QFile(SettingsFilePath());
	if (!file.open(QIODevice::ReadOnly)) {
		return result; // Файла нет — чистые дефолты.
	}
	const auto raw = file.readAll();
	file.close();

	auto error = QJsonParseError();
	const auto document = QJsonDocument::fromJson(raw, &error);
	if (error.error != QJsonParseError::NoError || !document.isObject()) {
		LOG(("Opengram: settings parse error: %1, using defaults"
			).arg(error.errorString()));
		return result;
	}
	const auto root = document.object();

	// config_url: если ключа нет — дефолт; если есть, в т.ч. ЯВНО
	// пустой ("") — берём как есть. Пустой => сеть/кэш пропускаем
	// и DC идут сразу из builtin_dcs (см. ApplyCustomServerConfig).
	if (root.contains(u"config_url"_q)) {
		result.configUrl = root.value(u"config_url"_q).toString().trimmed();
	}
	const auto timeout = root.value(u"request_timeout_ms"_q).toInt();
	if (timeout > 0) {
		result.timeoutMs = timeout;
	}
	result.updateUrl = root.value(u"update_url"_q).toString().trimmed();
	for (const auto &dcValue : root.value(u"builtin_dcs"_q).toArray()) {
		const auto dc = dcValue.toObject();
		const auto id = dc.value(u"id"_q).toInt();
		const auto ip = dc.value(u"ip"_q).toString();
		const auto port = dc.value(u"port"_q).toInt();
		if (id > 0 && !ip.isEmpty() && port > 0) {
			result.builtinEndpoints.push_back(
				QString::number(id) + ' ' + ip + ' '
				+ QString::number(port));
		}
	}
	return result;
}

// На первом запуске кладу рядом дефолтный opengram_settings.json,
// чтобы пользователь видел где и что менять ("чтобы было чётко").
void EnsureDefaultSettingsFile() {
	const auto path = SettingsFilePath();
	if (QFile::exists(path)) {
		return;
	}
	auto root = QJsonObject();
	root.insert(u"config_url"_q, QString::fromUtf8(kDefaultConfigUrl));
	root.insert(u"request_timeout_ms"_q, kDefaultTimeoutMs);
	root.insert(u"update_url"_q, QString::fromUtf8(kDefaultUpdateUrl));
	auto dcs = QJsonArray();
	for (auto id = 1; id <= 5; ++id) {
		auto dc = QJsonObject();
		dc.insert(u"id"_q, id);
		dc.insert(u"ip"_q, QString::fromUtf8(kDefaultDcIp));
		dc.insert(u"port"_q, kDefaultDcPort);
		dcs.push_back(dc);
	}
	root.insert(u"builtin_dcs"_q, dcs);

	auto file = QFile(path);
	if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
		file.close();
	} else {
		LOG(("Opengram: could not write default settings to '%1'"
			).arg(path));
	}
}

// Синхронно тяну тело ответа по HTTPS с жёстким таймаутом.
// Возвращаю пустой QByteArray при любой ошибке/таймауте.
[[nodiscard]] QByteArray FetchSync(const QString &url, int timeoutMs) {
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

	// Сам таймаут: если за timeoutMs ответа нет — рву запрос
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

	timer.start(timeoutMs);
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
	// На первом запуске создаю opengram_settings.json с дефолтами —
	// дальше URL/таймаут/built-in DC меняются в нём без пересборки.
	EnsureDefaultSettingsFile();
	const auto settings = LoadSettings();

	// config_url пуст -> сеть и кэш НЕ трогаем, DC берём сразу из
	// локального builtin_dcs (полностью офлайн/ручной режим).
	if (settings.configUrl.isEmpty()) {
		if (!settings.builtinEndpoints.isEmpty()
				&& ApplyEndpoints(dcOptions, settings.builtinEndpoints)) {
			LOG(("Opengram: config_url empty, applied builtin_dcs "
				"from opengram_settings.json"));
			return;
		}
		LOG(("Opengram: config_url empty and no builtin_dcs, "
			"using compiled built-in DC addresses"));
		return;
	}

	// Шаг 1: пробую свежий конфиг по сети. Успех -> обновляю кэш.
	auto json = FetchSync(settings.configUrl, settings.timeoutMs);
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

	// Шаг 3: ни сети, ни кэша — built-in DC из opengram_settings.json
	// (тоже редактируется на лету, без пересборки).
	if (!settings.builtinEndpoints.isEmpty()
			&& ApplyEndpoints(dcOptions, settings.builtinEndpoints)) {
		LOG(("Opengram: applied built-in DCs from opengram_settings.json"));
		return;
	}

	// Шаг 4: совсем ничего — остаёмся на компилированных kBuiltInDcs[]
	// (mtproto_dc_options.cpp). Крайний fallback, старт не блокируется.
	LOG(("Opengram: using compiled built-in DC addresses"));
}

QString ConfiguredUpdateUrl() {
	// Читаю файл заново (без кэша): апдейтер дёргает префикс не на
	// старте, к этому моменту opengram_settings.json уже на месте,
	// а правка подхватится без пересборки.
	return LoadSettings().updateUrl;
}

} // namespace Opengram
