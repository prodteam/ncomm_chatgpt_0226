# NeuroComm — State Diagrams & Signal Flow

> **Источник:** 2025_NeuroComm_state_diagramm.pdf  
> **Дата конвертации:** 2025-02-07  
> **Версия документа:** 1.4.0 
> **Изменения v1.3.6:** Добавлены MCU2→MCU3 status messages во все sequence diagrams. Исправлены неканонические наименования режимов. Обновлены cross-references на v1.3.6. SR encoder input = 8000 сэмплов (500 мс). Добавлен half-duplex RX path.

---

## Оглавление

1. [Обзор режимов](#обзор-режимов)
2. [AI-VOX](#1-ai-vox)
3. [AI-VOX PRO](#2-ai-vox-pro) *(новый с v1.2)*
4. [AI-VOX PRO + SR](#3-ai-vox-pro--sr) *(переименован с v1.2)*
5. [Сравнительная таблица режимов](#сравнительная-таблица-режимов)
6. [Параметры маркеров и буферов](#параметры-маркеров-и-буферов)

---

## Обзор режимов

```mermaid
graph LR
    A["AI-VOX<br/>(только VAD)"] --> B["AI-VOX PRO<br/>(VAD + KWS)"]
    B --> C["AI-VOX PRO + SR<br/>(VAD + KWS + SR gate)"]

    style A fill:#4CAF50,color:#fff
    style B fill:#FF9800,color:#fff
    style C fill:#f44336,color:#fff
```

Все три режима используют общий входной буфер (Input signal and Buffer) и систему VAD (Voice Activity Detection). Каждый последующий режим добавляет дополнительный уровень проверки перед разрешением передачи:

- **AI-VOX** — VAD обнаружил голос → сразу передача
- **AI-VOX PRO** — VAD + KWS: стартовая команда (1–3) открывает передачу, команда 4 (disconnect) закрывает. SR не используется.
- **AI-VOX PRO + SR** — то же что AI-VOX PRO, но стартовая команда дополнительно проверяется через SR модель (однократно, как «второй ключ»). SR не работает на поток речи — только на авторизацию.

---

## 1. AI-VOX

**Описание:** Базовый режим голосовой активации. Используется только VAD для определения наличия речи. Как только VAD обнаруживает голос — начинается передача.

### 1.1 Механика кольцевого буфера (КРИТИЧНО)

Входной буфер работает как **кольцевой буфер фиксированной длины**:

- Вход VAD-сети: **128 сэмплов** (8 мс при 16 kHz)
- Рабочий чанк VAD: **256 сэмплов = 16 мс** (VAD анализирует каждый второй блок по 128 — пропуск не влияет на точность, подтверждено тестами)
- Размер буфера: **10 чанков = 160 мс = 2560 сэмплов = 5120 байт**
- Буфер заполняется непрерывно: каждый новый чанк записывается в следующую позицию
- При переполнении: **11-й чанк вытесняет 1-й** (кольцевая перезапись)
- VAD анализирует каждый рабочий чанк (16 мс) независимо (voice / not voice)

**Логика VAD Start (set to 4):**
- VAD считает **последовательные** рабочие чанки с обнаруженным голосом
- Пока нет 4 подряд — **флаг VAD Start не выставляется**, буфер продолжает перезаписываться
- Как только 4 чанка подряд = voice → VAD Start срабатывает
- 4 чанка × 16 мс = **64 мс** задержка принятия решения

**Ключевой момент — привязка Output Start Marker:**

```
Output Start Marker = Current Position − VAD Start Marker (в чанках)
```

Это означает: выдача начинается **не с текущего чанка, а с чанка, где голос реально начался** — на 4 чанка (64 мс) назад в буфере.

**Пример:**

```
Чанк:         1     2     3     4     5     6     7     8     9    10    11    12
Время (мс):   0    16    32    48    64    80    96   112   128   144   160   176
VAD:          —     —     —     V     V     V     V     V     V     V     V     V
                                ↑                       ↑
                          начало голоса            VAD Start
                          (чанк 4, 48 мс)          (чанк 8, 112 мс)
                                |                       |
                       Output Start Marker         VAD принял решение
                                |
                       Выдача начинается отсюда → O4, O5, O6, O7, O8, O9...
```

- Чанки 1–3: шум, не содержат голоса
- Чанки 4–7: голос, но VAD ещё набирает 4 подтверждения подряд
- Чанк 8 (112 мс): VAD Start — 4-й подряд чанк с голосом, решение принято
- Output Start Marker → чанк 4 (48 мс) — начало реального голоса
- Output signal: **O4, O5, O6, O7, O8, O9, O10...** — первые 4 блока (O4–O7) берутся ретроспективно из кольцевого буфера, далее в реальном времени

> **Следствие:** Буфер 10 чанков (160 мс) с запасом вмещает pre-roll для VAD Start Marker = 4 (минимум 4 чанка = 64 мс). Оставшиеся 6 чанков (96 мс) — запас на случай задержек выдачи.

### 1.2 State Diagram

```mermaid
stateDiagram-v2
    [*] --> Idle

    Idle --> BufferFilling : Входной сигнал поступает

    state "Кольцевой буфер (10 чанков)" as BufferFilling {
        [*] --> Writing : Чанк записан
        Writing --> VAD_Check : VAD анализ чанка
        VAD_Check --> Writing : VAD=false или<br/>< 4 подряд → продолжаем<br/>(11-й вытесняет 1-й)
        VAD_Check --> VAD_Triggered : 4 чанка подряд VAD=true
    }

    VAD_Triggered --> OutputStart : Output Start Marker =<br/>Current Position − 4 чанка<br/>(начало реального голоса)
    OutputStart --> EnableOutput : Enable Output = ON
    EnableOutput --> PTT_ON : PTT включается
    PTT_ON --> Transmitting : Output Start/Finish = Start<br/>Выдача с позиции маркера

    state "Передача" as Transmitting {
        [*] --> PreRoll : Блоки из буфера<br/>(ретроспективные: 4 чанка)
        PreRoll --> RealTime : Блоки в реальном времени<br/>(O8, O9, O10...)
        RealTime --> RealTime : Непрерывная выдача
    }

    Transmitting --> VAD_Stop : VAD Stop Marker = 3<br/>(3 чанка подряд VAD=false)
    VAD_Stop --> OutputFinish : Output Finish Marker
    OutputFinish --> PTT_OFF : PTT выключается
    PTT_OFF --> Idle : Output Start/Finish = Finish

    Idle --> BufferFilling : Новый VAD Start<br/>(повторное обнаружение)

    note right of BufferFilling
        Кольцевой буфер: 10 чанков × 16 мс
        = 160 мс = 5120 байт.
        Непрерывная запись.
        11-й чанк вытесняет 1-й.
        VAD считает подряд идущие
        чанки с голосом.
    end note

    note right of Transmitting
        Первые 4 блока — из буфера
        (ретроспективно).
        Далее — в реальном времени.
    end note
```

### 1.3 Timing / Signal Flow

```mermaid
sequenceDiagram
    participant Buf as Ring Buffer<br/>(10×16ms = 160ms)
    participant VAD as VAD Engine
    participant Out as Output Control
    participant PTT as PTT
    participant Radio as Radio TX
    participant MCU3 as MCU3<br/>(Display)

    Note over Buf: Непрерывная запись чанков (16 мс / 256 сэмплов).<br/>VAD-сеть: вход 128 сэмплов, анализ каждого 2-го блока.<br/>11-й чанк вытесняет 1-й.

    Buf->>VAD: Чанк 1 (0 мс) → VAD=false (шум)
    Buf->>VAD: Чанк 2 (16 мс) → VAD=false
    Buf->>VAD: Чанк 3 (32 мс) → VAD=false

    Buf->>VAD: Чанк 4 (48 мс) → VAD=true (голос!) [1/4]
    Buf->>VAD: Чанк 5 (64 мс) → VAD=true [2/4]
    Buf->>VAD: Чанк 6 (80 мс) → VAD=true [3/4]
    Buf->>VAD: Чанк 7 (96 мс) → VAD=true [4/4]

    Note over VAD: ✅ VAD Start! (4 подряд × 16 мс = 64 мс)<br/>Текущая позиция = чанк 8 (112 мс)

    VAD->>Out: Output Start Marker = чанк 8 − 4 = чанк 4<br/>(48 мс — начало реального голоса)

    Out->>Out: Enable Output = ON
    Out->>PTT: PTT ON
    PTT->>Radio: Output Start
    Out->>MCU3: VAD_ON → дисплей: TX : AI-VOX, VOICE, PTT AUTO

    Note over Buf,Radio: Выдача из буфера (ретроспективно, 32 мс)

    Buf->>Out: O4 (48 мс, из буфера)
    Buf->>Out: O5 (64 мс, из буфера)
    Buf->>Out: O6 (80 мс, из буфера)
    Buf->>Out: O7 (96 мс, из буфера)

    Note over Buf,Radio: Выдача в реальном времени

    Buf->>Out: O8 (112 мс, реальное время)
    Out->>Radio: Выходной сигнал
    Buf->>Out: O9, O10, O11...
    Out->>Radio: Выходной сигнал

    Note over VAD: ... передача продолжается ...

    Buf->>VAD: Чанк N → VAD=false [1/3]
    Buf->>VAD: Чанк N+1 → VAD=false [2/3]
    Buf->>VAD: Чанк N+2 → VAD=false [3/3]

    Note over VAD: ⛔ VAD Stop! (3 подряд × 16 мс = 48 мс без голоса)

    VAD->>Out: Output Finish Marker
    Out->>PTT: PTT OFF
    PTT->>Radio: Output Finish
    Out->>MCU3: VAD_OFF → дисплей: RX : AI-VOX

    Note over Buf: Возврат в ожидание.<br/>Буфер продолжает писать.<br/>RX активен (half-duplex: PTT OFF → приём).
```

### 1.4 Ключевые параметры AI-VOX

| Параметр | Значение | Описание |
|---|---|---|
| Вход VAD-сети | 128 сэмплов = 8 мс | Нативный размер входа модели |
| Рабочий чанк VAD | 256 сэмплов = 16 мс = 512 байт | VAD анализирует каждый 2-й блок (экономия, без потери точности) |
| Размер кольцевого буфера | 10 чанков = 160 мс = 5120 байт | 11-й вытесняет 1-й |
| VAD Start Marker | set to 4 (конфигурируемый) | 4 чанка подряд VAD=true (64 мс) → голос подтверждён |
| VAD Stop Marker | set to 3 (конфигурируемый) | 3 чанка подряд VAD=false (48 мс) → голос пропал |
| Output Start Marker | Current Position − VAD Start Marker | Начало выдачи = начало реального голоса (на 4 чанка / 64 мс назад) |
| Output Finish Marker | Позиция последнего VAD=true чанка | Конец выдачи |
| Enable Output | ON после VAD Start, OFF после VAD Stop | Разрешение выхода |
| PTT | ON/OFF синхронно с Enable Output | Управление передатчиком |
| Pre-roll | 4 чанка = 64 мс из буфера | Ретроспективная выдача начала фразы |
| Выдача после pre-roll | Реальное время | Блоки передаются по мере поступления |

---

## 2. AI-VOX PRO

> ⚠️ **Новый в v1.2** — заменяет старый «AI-VOX + SR»

**Описание:** Командный режим с голосовым управлением (KWS — Keyword Spotting). После обнаружения голоса через VAD, система накапливает KWS буфер и ожидает голосовую команду. Стартовые команды (1–3) активируют передачу (переход в AI-VOX режим). Команда 4 (disconnect) завершает сессию. **SR в этом режиме не используется** — авторизация только по KWS.

### 2.0 Принцип работы (обзор)

```mermaid
graph TD
    A[VAD обнаружил голос] --> B[KWS буфер накапливается]
    B --> C{KWS: какая команда?}
    C -->|Команда 1–3<br/>стартовые| F["Сессия активна<br/>(Enable Output = ON, PTT OFF)<br/>Ожидание VAD Start"]
    C -->|Нет команды /<br/>неизвестная| B
    F --> F2["VAD Start → PTT ON<br/>(передача)"]
    F2 --> F3["VAD Stop → PTT OFF<br/>(пауза, сессия не закрывается)"]
    F3 --> F2
    F2 --> G["KWS мониторинг продолжается"]
    G --> H{KWS: команда 4?}
    H -->|Команда 4<br/>disconnect| J[Возврат в начальное состояние]
    H -->|Нет команды 4| G
    J --> A

    style A fill:#2196F3,color:#fff
    style C fill:#FF9800,color:#fff
    style F fill:#4CAF50,color:#fff
    style G fill:#FF9800,color:#fff
    style H fill:#FF9800,color:#fff
    style J fill:#f44336,color:#fff
```

### 2.1 Таблица команд KWS

| ID | Команда | Тип | Действие |
|---|---|---|---|
| 1 | *(стартовая команда 1)* | Start | Переход в AI-VOX режим |
| 2 | *(стартовая команда 2)* | Start | Переход в AI-VOX режим |
| 3 | *(стартовая команда 3)* | Start | Переход в AI-VOX режим |
| 4 | disconnect | Stop | Выход из AI-VOX → возврат в начальное состояние |

> **Примечание:** Конкретные ключевые слова для команд 1–3 определяются конфигурацией KWS модели (Sensory). Команда 4 "disconnect" — фиксированная.

### 2.2 State Diagram

```mermaid
stateDiagram-v2
    [*] --> Idle : Начальное состояние

    state "Фаза ожидания команды" as WaitPhase {
        Idle --> VAD_Active : VAD Start<br/>(голос обнаружен)
        VAD_Active --> KWS_Buffering : Накопление KWS буфера

        KWS_Buffering --> KWS_Cmd_Detected : KWS: команда 1, 2 или 3<br/>(стартовая команда)
        KWS_Buffering --> KWS_Buffering : Нет команды /<br/>неизвестная → продолжаем
        KWS_Buffering --> Idle : VAD Stop<br/>(голос пропал без команды)
    }

    state "Фаза передачи (AI-VOX режим)" as TxPhase {
        KWS_Cmd_Detected --> Session_Active : Стартовая команда принята<br/>Beep подтверждение<br/>Enable Output = ON<br/>PTT остаётся OFF

        Session_Active --> AIVOX_Transmitting : VAD Start<br/>(голос обнаружен)<br/>PTT ON → Output Start

        state KWS_Monitor <<choice>>
        AIVOX_Transmitting --> KWS_Monitor : KWS мониторинг<br/>параллельно

        KWS_Monitor --> AIVOX_Transmitting : Нет команды 4<br/>→ продолжаем передачу
        KWS_Monitor --> Disconnect : KWS: команда 4<br/>(disconnect)
    }

    state "VAD cycling в сессии" as VADCycle {
        AIVOX_Transmitting --> VAD_Pause : VAD Stop<br/>(пауза в речи)<br/>PTT OFF
        VAD_Pause --> AIVOX_Transmitting : VAD Start<br/>(речь возобновилась)<br/>PTT ON
    }

    Session_Active --> Disconnect : KWS: команда 4<br/>(disconnect до начала речи)
    Session_Active --> Timeout : Session Timeout (30 сек)<br/>нет VAD Start
    VAD_Pause --> Disconnect : KWS: команда 4<br/>(disconnect в паузе)
    VAD_Pause --> Timeout : Session Timeout (30 сек)<br/>тишина дольше таймаута

    Timeout --> Idle : Beep (timeout tone)<br/>Enable Output = OFF<br/>Полный сброс
    Disconnect --> Idle : PTT OFF<br/>Enable Output = OFF<br/>Полный сброс

    note right of Session_Active
        СЕССИЯ АКТИВНА, но PTT = OFF.
        Человек может молчать
        секунды после команды.
        PTT включится только
        по VAD Start.
        Если тишина > 30 сек →
        таймаут, сессия закрывается.
    end note

    note right of KWS_Monitor
        В активной сессии KWS
        продолжает работать
        параллельно и слушает
        команду 4 (disconnect).
    end note

    note left of VAD_Pause
        В AI-VOX режиме VAD
        управляет PTT ON/OFF
        для пауз в речи, но
        НЕ выходит из сессии.
    end note
```

### 2.3 State Diagram (детализация PTT в AI-VOX фазе)

```mermaid
stateDiagram-v2
    [*] --> Session_Active : KWS cmd 1–3 →<br/>сессия активна<br/>Beep + Enable Output = ON<br/>PTT = OFF

    Session_Active --> Speaking : VAD Start<br/>(голос обнаружен)

    Speaking --> PTT_ON : PTT ON → Output Start

    state "Передача" as TX {
        PTT_ON --> Transmitting : Выходной сигнал
        Transmitting --> PTT_ON : Новые блоки данных
    }

    PTT_ON --> PTT_OFF_Pause : VAD Stop<br/>(пауза в речи)<br/>PTT OFF
    PTT_OFF_Pause --> Speaking : VAD Start<br/>(речь возобновилась)

    Session_Active --> Disconnect : KWS: команда 4<br/>(disconnect до начала речи)
    Session_Active --> Timeout : Timeout (30 сек)
    PTT_ON --> Disconnect : KWS: команда 4<br/>(disconnect во время передачи)
    PTT_OFF_Pause --> Disconnect : KWS: команда 4<br/>(disconnect в паузе)
    PTT_OFF_Pause --> Timeout : Timeout (30 сек тишины)

    Timeout --> [*] : Beep (timeout) → Enable Output = OFF<br/>полный сброс → Idle
    Disconnect --> [*] : PTT OFF → Enable Output = OFF<br/>полный сброс → Idle

    note right of Session_Active
        Сессия активна, но
        передачи нет — PTT OFF.
        Ожидание VAD Start.
        Timeout = 30 сек.
    end note

    note right of TX
        KWS работает параллельно
        во время всей сессии.
    end note
```

### 2.4 Timing / Signal Flow — Полный цикл (connect → transmit → disconnect)

```mermaid
sequenceDiagram
    participant Mic as Input Buffer
    participant VAD as VAD Engine
    participant KWS as KWS<br/>(Keyword Spotting)
    participant Out as Output Control
    participant PTT as PTT
    participant Radio as Radio TX
    participant MCU3 as MCU3<br/>(Display)

    Note over Mic,MCU3: ══ ФАЗА 1: ОЖИДАНИЕ СТАРТОВОЙ КОМАНДЫ ══

    Mic->>VAD: Входной сигнал (блоки)
    VAD->>VAD: VAD Start (голос обнаружен)
    VAD->>KWS: Аудио → KWS буфер

    Note over KWS: KWS буфер накапливается...<br/>Анализ на команды 1–3

    KWS->>KWS: ✅ Стартовая команда обнаружена<br/>(cmd 1, 2 или 3)

    Note over Mic,MCU3: ══ ФАЗА 2: СЕССИЯ АКТИВНА (PTT OFF, ожидание VAD) ══

    KWS->>Out: Команда принята → Beep подтверждение
    Out->>Out: Enable Output = ON, PTT = OFF
    Out->>MCU3: CMD_DETECTED(cmd_id) → дисплей: CHANNEL ALPHA/BRAVO/DELTA
    Out->>MCU3: SESSION_ACTIVE → дисплей: сессия активна

    Note over Out: Сессия активна. PTT = OFF.<br/>Человек может молчать.<br/>RX активен (half-duplex).<br/>PTT включится по VAD Start.<br/>Session Timeout timer = 30 сек.

    VAD->>VAD: ... пауза (человек молчит, слышит RX) ...

    Note over Mic,MCU3: ══ ФАЗА 3: ПЕРЕДАЧА (AI-VOX РЕЖИМ) ══

    VAD->>VAD: VAD Start (голос обнаружен)
    VAD->>Out: VAD = ON
    Out->>PTT: PTT ON
    PTT->>Radio: Output Start
    Out->>MCU3: VAD_ON → дисплей: TX : AI-PRO, VOICE, PTT AUTO

    loop Передача речи (AI-VOX)
        Mic->>Out: Выходные блоки (O1, O2, O3...)
        Out->>Radio: Выходной сигнал
        Mic->>KWS: Параллельный KWS мониторинг
    end

    Note over VAD: VAD Stop (пауза в речи)
    VAD->>Out: VAD = OFF
    Out->>PTT: PTT OFF (пауза, сессия не закрывается)
    Out->>MCU3: VAD_OFF → дисплей: RX : AI-PRO + CHANNEL (сессия жива)

    Note over Out: RX активен (half-duplex).<br/>Session Timeout перезапускается.

    Note over VAD: VAD Start (речь возобновилась)
    VAD->>Out: VAD = ON
    Out->>PTT: PTT ON
    PTT->>Radio: Output Start (продолжение)
    Out->>MCU3: VAD_ON → дисплей: TX : AI-PRO

    loop Продолжение передачи
        Mic->>Out: Выходные блоки (O4, O5...)
        Out->>Radio: Выходной сигнал
        Mic->>KWS: KWS мониторинг продолжается
    end

    Note over Mic,MCU3: ══ ФАЗА 4: ОТКЛЮЧЕНИЕ ══

    KWS->>KWS: ⛔ Команда 4 обнаружена<br/>(disconnect)
    KWS->>Out: Disconnect signal → Beep (disconnect tone)
    Out->>PTT: PTT OFF
    PTT->>Radio: Output Finish
    Out->>Out: Enable Output = OFF
    Out->>MCU3: SESSION_CLOSED → дисплей: DISCONNECT (1 сек) → RX

    Note over Mic,MCU3: ══ ВОЗВРАТ В НАЧАЛЬНОЕ СОСТОЯНИЕ ══

    Note over KWS: Полный сброс.<br/>Ожидание новой стартовой команды (1–3).<br/>RX активен (half-duplex).
```

### 2.5 Ключевые параметры AI-VOX PRO

| Параметр | Значение | Описание |
|---|---|---|
| VAD Start Marker | set to 4 (единый, задаётся однократно) | Порог обнаружения начала речи |
| VAD Stop Marker | set to 3 (единый, задаётся однократно) | Порог обнаружения окончания речи |
| KWS команды 1–3 | Start (стартовые) | Инициируют переход в AI-VOX режим |
| KWS команда 4 | disconnect (Stop) | Выход из AI-VOX → полный сброс |
| Session Timeout | 30 сек (конфигурируемый) | Если в активной сессии нет VAD Start дольше таймаута → beep + автозакрытие сессии |
| SR | **Не используется** | — |
| Enable Output | ON после KWS cmd 1–3 | Разрешение выхода по стартовой команде, PTT остаётся OFF |
| PTT в сессии | Управляется VAD | PTT ON по VAD Start, PTT OFF по VAD Stop |
| KWS в сессии | Параллельный мониторинг | Только команда 4 (disconnect) |
| Сессионность | Да | connect (cmd 1–3) / disconnect (cmd 4) / timeout |

---

## 3. AI-VOX PRO + SR

> ⚠️ **Переименован в v1.2** (ранее «AI-VOX PRO» в v1.1)

**Описание:** Командный режим с двухфакторной авторизацией (KWS + SR). Работает аналогично AI-VOX PRO (режим 2), но стартовая команда дополнительно проверяется через SR модель — верификация говорящего как «второй ключ». SR работает **только один раз** на буфер стартовой команды — не на весь речевой поток (экономия ресурсов). После авторизации SR выключается, система переходит в AI-VOX режим с параллельным KWS мониторингом.

### 3.0 Принцип работы (обзор)

```mermaid
graph TD
    A[VAD обнаружил голос] --> B[KWS буфер накапливается]
    B --> C{KWS: какая команда?}
    C -->|Команда 1–3<br/>стартовые| D["Буфер → SR модель<br/>(второй ключ)"]
    C -->|Нет команды /<br/>неизвестная| B
    D --> E{SR результат?}
    E -->|SR = true<br/>говорящий подтверждён| F["Сессия активна<br/>(Enable Output = ON, PTT OFF)<br/>SR выключен"]
    E -->|SR = false| G[Отказ → возврат в ожидание KWS]
    F --> F2["VAD Start → PTT ON<br/>(передача)"]
    F2 --> F3["VAD Stop → PTT OFF<br/>(пауза, сессия не закрывается)"]
    F3 --> F2
    F2 --> H["KWS мониторинг продолжается<br/>(SR выключен)"]
    H --> I{KWS: команда 4?}
    I -->|Команда 4<br/>disconnect| J[Возврат в начальное состояние]
    I -->|Нет команды 4| H
    J --> A

    style A fill:#2196F3,color:#fff
    style C fill:#FF9800,color:#fff
    style D fill:#9C27B0,color:#fff
    style E fill:#9C27B0,color:#fff
    style F fill:#4CAF50,color:#fff
    style H fill:#FF9800,color:#fff
    style I fill:#FF9800,color:#fff
    style J fill:#f44336,color:#fff
```

### 3.1 Отличие от AI-VOX PRO (режим 2)

```mermaid
graph LR
    subgraph "AI-VOX PRO (режим 2)"
        A1["KWS cmd 1–3"] --> A2["→ сразу AI-VOX"]
    end

    subgraph "AI-VOX PRO + SR (режим 3)"
        B1["KWS cmd 1–3"] --> B2["→ SR verify<br/>(тот же буфер)"]
        B2 --> B3{"SR=true?"}
        B3 -->|Да| B4["→ AI-VOX"]
        B3 -->|Нет| B5["→ отказ"]
    end

    style A1 fill:#FF9800,color:#fff
    style A2 fill:#4CAF50,color:#fff
    style B1 fill:#FF9800,color:#fff
    style B2 fill:#9C27B0,color:#fff
    style B3 fill:#9C27B0,color:#fff
    style B4 fill:#4CAF50,color:#fff
    style B5 fill:#f44336,color:#fff
```

Единственное отличие: между детекцией стартовой команды и переходом в AI-VOX добавляется однократная SR верификация. Всё остальное (KWS мониторинг, disconnect, VAD cycling) — идентично режиму 2.

### 3.2 State Diagram (основной)

```mermaid
stateDiagram-v2
    [*] --> Idle : Начальное состояние

    state "Фаза авторизации" as AuthPhase {
        Idle --> VAD_Active : VAD Start<br/>(голос обнаружен)
        VAD_Active --> KWS_Buffering : Накопление KWS буфера

        KWS_Buffering --> KWS_Cmd_Detected : KWS: команда 1, 2 или 3<br/>(стартовая команда)
        KWS_Buffering --> KWS_Buffering : Нет команды /<br/>неизвестная → продолжаем
        KWS_Buffering --> Idle : VAD Stop<br/>(голос пропал без команды)

        KWS_Cmd_Detected --> SR_Verify : Тот же буфер → SR модель<br/>(верификация говорящего)

        SR_Verify --> SR_Confirmed : SR = true<br/>(говорящий подтверждён)
        SR_Verify --> SR_Rejected : SR = false<br/>(говорящий не подтверждён)

        SR_Rejected --> Idle : Отказ → возврат<br/>в ожидание
    }

    state "Фаза передачи (AI-VOX режим)" as TxPhase {
        SR_Confirmed --> Session_Active : Переход в AI-VOX<br/>SR выключается<br/>Beep + Enable Output = ON<br/>PTT = OFF

        Session_Active --> AIVOX_Transmitting : VAD Start<br/>(голос обнаружен)<br/>PTT ON → Output Start

        state KWS_Monitor <<choice>>
        AIVOX_Transmitting --> KWS_Monitor : KWS мониторинг<br/>параллельно (SR OFF)

        KWS_Monitor --> AIVOX_Transmitting : Нет команды 4<br/>→ продолжаем передачу
        KWS_Monitor --> Disconnect : KWS: команда 4<br/>(disconnect)
    }

    state "VAD cycling в сессии" as VADCycle {
        AIVOX_Transmitting --> VAD_Pause : VAD Stop<br/>(пауза в речи)<br/>PTT OFF
        VAD_Pause --> AIVOX_Transmitting : VAD Start<br/>(речь возобновилась)<br/>PTT ON
    }

    Session_Active --> Disconnect : KWS: команда 4<br/>(disconnect до начала речи)
    Session_Active --> Timeout : Session Timeout (30 сек)<br/>нет VAD Start
    VAD_Pause --> Disconnect : KWS: команда 4<br/>(disconnect в паузе)
    VAD_Pause --> Timeout : Session Timeout (30 сек)<br/>тишина дольше таймаута

    Timeout --> Idle : Beep (timeout tone)<br/>Enable Output = OFF<br/>Полный сброс
    Disconnect --> Idle : PTT OFF<br/>Enable Output = OFF<br/>Полный сброс

    note right of SR_Verify
        SR работает ОДИН РАЗ — только
        для верификации стартовой
        команды. Не на весь речевой
        поток (экономия ресурсов).
        SR = "второй ключ".
    end note

    note right of Session_Active
        СЕССИЯ АКТИВНА, но PTT = OFF.
        Человек может молчать
        секунды после авторизации.
        PTT включится только
        по VAD Start.
        Если тишина > 30 сек →
        таймаут, сессия закрывается.
    end note

    note right of KWS_Monitor
        В активной сессии KWS
        продолжает работать
        параллельно, но только
        слушает команду 4
        (disconnect).
        SR выключен.
    end note

    note left of VAD_Pause
        В AI-VOX режиме VAD
        управляет PTT ON/OFF
        для пауз в речи, но
        НЕ выходит из сессии.
    end note
```

### 3.3 State Diagram (детализация PTT в AI-VOX фазе)

```mermaid
stateDiagram-v2
    [*] --> Session_Active : SR confirmed →<br/>сессия активна<br/>Beep + Enable Output = ON<br/>PTT = OFF (SR выключен)

    Session_Active --> Speaking : VAD Start<br/>(голос обнаружен)

    Speaking --> PTT_ON : PTT ON → Output Start

    state "Передача" as TX {
        PTT_ON --> Transmitting : Выходной сигнал
        Transmitting --> PTT_ON : Новые блоки данных
    }

    PTT_ON --> PTT_OFF_Pause : VAD Stop<br/>(пауза в речи)<br/>PTT OFF
    PTT_OFF_Pause --> Speaking : VAD Start<br/>(речь возобновилась)

    Session_Active --> Disconnect : KWS: команда 4<br/>(disconnect до начала речи)
    Session_Active --> Timeout : Timeout (30 сек)
    PTT_ON --> Disconnect : KWS: команда 4<br/>(disconnect во время передачи)
    PTT_OFF_Pause --> Disconnect : KWS: команда 4<br/>(disconnect в паузе)
    PTT_OFF_Pause --> Timeout : Timeout (30 сек тишины)

    Timeout --> [*] : Beep (timeout) → Enable Output = OFF<br/>полный сброс → Idle (ожидание KWS+SR)
    Disconnect --> [*] : PTT OFF → Enable Output = OFF<br/>полный сброс → Idle (ожидание KWS+SR)

    note right of Session_Active
        Сессия активна, но
        передачи нет — PTT OFF.
        Ожидание VAD Start.
        Timeout = 30 сек.
    end note

    note right of TX
        KWS работает параллельно
        во время всей сессии.
        SR выключен.
    end note
```

### 3.4 Timing / Signal Flow — Полный цикл (connect → transmit → disconnect)

```mermaid
sequenceDiagram
    participant Mic as Input Buffer
    participant VAD as VAD Engine
    participant KWS as KWS<br/>(Keyword Spotting)
    participant SR as SR Model<br/>(одноразовая)
    participant Out as Output Control
    participant PTT as PTT
    participant Radio as Radio TX
    participant MCU3 as MCU3<br/>(Display)

    Note over Mic,MCU3: ══ ФАЗА 1: АВТОРИЗАЦИЯ (KWS + SR) ══

    Mic->>VAD: Входной сигнал (блоки)
    VAD->>VAD: VAD Start (голос обнаружен)
    VAD->>KWS: Аудио → KWS буфер

    Note over KWS: KWS буфер накапливается...<br/>Анализ на команды 1–3

    KWS->>KWS: ✅ Команда обнаружена<br/>(cmd 1, 2 или 3)
    Out->>MCU3: CMD_DETECTED(cmd_id) → Beep подтверждение KWS
    KWS->>SR: Тот же буфер → SR модель<br/>(8000 сэмплов = 500 мс, looping если короче)

    Note over SR: SR верификация говорящего...<br/>(однократная проверка, ~30–60 мс)

    SR->>SR: SR = true ✅<br/>(говорящий подтверждён)
    SR->>SR: SR выключается (больше не используется)

    Note over Mic,MCU3: ══ ФАЗА 2: СЕССИЯ АКТИВНА (PTT OFF, ожидание VAD) ══

    SR->>Out: Авторизация пройдена → Beep подтверждение SR
    Out->>Out: Enable Output = ON, PTT = OFF
    Out->>MCU3: SR_CONFIRMED → дисплей: SPEAKER KNOWN
    Out->>MCU3: SESSION_ACTIVE → дисплей: CHANNEL + сессия активна

    Note over Out: Сессия активна. PTT = OFF.<br/>Человек может молчать.<br/>RX активен (half-duplex).<br/>PTT включится по VAD Start.<br/>Session Timeout timer = 30 сек.

    VAD->>VAD: ... пауза (человек молчит, слышит RX) ...

    Note over Mic,MCU3: ══ ФАЗА 3: ПЕРЕДАЧА (AI-VOX РЕЖИМ, SR OFF) ══

    VAD->>VAD: VAD Start (голос обнаружен)
    VAD->>Out: VAD = ON
    Out->>PTT: PTT ON
    PTT->>Radio: Output Start
    Out->>MCU3: VAD_ON → дисплей: TX : AI-PRO+SR, VOICE, PTT AUTO, SPEAKER KNOWN

    loop Передача речи (AI-VOX)
        Mic->>Out: Выходные блоки (O1, O2, O3...)
        Out->>Radio: Выходной сигнал
        Mic->>KWS: Параллельный KWS мониторинг (SR OFF)
    end

    Note over VAD: VAD Stop (пауза в речи)
    VAD->>Out: VAD = OFF
    Out->>PTT: PTT OFF (пауза, сессия не закрывается)
    Out->>MCU3: VAD_OFF → дисплей: RX : AI-PRO+SR + CHANNEL + SPEAKER KNOWN

    Note over Out: RX активен (half-duplex).<br/>Session Timeout перезапускается.

    Note over VAD: VAD Start (речь возобновилась)
    VAD->>Out: VAD = ON
    Out->>PTT: PTT ON
    PTT->>Radio: Output Start (продолжение)
    Out->>MCU3: VAD_ON → дисплей: TX : AI-PRO+SR

    loop Продолжение передачи
        Mic->>Out: Выходные блоки (O4, O5...)
        Out->>Radio: Выходной сигнал
        Mic->>KWS: KWS мониторинг продолжается
    end

    Note over Mic,MCU3: ══ ФАЗА 4: ОТКЛЮЧЕНИЕ ══

    KWS->>KWS: ⛔ Команда 4 обнаружена<br/>(disconnect)
    KWS->>Out: Disconnect signal → Beep (disconnect tone)
    Out->>PTT: PTT OFF
    PTT->>Radio: Output Finish
    Out->>Out: Enable Output = OFF
    Out->>MCU3: SESSION_CLOSED → дисплей: DISCONNECT (1 сек) → RX

    Note over Mic,MCU3: ══ ВОЗВРАТ В НАЧАЛЬНОЕ СОСТОЯНИЕ ══

    Note over KWS,SR: Полный сброс.<br/>Ожидание новой стартовой команды (1–3) + SR верификация.<br/>RX активен (half-duplex).
```

### 3.5 Timing / Signal Flow — Сценарий отказа SR

```mermaid
sequenceDiagram
    participant Mic as Input Buffer
    participant VAD as VAD Engine
    participant KWS as KWS
    participant SR as SR Model
    participant Out as Output Control
    participant MCU3 as MCU3<br/>(Display)

    Mic->>VAD: Входной сигнал
    VAD->>VAD: VAD Start
    VAD->>KWS: Аудио → KWS буфер

    KWS->>KWS: ✅ Команда 2 обнаружена
    Out->>MCU3: CMD_DETECTED(2) → Beep подтверждение KWS
    KWS->>SR: Буфер → SR модель<br/>(8000 сэмплов = 500 мс, looping если короче)

    SR->>SR: SR = false ❌<br/>(неизвестный говорящий)

    Note over SR,Out: Авторизация отклонена!<br/>Передача НЕ разрешена.

    SR->>Out: SR rejected → Beep (reject tone)
    Out->>Out: Enable Output остаётся OFF
    Out->>MCU3: SR_REJECTED → дисплей: SPEAKER UNKNOWN (1 сек) → RX
    
    Note over KWS: Возврат в начальное состояние.<br/>Ожидание новой стартовой команды + SR.<br/>RX активен (half-duplex).
```

### 3.6 Ключевые параметры AI-VOX PRO + SR

| Параметр | Значение | Описание |
|---|---|---|
| VAD Start Marker | set to 4 (единый, задаётся однократно) | Порог обнаружения начала речи |
| VAD Stop Marker | set to 3 (единый, задаётся однократно) | Порог обнаружения окончания речи |
| KWS команды 1–3 | Start (стартовые) | Инициируют авторизацию (KWS → SR) |
| KWS команда 4 | disconnect (Stop) | Выход из AI-VOX → полный сброс |
| Session Timeout | 30 сек (конфигурируемый) | Если в активной сессии нет VAD Start дольше таймаута → beep + автозакрытие сессии |
| SR модель | Однократная | Работает ОДИН РАЗ на буфер стартовой команды. Не на речевой поток. |
| SR буфер | = KWS буфер | Тот же буфер, что накоплен для KWS, передаётся в SR |
| SR результат | true / false | true → AI-VOX, false → отказ → начальное состояние |
| SR в AI-VOX фазе | **Выключен** | Экономия ресурсов, SR не нужен после авторизации |
| Enable Output | ON после SR=true | Разрешение выхода только после двухфакторной проверки, PTT остаётся OFF |
| PTT в сессии | Управляется VAD | PTT ON по VAD Start, PTT OFF по VAD Stop |
| KWS в сессии | Параллельный мониторинг | Только команда 4 (disconnect), SR выключен |

### 3.7 Логика переходов (сводка)

```mermaid
graph LR
    subgraph "Фаза авторизации"
        A["VAD Start"] --> B["KWS буфер"]
        B --> C{"Команда<br/>1–3?"}
        C -->|Да| D["SR verify<br/>(тот же буфер)"]
        C -->|Нет| B
        D --> E{"SR=true?"}
        E -->|Нет| A
    end

    subgraph "Фаза передачи"
        E -->|Да| F["AI-VOX режим<br/>PTT по VAD<br/>SR OFF"]
        F --> G["KWS мониторинг<br/>(SR OFF)"]
        G --> H{"Команда 4?"}
        H -->|Нет| F
    end

    H -->|disconnect| A

    style A fill:#2196F3,color:#fff
    style C fill:#FF9800,color:#fff
    style D fill:#9C27B0,color:#fff
    style E fill:#9C27B0,color:#fff
    style F fill:#4CAF50,color:#fff
    style G fill:#FF9800,color:#fff
    style H fill:#f44336,color:#fff
```

---

## Сравнительная таблица режимов

```mermaid
graph TD
    subgraph "AI-VOX"
        A1[VAD] --> A2[Output + PTT]
    end

    subgraph "AI-VOX PRO"
        B1[VAD] --> B2[KWS буфер]
        B2 --> B3["KWS cmd 1–3"]
        B3 --> B4["AI-VOX режим<br/>+ KWS мониторинг"]
        B4 --> B5["KWS cmd 4<br/>(disconnect)"]
        B5 --> B1
    end

    subgraph "AI-VOX PRO + SR"
        C1[VAD] --> C2[KWS буфер]
        C2 --> C3["KWS cmd 1–3"]
        C3 --> C4["SR verify<br/>(однократно)"]
        C4 --> C5["AI-VOX режим<br/>+ KWS мониторинг<br/>SR OFF"]
        C5 --> C6["KWS cmd 4<br/>(disconnect)"]
        C6 --> C1
    end

    style A1 fill:#4CAF50,color:#fff
    style A2 fill:#4CAF50,color:#fff
    style B1 fill:#FF9800,color:#fff
    style B2 fill:#FF9800,color:#fff
    style B3 fill:#FF9800,color:#fff
    style B4 fill:#4CAF50,color:#fff
    style B5 fill:#f44336,color:#fff
    style C1 fill:#f44336,color:#fff
    style C2 fill:#f44336,color:#fff
    style C3 fill:#f44336,color:#fff
    style C4 fill:#9C27B0,color:#fff
    style C5 fill:#4CAF50,color:#fff
    style C6 fill:#f44336,color:#fff
```

| Характеристика | AI-VOX | AI-VOX PRO | AI-VOX PRO + SR |
|---|---|---|---|
| VAD | ✅ | ✅ | ✅ |
| KWS (Keyword Spotting) | ❌ | ✅ (команды 1–4) | ✅ (команды 1–4) |
| Speaker Recognition | ❌ | ❌ | ✅ (однократный, «второй ключ») |
| Авторизация | Нет | KWS cmd 1–3 | KWS cmd 1–3 → SR (один раз) |
| Деавторизация | — | KWS cmd 4 (disconnect) | KWS cmd 4 (disconnect) |
| VAD Start Marker | 4 | 4 | 4 |
| VAD Stop Marker | 3 | 3 | 3 |
| SR активен во время TX | — | — | ❌ (выключен после авторизации) |
| KWS активен во время TX | — | ✅ (мониторинг cmd 4) | ✅ (мониторинг cmd 4) |
| Задержка до PTT | Минимальная | Средняя (KWS detect) | Средняя+ (KWS detect + SR verify) |
| Уровень защиты | Базовый | Средний (только KWS) | Максимальный (двухфакторный) |
| Ложные срабатывания | Возможны | Снижены | Минимальны |
| Сессионность | Нет | Да (connect / disconnect / timeout) | Да (connect / disconnect / timeout) |
| Нагрузка на ресурсы | Минимальная | Средняя (VAD + KWS) | Средняя (SR только при авторизации) |

---

## Параметры маркеров и буферов

### Входной буфер (Input Signal and Buffer)

Все режимы используют **кольцевой буфер** с рабочими чанками по 256 сэмплов (16 мс при 16 kHz).

**AI-VOX — кольцевой буфер MCU1:**
- Рабочий чанк: 256 сэмплов = 16 мс (VAD-сеть: вход 128 сэмплов, анализ каждого 2-го блока)
- Размер: 10 чанков × 16 мс = 160 мс (2560 сэмплов, 5120 байт)
- Тип: кольцевой (11-й чанк вытесняет 1-й)
- Назначение: хранение pre-roll (начало фразы до момента принятия решения VAD)
- При VAD Start: выдача начинается с позиции `Current Position − VAD Start Marker` чанков (ретроспективно из буфера)
- Минимальный размер буфера: ≥ VAD Start Marker чанков (4 × 16 мс = 64 мс)

**AI-VOX PRO / AI-VOX PRO + SR — KWS буфер на MCU2 (Sensory):**
- MCU1 передаёт аудио по UART **только при VAD=ON** (+ pre-roll 4 чанка). При VAD=OFF аудио не передаётся — экономия UART bandwidth
- MCU2 пишет всё полученное в Sensory (перепаковка 256→240 сэмплов)
- VAD=OFF → MCU1 прекращает передачу, шлёт `EVT_VAD(flag=0)`. MCU2 отдаёт Sensory остаток на анализ
- Trigger не найден → Sensory restart, буфер сброшен, ожидание следующего VAD=ON
- Trigger найден → Session Active
- Sensory brick: **240 сэмплов = 15 мс** — порция на один вызов `SensoryProcessData()`
- Внутренний аудио-буфер Sensory: **630 мс = 10080 сэмплов = ~20 кБ**
- Backoff (ретроспективный просмотр): **270 мс** (для endpoint detection)
- InputBuffer MCU2: **32000 сэмплов = 2 секунды** (кольцевой, float)
- MCU2 перепаковывает VAD-чанки MCU1 (256 сэмплов / 16 мс) в Sensory brick'и (240 сэмплов / 15 мс) — это **не 1:1**, требуется промежуточный буфер
- В режиме PRO + SR: тот же Sensory буфер (с endpoint detection) передаётся на SR модель

### Параметры VAD на MCU1 (ЕДИНЫЕ для всех режимов)

> **Архитектурное решение:** VAD параметры задаются на MCU1 **однократно** при инициализации и **не меняются** при переключении режимов на MCU2. MCU1 не знает о режимах AI-VOX / PRO / PRO+SR — это логика MCU2. Конкретные значения подбираются по результатам тестов и фиксируются в конфигурации.

| Параметр | Значение | Примечание |
|----------|----------|------------|
| VAD Start Marker | 4 (подбирается тестами) | Единый для всех режимов |
| VAD Stop Marker | 3 (подбирается тестами) | Единый для всех режимов |
| VAD chunk | 256 сэмплов = 16 мс | Фиксированный, привязан к VAD-сети |
| Pre-roll буфер | 10 чанков = 160 мс | Кольцевой, MCU1 |

### Маркеры (Markers)

| Маркер | Описание | AI-VOX | AI-VOX PRO | AI-VOX PRO + SR |
|---|---|---|---|---|
| VAD Start Marker | Начало обнаруженной речи | ✅ | ✅ | ✅ |
| VAD Stop Marker | Конец обнаруженной речи | ✅ | ✅ (в AI-VOX фазе управляет PTT) | ✅ (в AI-VOX фазе управляет PTT) |
| Output Start Marker | Начало выходного блока | Привязан к VAD Start | Привязан к KWS cmd 1–3 | Привязан к SR=true |
| Output Finish Marker | Конец выходного блока | Привязан к VAD Stop | Привязан к KWS cmd 4 | Привязан к KWS cmd 4 |
| KWS Command Marker | Обнаруженная команда | — | cmd 1–3 (start), cmd 4 (disconnect) | cmd 1–3 (start), cmd 4 (disconnect) |
| SR Start Marker | Начало SR обработки | — | — | Позиция KWS буфера (однократно) |

### Буфер KWS (AI-VOX PRO и AI-VOX PRO + SR)

В режимах AI-VOX PRO и AI-VOX PRO + SR буфер используется следующим образом:

**AI-VOX PRO:**
1. **Этап KWS:** Буфер накапливается после VAD Start. KWS анализирует поток на наличие команд 1–3.
2. **Результат:** Команда обнаружена → сразу переход в AI-VOX режим.

**AI-VOX PRO + SR (дополнительный этап):**
1. **Этап KWS:** Буфер накапливается после VAD Start. KWS анализирует поток на наличие команд 1–3.
2. **Этап SR:** При обнаружении стартовой команды **тот же буфер** (без повторного накопления) передаётся в SR модель для верификации говорящего.
3. **Результат:** SR=true → авторизация, SR выключается. SR=false → сброс, повторное ожидание.

> **Ключевое ограничение по ресурсам:** SR не работает на весь речевой поток — только однократно на буфер стартовой команды. Это принципиальное архитектурное решение для экономии вычислительных ресурсов MCU2.

### Выходной сигнал (Output Signal)

- **AI-VOX:** выходные блоки O1, O2, O3... передаются при VAD Start → VAD Stop
- **AI-VOX PRO:** передача начинается после KWS cmd 1–3, прекращается по KWS cmd 4
- **AI-VOX PRO + SR:** передача начинается после KWS cmd 1–3 + SR=true, прекращается по KWS cmd 4
- Во всех режимах: между передачами (PTT OFF → PTT ON) выходной сигнал отсутствует

---

## Changelog

| Версия | Дата | Изменения |
|---|---|---|
| 1.0 | 2025-02-07 | Первоначальная конвертация из PDF. Три режима: AI-VOX, AI-VOX+SR, AI-VOX PRO (wake word) |
| 1.1 | 2025-02-07 | Переработан режим 3: новая логика KWS→SR(однократный)→AI-VOX с командами 1–4 |
| 1.2 | 2025-02-07 | Исправлена структура режимов: режим 2 = AI-VOX PRO (VAD+KWS, без SR), режим 3 = AI-VOX PRO + SR (KWS + однократная SR авторизация). SR работает только на авторизацию, не на речевой поток. |
| 1.3 | 2025-02-07 | Унификация версии в рамках документ-пакета. Уточнение: «MCU» → «MCU2» в контексте ресурсов. Кросс-ссылки синхронизированы с Architecture v1.3.6 и UART Protocol v1.3.6. |
| 1.3.2 | 2025-02-08 | Уточнены параметры VAD и KWS буферов по данным из Keln кода. VAD рабочий чанк = 256 сэмплов (16 мс). KWS Sensory: brick = 240 (15 мс), буфер = 630 мс. Добавлена механика кольцевого буфера для AI-VOX. |
| 1.3.3 | 2025-02-08 | **Критичное исправление:** PTT в AI-VOX PRO и PRO+SR. После KWS cmd (или SR=true) сессия активируется (Enable Output=ON), но PTT остаётся OFF. PTT ON — только по VAD Start. Добавлено состояние Session_Active во все state/timing диаграммы. Disconnect возможен из любого состояния сессии (Session_Active, PTT ON, VAD Pause). |
| 1.3.6 | 2026-02-12 | **MCU2→MCU3 status messages:** Добавлен participant MCU3 (Display) во все sequence diagrams. Добавлены сообщения: CMD_DETECTED, SESSION_ACTIVE, SESSION_CLOSED, SR_CONFIRMED, SR_REJECTED, VAD_ON, VAD_OFF — с описанием реакции дисплея. **Naming:** Исправлены неканонические наименования («AI-VOX KWS» → «AI-VOX PRO», «AI-VOX + SR» оставлен только в историческом контексте). **SR:** Encoder input = 8000 сэмплов (500 мс), looping если короче. **Half-duplex:** Добавлен RX path (PTT OFF → приём активен) во все sequence diagrams. Cross-references обновлены на v1.3.6. |

---

> **Примечания к документу:**
> - State diagrams являются логической интерпретацией требований к системе
> - Режим 1 (AI-VOX) не изменён с v1.0
> - Режим 2 (AI-VOX PRO) — новый с v1.2, заменяет старый «AI-VOX + SR»
> - Режим 3 (AI-VOX PRO + SR) — переименован и уточнён с v1.1
> - Связанные документы: `01_2026_NC_architecture_v1.3.6.md`, `UART_Protocol_Spec_v1.3.6.md`, `NeuroComm_UI_Spec_v1.0.md`
> - Для просмотра Mermaid диаграмм используйте редактор с поддержкой Mermaid (VS Code, GitHub, и др.)
