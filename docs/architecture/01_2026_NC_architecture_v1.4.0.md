# NeuroComm Product Architecture – v1.4.0
*(NASP-oriented, clean-slate redesign)*

## 1. Цели документа

Этот документ фиксирует **целевую архитектуру продукта NeuroComm**, основанную на:
- фактическом анализе текущей demo-реализации (Keln, 3 MCU),
- выявленных архитектурных проблемах,
- продуктовых требованиях,
- будущем переходе на нейроморфный чип (NASP).

Документ является **базой для новой реализации с нуля**.  
Текущий код Keln используется **только как база знаний**, не как кодовая основа.

---

## 2. Общая концепция

### Целевой продукт
В финальном продукте используется **2 кристалла** + **1 service MCU** для DemoBox:

1. **ADC + MCU1 → digital twin NASP (нейроморфный чип)**
   - VAD + buffer
   - VE
   - Минимальная обработка аудио (LowFreq filter + clip control)
   - Жёсткие real-time контракты

2. **MCU2 + DAC → Product SoC** (product level code)
   - SR + buffer
   - KWS + buffer
   - State machine
   - Communication block

3. **MCU3 → UI / Display / Radio control** (non product level code)
   - Максимально лёгкий и заменяемый

Bridge-плата и сервисный MCU используются **только в demo / dev среде**.

---

## 3. Ответственность MCU1 (NASP digital twin)

### 3.1 Функции (ТОЛЬКО)

- Voice Activity Detection (VAD)
- Voice Enhancement (VE)
- Минимальная маршрутизация аудио
- Выдача статусов

❌ **Запрещено на MCU1:**
- SR
- KWS
- Длинные аудио-буферы
- Продуктовая логика
- State machine
- UI / BLE / Radio logic

> MCU1 = чистый аудио-алгоритмический accelerator

---

### 3.2 Аудио входы (ADC)

MCU1 получает **2 канала ADC (16 kHz / 16 bit)**:

| Канал | Источник | Назначение |
|-------|----------|------------|
| In1   | MIC (Headset mic) | Всегда идёт на VAD |
| In2   | Radio SPK         | RX аудио, VE RX |

---

### 3.3 Логика маршрутизации внутри MCU1

#### MIC (In1)
- Всегда подаётся на **VAD**
- Минимальная обработка аудио (LowFreq filter (≪100 Hz) + clip control)
- При TX → идёт на VE
- При RX → не используется

#### SPK / RX (In2)
- Входной gain control: если уровень ниже заданного (−50 dB), в линии ноль, чтобы не передавать фоновый шум
- Всегда подаётся на VE
- VE может быть enabled / disabled
- Используется только в RX

---

### 3.4 Контракт VE (КРИТИЧНО)

**VE mode = Disabled ⇒ VE output = BYPASS**

- Выход VE = точная копия входа
- Никакого mute
- Никакого "last buffer"
- Никакого undefined поведения

Это обязательный контракт для:
- стабильной работы KWS
- одновременных RX/TX режимов
- будущего VE_always_on режима

---

### 3.5 Audio Output MCU1

MCU1 **является аудио источником по умолчанию** для MCU2.

| Режим | Audio out |
|-------|-----------|
| RX (all) | SPK → VE → UART out to MCU2 |
| RX (all) | MIC → VAD → UART out to MCU2 |
| TX (PTT ON) | MIC → VE → UART out to MCU2 |
| AI-VOX (TX через VAD → PTT) | MIC → VAD → VE → UART out to MCU2 |

При `PTT=OFF` в режиме TX:
- выход **не активен**
- не передаётся даже тишина

---

## 4. Ответственность MCU2 (Product SoC)

### 4.1 Функции

- State machine продукта
- SR (Speaker Recognition)
- KWS (Keyword Spotting)
- Управление PTT по флагу VAD
- Управление VE режимами
- **Beep-подтверждение** при смене состояния (короткий тональный сигнал в наушник)
- **Передача статуса на MCU3** при каждом переходе state machine (для отображения на дисплее)
- Log record

### 4.2 Beep Feedback (ОБЯЗАТЕЛЬНО)

При каждом переходе state machine MCU2 **обязан**:

1. **Сгенерировать короткий beep** в audio out (наушник пользователя) — подтверждение перехода
2. **Отправить статусное сообщение на MCU3** — для обновления дисплея

Beep генерируется локально на MCU2 (синтез тона, не из буфера). Beep не должен конфликтовать с аудио потоком — допускается короткое микширование или вставка в паузу.

Таблица событий с beep/status:

| Событие | Beep | Статус → MCU3 |
|---------|------|---------------|
| KWS: стартовая команда (cmd 1–3) обнаружена | ✅ | `CMD_DETECTED: cmd_id` |
| SR = true (авторизация пройдена) | ✅ | `SR_CONFIRMED` |
| SR = false (авторизация отклонена) | ✅ (другой тон) | `SR_REJECTED` |
| Переход в AI-VOX режим (PTT ON) | ✅ | `SESSION_ACTIVE` |
| KWS: disconnect (cmd 4) | ✅ | `SESSION_CLOSED` |
| Session Timeout (30 сек тишины) | ✅ (timeout tone) | `SESSION_TIMEOUT` |
| VAD ON (начало речи в сессии) | — | `VAD_ON` |
| VAD OFF (пауза в сессии) | — | `VAD_OFF` |

> **Примечание:** Beep подаётся только на ключевые переходы state machine, не на каждый VAD cycling. VAD ON/OFF отправляются на MCU3 для отображения, но без звукового подтверждения.

---

### 4.3 Буферизация

