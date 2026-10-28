# Нормализация — этап 3

```powershell
$env:PATH = 'C:\msys64\clang64\bin;' + $env:PATH
python scripts/fetch_instrument.py data/metadata/btcusdt-new.json
.\build\clang-release\converter.exe --input data/raw/btcusdt-20261027-linear-x25519.jsonl --instrument data/metadata/btcusdt-new.json --output data/normalized/events-new.bin
.\build\clang-release\converter.exe --verify data/normalized/events-new.bin
python scripts/verify_normalized.py data/normalized/events-new.bin data/raw/btcusdt-20261027-linear-x25519.jsonl data/metadata/btcusdt-new.json
```

Программы не перезаписывают существующие выходные файлы. Для повторного запуска
используйте новое имя. Конвертер сначала проверяет весь захват, затем пишет файл
и выполняет точный round-trip. При ошибке записи может остаться неполный файл;
такой файл нельзя использовать до успешного `--verify`.

REST: `GET /v5/market/instruments-info?category=linear&symbol=BTCUSDT`.
Скрипт fetch использует HTTPS с проверкой сертификатов, ограничивает ответ
1 МиБ и сохраняет его без изменения байтов. URL, UTC получения и SHA-256
выводятся на stdout. Формат: [events-format.md](events-format.md).

Конвертер проверяет schema, категорию/символ, порядок session_start/disconnect,
наличие завершающего collector_stop, accepted, snapshot перед delta, точность
decimal, кратность шагам, границы int64, типы полей, размеры массивов,
дубликаты и сортировку snapshot. Повреждённые сообщения не пропускаются молча.
Ошибка JSON/сессии содержит номер исходной строки. Лимит строки JSONL — 2 МиБ,
чтобы вместить разрешённый collector payload 1 МиБ вместе с envelope.
Control-пакеты не становятся рыночными событиями.

## Проверено на реальных данных 2026-10-27

- Источник: `data/raw/btcusdt-20261027-linear-x25519.jsonl`.
- Metadata: `data/metadata/btcusdt-linear-20261027.json`.
- Основной результат: `data/normalized/events.bin`.
- 10000 рыночных событий + 2 reset = 10002 записи.
- 62023 изменения уровней, price_decimals=1, quantity_decimals=3.
- Размер: 1712640 байт.
- Payload CRC32: `c1fad413`.
- SHA-256 events.bin:
  `58062375a318a015a7032648d978af9edcdc254432a61bead4899612ba2ebe41`.
- SHA-256 metadata:
  `b0f271f2cf3d36acea3ff92a5d2f444b43e5e8c9e648092f3b72bb7ddec63084`.
- GCC Release и Clang Release создали побайтно одинаковые файлы.
- Debug reader обоих компиляторов также прочёл результат успешно.
- Независимый Python audit проверил каждый уровень через Decimal, все поля
  событий, CRC32 через zlib и SHA-256 через hashlib.

Metadata получены после записи потока. Совместимость всех записанных цен и
объёмов с полученными шагами проверена; историческая неизменность спецификации
инструмента этим не доказывается. Для новых захватов сохраняйте metadata перед
сбором данных. Файл metadata содержит также время ответа Bybit.

## Автоматические тесты

17 тестов в каждом из gcc-release, clang-release, gcc-debug, clang-debug.
В их числе: int64 min/max и переполнения, потеря дробной точности, шаг 0.50/0.002,
удаление уровней, пустая BID сторона, reconnect/reset, seq с пропусками,
round-trip, запрет перезаписи и 100000 синтетических обновлений.
Это тесты reader/converter; корректность самого стакана относится к этапу 4.

Эталон `tests/fixtures/events-v1-golden.hex` состоит из 776 байт, независимо
сформированных Python struct/zlib из заранее рассчитанных значений. C++ должен
создать точно такие же байты. Для каждого из 776 возможных мест усечения и
каждого изменённого байта ожидается отказ. Дополнительно проверяются некорректные
длины, поля и уровни с заново вычисленными CRC, чтобы пройти до семантических
проверок reader.

Валидация использует переиспользуемый массив цен вместо аллокаций узлов set
на каждое событие. Все изменения уровней хранятся непрерывно; reader резервирует
память для дескрипторов и уровней по проверенным размерам файла.
Результатов производительности стакана на этом этапе ещё нет.
