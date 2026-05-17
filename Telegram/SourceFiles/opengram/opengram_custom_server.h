/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <QtCore/QString>

namespace MTP {
class DcOptions;
} // namespace MTP

namespace Opengram {

// Это мой загрузчик динамических адресов DC для opengram-форка.
// Идея: адреса серверов берутся с https://api.opengra.me/v1/config
// и применяются ДО первого MTProto-коннекта, чтобы при смене IP
// сервера не пересобирать клиент. RSA-ключ уже вшит при сборке
// (mtproto_dc_options.cpp), его я тут не трогаю — только IP:port.
//
// Вызывать строго из Application::run() перед startDomain():
// на этот момент fallbackProductionConfig().dcOptions() уже создан
// (built-in адреса), а MTP::Instance / первый коннект ещё нет.
//
// URL/таймаут/built-in DC берутся из opengram_settings.json рядом с
// данными приложения (создаётся с дефолтами на первом запуске) —
// меняются без пересборки. Если config_url пуст — сеть и кэш
// пропускаются, DC берутся сразу из builtin_dcs того же файла.
// Любой провал НЕ должен мешать старту приложения, функция не кидает.
void ApplyCustomServerConfig(not_null<MTP::DcOptions*> dcOptions);

// Базовый URL апдейтера из opengram_settings.json (поле "update_url").
// Апдейтер сам дописывает к нему "/current". Пусто -> вызывающий код
// берёт свой дефолт (см. Local::readAutoupdatePrefixRaw). Читается с
// диска на каждый вызов — менять можно без пересборки.
[[nodiscard]] QString ConfiguredUpdateUrl();

} // namespace Opengram