- **Все длинные буферы находятся ТОЛЬКО в MCU2**
- MCU1 хранит максимум:
  - chunk для VAD (кольцевой pre-roll буфер: 10 чанков × 16 мс = 160 мс, 5120 байт)

---

## 5. Ответственность MCU3 (Demo box)

### 5.1 Функции

- UI (джойстик + дисплей)
- Display
- USB

### 5.2 UI — навигация (4-позиционный джойстик)

MCU3 управляет пользовательским интерфейсом через **4-позиционный джойстик** (вверх / вниз / вправо / влево).

Логика навигации:

| Действие | Функция |
|----------|---------|
| **Вправо** | Вход в меню / переход на следующий уровень (детали режима) |
| **Влево** | Назад — сохранить текущее состояние и вернуться на уровень выше |
| **Вверх** | Перемещение по строкам меню вверх |
| **Вниз** | Перемещение по строкам меню вниз |

Структура меню (верхний уровень — выбор режима):

| Строка | Режим |
|--------|-------|
| 1 | AI-VOX |
| 2 | AI-VOX PRO |
| 3 | AI-VOX PRO + SR |
| 4 | VE |

При навигации **вправо** — переход на уровень деталей выбранного режима (параметры, настройки). При **влево** — запись выбранного состояния и возврат на верхний уровень.

Все переходы и текущая позиция в меню отображаются на дисплее в реальном времени.

> **Примечание:** Детальная таблица переходов, подуровней и доступных настроек каждого режима будет описана в отдельном документе (UI Navigation Spec).

### 5.3 Отображение статуса (Display)

MCU3 получает статусные сообщения от MCU2 и отображает **актуальное состояние системы** на дисплее:

| Статус от MCU2 | Отображение на дисплее |
|----------------|------------------------|
| `CMD_DETECTED: cmd_id` | Команда обнаружена (номер команды) |
| `SR_CONFIRMED` | Говорящий подтверждён ✅ |
| `SR_REJECTED` | Говорящий не подтверждён ❌ |
| `SESSION_ACTIVE` | Сессия активна / TX режим |
| `SESSION_CLOSED` | Сессия завершена (disconnect) / Ожидание |
| `SESSION_TIMEOUT` | Сессия завершена (timeout 30 сек) / Ожидание |
| `VAD_ON` | Речь (голос активен) |
| `VAD_OFF` | Пауза (голос неактивен) |

Дисплей работает в двух контекстах:
1. **Меню** — навигация по режимам и настройкам (управление джойстиком)
2. **Рабочий экран** — отображение текущего режима, статуса state machine и событий в реальном времени

---

## 6. KWS Audio Contract (важно)

MCU2 **должен поддерживать 2 режима KWS входа**:

1. **MIC_RAW**
   - Сырой MIC
   - Максимальная стабильность

2. **MIC_VE**
   - MIC после VE
   - Используется в режиме `VE_always_on`

Правила:
- Если VE включён раздельно (RX / TX) → KWS получает **bypass**
- Если выбран `VE_always_on` → KWS получает VE stream

---

## 6.5 SR Pipeline — Speaker Recognition (архитектура верификации)

> **Источник:** Taiwan прошивка (vadvesr-taiwan), SvcCore/algorithms/sr_algorithm.hpp, voiceidencoder.hpp, voiceidpredictor.hpp. SR pipeline проверен и работает в связке VAD+VE+SR на одном STM32.

### 6.5.1 Общая схема

```
Audio (KWS буфер) → MelSpec (TFLite) → VoiceID Encoder (TFLite) → Embedding (128 float) → Predictor (cosine + Weibull) → SR=true/false
```

В NeuroComm SR работает **однократно** — на буфер стартовой команды, выделенный Sensory endpoint detection. Это тот же аудио-сегмент, на котором KWS нашёл команду 1–3. SR не работает на речевой поток (экономия ресурсов MCU2).

### 6.5.2 Входные данные для SR

| Параметр | Значение | Примечание |
|----------|----------|------------|
| Sample rate | 16 kHz | Единый для всей системы |
| UART формат | int16 LE mono | Как приходит от MCU1 (AUDIO_TX_FRAME) |
| Внутренний формат MCU2 | float32 | MCU2 конвертирует int16 → float при приёме |
| **Encoder input size** | **8000 сэмплов = 500 мс** | Фиксированный размер входа VoiceID модели (уточнено с SR разработчиком) |
| Window size | 256 сэмплов = 16 мс | Совпадает с VAD chunk |
| Минимальная длительность | 250 мс (4000 сэмплов) | Короче → SR=false (слишком мало данных) |

### 6.5.3 Обработка KWS буфера для SR

KWS буфер (аудио-сегмент команды из Sensory endpoint detection) может быть короче или длиннее одного SR пакета (8000 сэмплов = 500 мс).

**Три сценария:**

**Сценарий A: Буфер < 250 мс (< 4000 сэмплов)**
- SR skip → **SR=false** (слишком короткий фрагмент, недостаточно для надёжного embedding)

**Сценарий B: Буфер 250–500 мс (4000–8000 сэмплов) — типичный случай**
- Размножение (looping): циклическое копирование до заполнения 8000 сэмплов
- Один вызов encoder → один embedding → одно решение SR

```
KWS буфер: [cmd_audio, len=N сэмплов]  (N < 8000)

Размножение:
┌──────────────┬──────────────┬───────┐
│ cmd_audio    │ cmd_audio    │ cmd...│
│ [0..N-1]     │ [0..N-1]     │ [0..] │
└──────────────┴──────────────┴───────┘
←────────── 8000 сэмплов (500 мс) ────→
```

