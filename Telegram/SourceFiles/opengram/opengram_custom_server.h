/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

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
// Поведение при сбое сети: пробую свежий конфиг по HTTP (таймаут
// ~2000мс), при успехе пишу его в кэш в cWorkingDir(). Если сеть
// недоступна — поднимаю последний кэш. Если и кэша нет — ничего не
// делаю, остаются вшитые built-in адреса. Любой провал НЕ должен
// мешать старту приложения, поэтому функция ничего не кидает.
void ApplyCustomServerConfig(not_null<MTP::DcOptions*> dcOptions);

} // namespace Opengram