Алгоритм looping:
1. `voice_buffer[0..voice_buffer_size-1]` — исходный KWS буфер
2. `copy_index = 0`
3. Подаём `voice_buffer[copy_index..copy_index+256]` в encoder по 256 сэмплов
4. `copy_index += 256`; если `copy_index >= voice_buffer_size` → `copy_index = 0` (wrap)
5. Продолжаем пока encoder не заполнен (`isEncodingsReady()`)

**Сценарий C: Буфер > 500 мс (> 8000 сэмплов)**
- Обрабатываем **два пакета** последовательно:
  - Пакет 1: первые 8000 сэмплов → encoder → embedding_1
  - Пакет 2: оставшиеся сэмплы (< 8000) → размножение до 8000 → encoder → embedding_2
- Финальное решение: по **обоим embedding'ам** (оба должны быть known, или берём лучший distance — стратегия определяется тестами)

```
KWS буфер: [cmd_audio, len=M сэмплов]  (M > 8000)

┌──────────────────┬────────────────────────────────┐
│     Пакет 1      │           Пакет 2              │
│ [0..7999]        │ [8000..M-1] + looping до 8000  │
│ → embedding_1    │ → embedding_2                  │
└──────────────────┴────────────────────────────────┘
```

> **Примечание:** KWS команда ("connect", "alpha", "bravo") типично 300–700 мс. Сценарий B — основной рабочий случай. Сценарий C — edge case для длинных фраз или замедленной речи.

### 6.5.4 MelSpec модель (TFLite)

Преобразует аудио в mel-спектрограмму для VoiceID encoder.

| Параметр | Значение | Примечание |
|----------|----------|------------|
| N_FFT | 1024 сэмплов | Размер окна FFT |
| HOP_LENGTH | 300 сэмплов | Шаг между окнами |
| MEL_OUTPUT_SIZE | 64 | Количество mel-бинов на фрейм |
| MEL_OUTPUT_COUNT | 24 | Количество mel-фреймов для окна 500 мс |
| Tensor arena (FW) | 50 KB | Память для TFLite на MCU2 |
| Модель | `model_svc_melspec_win_0_5` | Версия для окна 0.5 сек |

**Процесс:** На каждые N_FFT (1024) сэмплов с шагом HOP_LENGTH (300) вычисляется один mel-фрейм (64 float). Для 8000 сэмплов (500 мс): `floor((8000 - 1024) / 300) + 1 = 24` mel-фрейма.

### 6.5.5 VoiceID Encoder модель (TFLite)

Преобразует mel-спектрограмму в d-vector (embedding).

| Параметр | Значение | Примечание |
|----------|----------|------------|
| Вход | 64 × 24 float | Транспонированная mel-спектрограмма (MEL_OUTPUT_SIZE × MEL_OUTPUT_COUNT) |
| Выход | **128 float** | d-vector (embedding), L2-нормализованный |
| Tensor arena (FW) | 115 KB | Память для TFLite на MCU2 |
| Модель | `model_svc_voice_embedder_win_0_5` | Версия для окна 0.5 сек |

### 6.5.6 Predictor (верификация) — МОДУЛЬНАЯ АРХИТЕКТУРА

Сравнивает embedding с записанным centroid'ом пользователя.

> **⚠️ АРХИТЕКТУРНОЕ ТРЕБОВАНИЕ:** Модуль классификатора (расчёт distance от embedding до centroid и принятие решения known/unknown) должен быть реализован как **заменяемый компонент** с чётким интерфейсом. Уже сейчас тестируются несколько алгоритмов классификации, которые будут предоставлены как Python код для интеграции. Код MCU2 должен поддерживать замену классификатора без изменения остальной SR pipeline.

**Интерфейс классификатора (абстракция):**
```
interface SrClassifier:
    init(centroids, params)                    → void
    classify(embedding[128]) → (bool is_known, float distance, int voice_id)
    addVoice(embeddings[], voice_id)           → void  (enrollment)
    reset()                                     → void
```

**Текущая реализация (baseline): Cosine + Weibull**

| Параметр | Значение (default) | Описание |
|----------|-------------------|----------|
| D_T (distance threshold) | **0.47** | Порог cosine distance. Distance = 1 − cosine_similarity |
| CDF_T (Weibull CDF threshold) | **1.0** | Порог Weibull CDF. При 1.0 фактически отключён |
| nVoices | 5 | Максимальное количество записанных голосов |

**Алгоритм решения (текущий):**
1. Embedding нормализуется (L2)
2. Вычисляется cosine_similarity с каждым centroid'ом
3. Distance = 1 − cosine_similarity
4. Находим минимальный distance → ближайший голос
5. Weibull CDF проверка (при CDF_T < 1.0)
6. Если `distance ≤ D_T` И `CDF ≤ CDF_T` → **SR=true** (известный голос)
7. Иначе → **SR=false** (неизвестный голос)

> **Для NeuroComm:** SR работает однократно на буфер команды. Одно решение — одно (или два) embedding. Будущие классификаторы будут поставляться как Python → C++ конвертация через тот же интерфейс.

### 6.5.7 Enrollment (запись голоса, SERVICE menu)

Для регистрации голоса пользователя:

| Параметр | Значение | Примечание |
|----------|----------|------------|
| EMBEDDINGS_PER_CENTROID_COUNT | 20 | Количество embedding'ов для вычисления centroid |
| Длительность записи | 20 × 0.5 сек = **10 сек** голоса | Пользователь говорит ~10 сек |
| Хранение | Flash MCU2 (ParamManager) | Centroid + параметры классификатора |

**Процесс enrollment:**
1. Пользователь выбирает SR Config → Start Record Voice в SERVICE menu
2. MCU2 начинает запись: каждый голосовой сегмент (VAD=ON) → MelSpec → Encoder → embedding
3. Накапливается 20 embedding'ов (по одному на ~0.5 сек голоса)
4. Centroid = среднее по всем embedding'ам, L2-нормализованное
5. Параметры классификатора фитятся (Weibull: shape, scale по distances)
6. Сохраняется в Flash

### 6.5.8 Ресурсы MCU2 для SR

| Ресурс | MelSpec | VoiceID Encoder | Итого SR |
|--------|---------|-----------------|----------|
| Tensor arena | 50 KB | 115 KB | **165 KB** |
| Centroid storage | — | — | 128 float × nVoices + classifier params ≈ **3 KB** |
| Voice buffer | — | — | 8256 float ≈ **33 KB** (encoder input + 1 window) |
| Mel buffer | 24 × 64 float ≈ 6 KB | — | **6 KB** |

> **Важно:** Tensor arena для MelSpec и VoiceID могут разделять память (sequential execution). Voice buffer уменьшен с 65 KB до 33 KB благодаря encoder input = 8000 вместо 16000.

### 6.5.9 SR в контексте NeuroComm (режим AI-VOX PRO + SR)

Последовательность SR верификации в NeuroComm:
1. KWS обнаружил стартовую команду (cmd 1–3) в Sensory буфере
2. Sensory endpoint detection выделяет аудио-сегмент команды (len = N сэмплов)
3. MCU2 проверяет длину:
   - N < 4000 (< 250 мс) → SR=false, skip
   - N ≤ 8000 (250–500 мс) → looping до 8000, один embedding (Сценарий B)
   - N > 8000 (> 500 мс) → два пакета, два embedding'а (Сценарий C)
4. MelSpec: аудио → mel-спектрограмма (24 фрейма × 64 бинов)
5. VoiceID Encoder: mel → embedding (128 float)
6. Classifier: embedding(s) vs centroid → distance → SR=true/false
7. MCU2 генерирует beep (confirm/reject) и шлёт `SR_CONFIRMED` / `SR_REJECTED` → MCU3

**Время выполнения SR** (оценка для STM32H7 @ 480 MHz):
- MelSpec inference: ~5–10 мс (×1 или ×2)
- VoiceID inference: ~20–50 мс (×1 или ×2)
- Classifier (math): < 1 мс
- **Итого: ~30–60 мс** (один пакет) / ~60–120 мс (два пакета)

---

## 7. MCU1 ↔ MCU2 Протокол (as-built baseline)

### 7.1 Физический линк (текущая демо-плата)

- UART
- MCU1 pin 14 (RX) ↔ MCU2 pin 14
- MCU1 pin 15 (TX) ↔ MCU2 pin 15
- Реальный интерфейс: **USART3**
- Baud rate: **1 000 000**

> UART4 @ 500K используется для других целей, не MCU1↔MCU2

---

### 7.2 Ограничения пропускной способности

- 1 000 000 baud ≈ 100 kB/s полезных данных
- Аудио 16 kHz / 16 bit = 32 kB/s на канал (два канала: RX + MIC)

➡️ **Минимальный запас**, любой оверхед критичен  
➡️ Архитектура должна быть **детерминированной**

---

### 7.3 Роли по времени

- **MCU2 владеет временем**
- MCU1 работает как slave accelerator
- Таймстемпы (если есть) задаются MCU2

---

### 7.4 События MCU1 → MCU2

Минимальный набор:
- `EVT_VAD` (VAD = 0 / 1 + confidence + chunk_index)
- `AUDIO_RX_FRAME` / `AUDIO_TX_FRAME` (только если разрешено через CMD_SET_STREAMS)
- `EVT_ERROR` (при ошибках)

> Полный список сообщений, формат фреймов и протокол — см. `UART_Protocol_Spec_v1.3.5.md` (актуальная версия протокола)

---

### 7.5 Reset / Reconnect

При любом reset:
- все буферы обнуляются
- состояния сбрасываются
- MCU2 повторно конфигурирует MCU1

---

## 7.6 Входные буферы и данные модулей (сводка)

> Этот раздел консолидирует описание входных данных, буферов и их жизненного цикла для каждого модуля обработки. Детали алгоритмов — в соответствующих разделах документа и в `NeuroComm_State_Diagrams_v1.3.6.md`.

### 7.6.1 VAD (Voice Activity Detection) — MCU1

| Параметр | Значение | Примечание |
|----------|----------|------------|
| **Расположение** | MCU1 | Единственный модуль на MCU1 |
| **Вход** | MIC (In1), int16, 16 kHz | После LowFreq filter + clip control |
| **Рабочий чанк** | 256 сэмплов = 16 мс | VAD-сеть: вход 128 сэмплов, анализ каждого 2-го блока |
| **VAD Start Marker** | 4 чанка подряд VAD=true (64 мс) | Единый для всех режимов, задаётся при инициализации |
| **VAD Stop Marker** | 3 чанка подряд VAD=false (48 мс) | Единый для всех режимов, задаётся при инициализации |
| **Кольцевой pre-roll буфер** | 10 чанков × 16 мс = 160 мс = 5120 байт | 11-й вытесняет 1-й |
| **Pre-roll при VAD Start** | 4 чанка = 64 мс из кольцевого буфера | Начало фразы не теряется |
| **Выход** | `EVT_VAD(flag=0/1)` + `AUDIO_TX_FRAME` (int16 LE) | Аудио передаётся **только при VAD=ON** |
| **При VAD=OFF** | Аудио не передаётся. Только `EVT_VAD(flag=0)` | Экономия UART bandwidth |

**Жизненный цикл:**
1. MCU1 непрерывно анализирует MIC чанками по 16 мс
2. Чанки записываются в кольцевой pre-roll буфер
3. VAD Start (4 подряд true) → передача pre-roll + realtime по UART
4. VAD Stop (3 подряд false) → прекращение передачи, `EVT_VAD(flag=0)`

### 7.6.2 VE (Voice Enhancement) — MCU1

| Параметр | Значение | Примечание |
|----------|----------|------------|
| **Расположение** | MCU1 | Обработка на том же MCU что VAD |
| **Вход TX** | MIC (In1) | Исходящий голос → VE → UART → MCU2 |
| **Вход RX** | Radio SPK (In2) | Входящий сигнал → VE → UART → MCU2 → наушник |
| **Контракт VE=OFF** | Выход = BYPASS (точная копия входа) | Никакого mute, никакого undefined |
| **Режимы** | VE TX / VE RX / VE Always / VE OFF | Конфигурируется через SERVICE menu (MCU3 → MCU2 → MCU1) |
| **Формат входа** | int16, 16 kHz, 256 сэмплов (16 мс) | Тот же формат что VAD chunk |
| **Формат выхода** | int16, 16 kHz | UART → MCU2 (AUDIO_TX_FRAME / AUDIO_RX_FRAME) |

**Жизненный цикл:**
- VE включён раздельно (TX/RX) или всегда (Always)
- При VE=OFF → bypass контракт: выход = точная копия входа
- KWS получает bypass если VE раздельный, VE stream если VE_always_on

### 7.6.3 KWS (Keyword Spotting) — MCU2, Sensory API

| Параметр | Значение | Примечание |
|----------|----------|------------|
| **Расположение** | MCU2 | Sensory TrulyHandsfree engine |
| **Вход** | AUDIO_TX_FRAME от MCU1 (int16 → float32) | MCU2 конвертирует при приёме |
| **Sensory brick** | 240 сэмплов = 15 мс | Порция на один вызов `SensoryProcessData()` |
| **Перепаковка** | MCU1 шлёт 256 (16 мс) → MCU2 перепаковывает в 240 (15 мс) | Промежуточный `sensoryBuffer`, **не 1:1** |
| **Внутренний буфер Sensory** | 630 мс = 10080 сэмплов ≈ 20 KB | AUDIO_BUFFER_MS, кольцевой |
| **InputBuffer MCU2** | 32000 сэмплов = 2 сек (float) | Промежуточный кольцевой буфер |
| **BACKOFF_MS** | 270 мс | Ретроспективный просмотр для endpoint detection |
| **KWS timeout** | 5 сек (конфигурируемый) | Таймаут ожидания команды |
| **Выход** | Trigger event: cmd_id (1–4) + аудио-сегмент (endpoint detection) | cmd 1–3 = стартовые, cmd 4 = disconnect |

**Жизненный цикл (KWS фаза):**
1. MCU1 шлёт AUDIO_TX_FRAME **только при VAD=ON** (+ pre-roll)
2. MCU2 конвертирует int16 → float32, пишет в sensoryBuffer
3. MCU2 перепаковывает 256 → 240 сэмплов, подаёт в Sensory
4. Sensory анализирует непрерывно (wake word + commands)
5. VAD=OFF → MCU1 прекращает передачу, MCU2 отдаёт Sensory остаток
6. Trigger найден → аудио-сегмент команды передаётся SR (если PRO+SR)
7. Trigger не найден → Sensory restart, буфер сброшен, ожидание следующего VAD=ON

### 7.6.4 SR (Speaker Recognition) — MCU2, TFLite

| Параметр | Значение | Примечание |
|----------|----------|------------|
| **Расположение** | MCU2 | MelSpec + VoiceID (TFLite) + Classifier |
| **Вход** | Аудио-сегмент команды из Sensory endpoint detection | float32, тот же буфер что KWS |
| **Encoder input size** | 8000 сэмплов = 500 мс | Фиксированный размер входа VoiceID модели |
| **Минимальная длина** | 4000 сэмплов = 250 мс | Короче → SR=false |
| **Looping** | Буфер < 8000 → циклическое копирование до 8000 | Типичный случай (команда 300–700 мс) |
| **Двойной пакет** | Буфер > 8000 → два пакета по 8000 (второй с looping) | Edge case, длинные фразы |
| **MelSpec** | N_FFT=1024, HOP=300, 64 mel-бинов, 24 фрейма | TFLite, arena 50 KB |
| **VoiceID Encoder** | Вход 64×24 mel → выход 128 float embedding | TFLite, arena 115 KB |
| **Classifier** | Модульный (baseline: cosine + Weibull) | D_T=0.47, CDF_T=1.0 |
| **Выход** | SR=true / SR=false + distance | → beep + `SR_CONFIRMED` / `SR_REJECTED` → MCU3 |

**Жизненный цикл (SR фаза — только AI-VOX PRO+SR):**
1. KWS trigger → Sensory выделяет аудио-сегмент
2. Проверка длины (< 250 мс → skip)
3. Looping до 8000 (или двойной пакет если > 8000)
4. MelSpec → VoiceID → embedding(s)
5. Classifier: embedding vs centroid → SR result
6. Beep + статус → MCU3

---

## 8. TX логика — режимы работы

> Детальные state diagrams и timing-диаграммы для всех режимов см. в файле:  
> **`NeuroComm_State_Diagrams_v1.3.6.md`**

### 8.1 AI-VOX (только VAD)

Распределение по MCU:
- **MCU1:** VAD-детекция (MIC), буферизация начала фразы, передача MIC-аудио по UART; приём RX-аудио (Radio SPK) и передача на MCU2
- **MCU2:** управление PTT по флагу VAD, выдача audio_out (TX или RX)

> **Half-duplex:** радиоканал — полудуплекс. PTT ON → TX (передача, RX заблокирован несущей). PTT OFF → RX (приём, Radio SPK → MCU1 → VE → MCU2 → наушник). TX и RX **взаимоисключающие**.

Алгоритм (TX path — MIC):
1. MCU1 получает сигнал с MIC и обрабатывает рабочими чанками по 16 мс (256 сэмплов; VAD-сеть: вход 128 сэмплов, анализ каждого 2-го блока)
2. MCU1 детектирует `VAD = true` для каждого чанка
3. MCU1 принимает решение на основании загруженной конфигурации (VAD Start Marker = 4, VAD Stop Marker = 3: параметры **единые для всех режимов**, задаются однократно при инициализации, не меняются при переключении режимов на MCU2)
4. После принятия решения `VAD=ON` MCU1 начинает передачу аудио по UART → MCU2: сначала **pre-roll** (4 чанка = 64 мс из кольцевого буфера — начало фразы), затем в реальном времени. Параллельно передаёт `EVT_VAD(flag=1)`
5. MCU2 получает `EVT_VAD(flag=1)`, включает `PTT = ON`
6. MCU2 начинает выдачу audio_out на линию out
7. Передача начинается **с pre-roll чанка**, чтобы не терять начало фразы
8. VAD продолжает мониторить сигнал с MIC. Если VAD Stop Marker = 3 чанков подряд `VAD = false`, MCU1 **прекращает передачу аудио** по UART и шлёт `EVT_VAD(flag=0)`
9. MCU2 при получении `EVT_VAD(flag=0)` выключает `PTT = OFF`
10. Система возвращается в состояние ожидания, готова к следующему VAD Start

RX path (Radio → наушник):
- При PTT=OFF: Radio SPK → MCU1 In2 → VE (если включён) → UART → MCU2 → наушник
- При PTT=ON: RX заблокирован (несущая занята передачей), MCU1 не передаёт RX-аудио

---

### 8.2 AI-VOX PRO (VAD + KWS, без SR)

Распределение по MCU:
- **MCU1:** VAD-детекция (MIC), передача MIC-аудио по UART **только при VAD=ON** (+ pre-roll); приём RX-аудио (Radio SPK) и передача на MCU2
- **MCU2:** KWS-детекция команд, state machine (connect/disconnect/timeout), управление PTT по VAD в активной сессии

> **Half-duplex:** PTT ON → TX (RX заблокирован). PTT OFF → RX (Radio SPK → MCU1 → VE → MCU2 → наушник). В Session Active (PTT OFF, ожидание VAD) RX **активен** — человек слышит входящее.

#### Фаза 1: Ожидание стартовой команды

**Поток данных MCU1 → MCU2:**
1. MCU1 непрерывно анализирует MIC чанками по 16 мс, но **не передаёт аудио по UART пока VAD=OFF**
2. VAD=ON → MCU1 начинает передачу: сначала pre-roll (4 чанка = 64 мс из кольцевого буфера), затем в реальном времени. Параллельно шлёт `EVT_VAD(flag=1)`
3. MCU2 получает аудио-фреймы и **пишет всё в Sensory** (перепаковка 256→240 сэмплов)
4. Sensory анализирует поток на наличие trigger (wake word) и команд 1–3
5. VAD=OFF → MCU1 шлёт `EVT_VAD(flag=0)` и **прекращает передачу аудио**
6. MCU2 при получении VAD=OFF:
   - Отдаёт Sensory остаток буфера на анализ
   - Sensory завершает анализ текущего сегмента
   - **Если trigger/команда не найдена** → Sensory restart, буфер сброшен, ожидание следующего VAD=ON
   - **Если команда 1–3 найдена** → переход в Фазу 2

> **Ключевой момент:** MCU1 шлёт аудио **только при VAD=ON**. Это экономит UART bandwidth и CPU MCU2 — шум не передаётся и не анализируется. Команда — это всегда короткий utterance с паузой после: человек произносит команду, замолкает и ждёт beep подтверждения.

#### Фаза 2: Активная сессия (Session Active)

При обнаружении стартовой команды MCU2:
1. Генерирует **beep** подтверждения в наушник
2. Отправляет `CMD_DETECTED: cmd_id` → MCU3 (дисплей)
3. Переходит в **активную сессию**:
   - `Enable Output = ON`, **PTT остаётся OFF** (ожидание VAD Start)
   - Отправляет `SESSION_ACTIVE` → MCU3
4. Ожидание речи: человек может молчать после команды
   - **Session Timeout (30 сек, конфигурируемый):** если VAD=ON не приходит за 30 сек → beep (timeout tone) + автозакрытие сессии → `SESSION_TIMEOUT` → MCU3

#### Фаза 3: Передача (AI-VOX режим)

5. VAD=ON → MCU1 возобновляет передачу аудио, MCU2 включает `PTT=ON`
6. MCU2 выдаёт audio_out на линию out, отправляет `VAD_ON` → MCU3
7. VAD=OFF → MCU1 прекращает передачу, MCU2 выключает `PTT=OFF` (пауза), отправляет `VAD_OFF` → MCU3
8. Сессия при паузах **не прерывается** — PTT cycling по VAD, Session Timeout перезапускается при каждом VAD=ON
9. В активной сессии KWS продолжает параллельный мониторинг (только cmd 4)

#### Фаза 4: Отключение

10. При обнаружении команды 4 (disconnect) или по Session Timeout MCU2 выполняет:
    - Генерирует **beep** подтверждения (disconnect или timeout — разные тоны)
    - `PTT = OFF`, `Enable Output = OFF`
    - Отправляет `SESSION_CLOSED` → MCU3 (дисплей)
    - Полный сброс state machine → возврат в начальное состояние (Фаза 1)

---

### 8.3 AI-VOX PRO + SR (VAD + KWS + однократная SR авторизация)

Распределение по MCU:
- **MCU1:** VAD-детекция (MIC), передача MIC-аудио по UART **только при VAD=ON** (+ pre-roll); приём RX-аудио (Radio SPK) и передача на MCU2
- **MCU2:** KWS-детекция команд, однократная SR верификация, state machine, управление PTT

> **Half-duplex:** идентично AI-VOX PRO (п. 8.2). В Session Active (PTT OFF) RX активен.

Алгоритм — аналогичен AI-VOX PRO (п. 8.2), но между обнаружением стартовой команды и активацией сессии добавляется SR верификация:

#### Фаза 1: Ожидание стартовой команды

Идентично AI-VOX PRO (п. 8.2, Фаза 1): MCU1 шлёт аудио только при VAD=ON, MCU2 пишет в Sensory, при VAD=OFF — Sensory анализирует и либо находит команду, либо restart.

#### Фаза 1.5: SR верификация (дополнительный этап)

При обнаружении стартовой команды (cmd 1–3) MCU2:
1. Генерирует **beep** подтверждения в наушник
2. Отправляет `CMD_DETECTED: cmd_id` → MCU3 (дисплей)
3. Передаёт **тот же KWS буфер** (Sensory endpoint detection) на SR модель для верификации говорящего

   **SR = true** → MCU2 генерирует **beep** подтверждения, отправляет `SR_CONFIRMED` → MCU3, переходит в Фазу 2. SR модель **выключается** (экономия ресурсов).

   **SR = false** → MCU2 генерирует **beep** отказа (другой тон), отправляет `SR_REJECTED` → MCU3. Возврат в Фазу 1.

> **Ключевое ограничение:** SR работает **только один раз** — на буфер стартовой команды. Не на весь речевой поток. Команда короткая: человек произнёс, замолчал, ждёт beep. SR анализирует именно этот сегмент.

#### Фазы 2–4: Активная сессия, передача, отключение

Идентично AI-VOX PRO (п. 8.2, Фазы 2–4): Session Active (PTT OFF, ожидание VAD) → PTT cycling по VAD → disconnect (cmd 4) или Session Timeout (30 сек). SR в активной сессии **выключен**.

---

### 8.4 Сводная таблица: распределение функций по MCU в TX режимах

| Функция | MCU1 | MCU2 | MCU3 | Примечание |
|---------|------|------|------|------------|
| VAD-детекция | ✅ | — | — | Все режимы |
| Аудио буфер (начало фразы) | ✅ (160 мс) | — | — | Кольцевой pre-roll, 10 чанков × 16 мс |
| Передача MIC-аудио UART | ✅ | — | — | MCU1 → MCU2 (только при VAD=ON) |
| Приём RX-аудио (Radio SPK) | ✅ | — | — | Radio SPK → MCU1 → VE → UART → MCU2 |
| KWS-детекция | — | ✅ | — | AI-VOX PRO, AI-VOX PRO + SR |
| SR верификация | — | ✅ | — | Только AI-VOX PRO + SR, однократно |
| State machine | — | ✅ | — | AI-VOX PRO, AI-VOX PRO + SR |
| PTT управление | — | ✅ | — | По флагу VAD от MCU1 |
| Audio out (TX) | — | ✅ | — | MCU2 → Radio TX (MIC path, при PTT=ON) |
| Audio out (RX) | — | ✅ | — | MCU2 → наушник (RX path, при PTT=OFF) |
| Half-duplex | ✅ | ✅ | — | PTT ON → TX (RX заблокирован). PTT OFF → RX |
| Beep подтверждение | — | ✅ | — | Генерация тона при смене состояния |
| Отображение статуса | — | — | ✅ | По сообщениям от MCU2 |

---

## 9. Логирование и диагностика

> **Источник:** Анализ Taiwan прошивки (vadvesr-taiwan), basebox.h, vadbox.h, vadsrbox.h, basebox_mescodes.h, msgpackstruct.hpp.

### 9.1 Типы логов и нотификаций (из Taiwan кода)

В Taiwan прошивке все логи и нотификации передаются по протоколу MCU ↔ Python app через msgpack. Типы сообщений:

**Нотификации алгоритмов (runtime):**

| MessageCode | Структура | Описание | Условие отправки |
|-------------|-----------|----------|-----------------|
| `kVadNotification` | VadNotification: ts_ms, count, min, max, map (64-bit bitmap) | VAD статистика: timestamp, количество чанков, VAD bitmap | Если `LoggingParams.VADMap = true` |
| `kVadExtended` | — | Расширенная VAD информация | Если `LoggingParams.VADExtMap = true` |
| `kSrMessage` | SRNotification: SRMessage enum | SR события (known/unknown/record/etc) | Всегда (кроме kPredictorNotInited) |
| `kSensoryNotification` | SensoryNotification: language, eventType, index | KWS/WWD события: trigger detected, listening started/stopped | При событиях Sensory |
| `kVoiceObtained` | — | Голосовой фрагмент получен (VAD segment ready) | При завершении VAD-сегмента |

**SR нотификации (детализация SRMessage enum):**

| SRMessage | Значение | Описание |
|-----------|----------|----------|
| kUnknown | 0 | SR: голос не распознан |
| kKnown | 1 | SR: голос распознан |
| kPredictorNotInited | 2 | SR: модель не обучена (не отправляется в протокол) |
| kRecordStarted | 3 | Enrollment: запись начата |
| kRecordFinished | 4 | Enrollment: запись завершена (centroid вычислен) |
| kRecordOneSec | 5 | Enrollment: записана 1 порция (embedding) |
| kVoiceThrewAway | 6 | SR: голосовой фрагмент отброшен (слишком короткий) |
| kDetectedKnown | 7 | SR: финальное решение — известный (после SrVoiceDetector) |
| kDetectedUnknown | 8 | SR: финальное решение — неизвестный |

**Параметры и конфигурация (по запросу):**

| MessageCode | Направление | Описание |
|-------------|-------------|----------|
| `kGetParams` | App → MCU | Запрос текущих параметров |
| `kVadParams` / `kSetVadParams` | Двунапр. | VAD: VadThr, startMarkerSize, stopMarkerSize |
| `kSrParams` / `kSetSrParams` | Двунапр. | SR: distance, cdf, startTimeout, stopTimeout, nVoices |
| `kVeParams` / `kSetVeParams` | Двунапр. | VE: volControlDecrease, veIncreaseCoefficient |
| `kSensoryParams` / `kSetSensoryParams` | Двунапр. | Sensory: OP, KWSTimeout, language, inputBufferLatency |
| `kBaseParams` / `kSetBaseParams` | Двунапр. | Base: gain, useAgc |
| `kVeSrMode` / `kSetVeSrMode` | Двунапр. | Текущий режим (AI_MUTE, AI_MUTE_VE, AI_MUTE_SR, etc) |
| `kSetLoggingParams` | App → MCU | Включить/выключить VADMap, VADExtMap |

**Запись аудио (отладка):**

| MessageCode | Описание |
|-------------|----------|
| `kRecordStartInput` | Начать запись входного аудио (MIC raw) |
| `kRecordStartOutput` | Начать запись выходного аудио (после VE) |
| `kRecordStartSREncoder` | Начать запись входа SR encoder (для отладки embedding) |
| `kRecordStop` | Остановить запись |
| `kLc3` | LC3-сжатые аудио данные (для передачи по BLE) |

**Служебные:**

| MessageCode | Описание |
|-------------|----------|
| `kDeviceInfo` | Тип бокса, версии FW (STM + Nordic), время сборки |
| `kModelVersion` | Версия каждой модели (VAD, VE, MelSpec, VoiceID) |
| `kFirmwareError` | Debug message: severity (BLOCKER..ENHANCEMENT) + текст (128 char) |
| `kStart` / `kStop` | Старт/стоп алгоритмов |

### 9.2 Логирование для NeuroComm (требования)

На основе анализа Taiwan + архитектуры NeuroComm, определяем категории логов:

**Категория 1: Runtime телеметрия (для дисплея MCU3)**
- VAD ON/OFF events → MCU3
- KWS trigger events (cmd 1–4) → MCU3
- SR result (confirmed/rejected) → MCU3
- Session events (active/closed/timeout) → MCU3
- PTT state changes → MCU3

> Эти события уже описаны в разделе 4.2 (Beep/Status таблица) и реализуются через MCU2 → MCU3 протокол.

**Категория 2: Debug телеметрия (для Python config app, USB/BLE)**
- VAD bitmap (64-bit map чанков voice/silence) — включается по запросу
- SR distance values (числовое значение cosine distance для каждого embedding)
- SR classifier debug: embedding vector, centroid, distance, CDF
- KWS Sensory confidence scores
- Audio levels (RMS/peak per window)
- VE gain state

**Категория 3: Аудио запись (для offline анализа)**
- Запись MIC raw (вход VAD, до VE)
- Запись после VE (что подаётся на KWS)
- Запись SR encoder input (что подаётся на MelSpec)
- Формат:PCM для USB

**Категория 4: Конфигурация (параметры для тюнинга через Python app)**
- VAD: threshold, start/stop markers
- SR: D_T, CDF_T, nVoices, min voice length
- VE: gain coefficients, mode
- KWS: Sensory OP level, timeout, language
- System: mode (AI-VOX/PRO/PRO+SR), session timeout
- Base: AGC gain

**Категория 5: Firmware ошибки**
- Severity levels: BLOCKER, CRITICAL, MAJOR, MINOR, TRIVIAL, ENHANCEMENT
- Текстовое описание (до 128 символов)
- Timestamp

### 9.3 Интерфейс логирования

| Канал | Протокол | Используется для |
|-------|----------|-----------------|
| MCU2 → MCU3 (UART 500K) | Статусные сообщения | Дисплей + LED |
| MCU2 → Python App (USB через MCU3) | msgpack binary | Config, debug телеметрия, аудио запись |


> **Примечание:** Детальный протокол debug-канала будет описан в отдельном документе (Debug/Config Tool Spec) при подготовке Python приложения.

---

## 10. Следующие шаги

1. Зафиксировать Architecture Spec (этот документ)
2. Реализовать:
   - reference MCU1 firmware (NASP emulator)
   - минимальный MCU2 mock
3. Только после этого:
   - интеграция SR/KWS
   - оптимизация под low-power SoC
   - подготовка к NASP silicon

---

## 10. Ключевой принцип

**MCU1 — алгоритмы. MCU2 — продукт.**

---

## Changelog

| Версия | Дата | Изменения |
|--------|------|-----------|
| 1.0 | — | Первоначальная архитектура |
| 1.2 | — | Добавлены заглушки для TX логики AI-VOX PRO и AI-VOX PRO + SR |
| 1.3 | 2025-02-07 | Исправлены противоречия и синтаксис. Заполнены разделы 8.2 и 8.3. Добавлены: beep-подтверждение при смене состояний (MCU2 → наушник), передача статуса MCU2 → MCU3 для дисплея, таблица событий beep/status (п. 4.2), отображение статуса на MCU3 (п. 5.2). Ссылки на `NeuroComm_State_Diagrams_v1.3.6.md` и `UART_Protocol_Spec_v1.3.5.md` |

