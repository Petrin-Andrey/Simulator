#define _CRT_SECURE_NO_WARNINGS
#include "raylib.h"
#include "rlgl.h"
#include <algorithm>
#include <string>
#include <cmath>
#include <cstdio>
#include <cstdarg>
#include <cstring>

extern "C" __declspec(dllimport) void* __stdcall ShellExecuteA(void*, const char*, const char*, const char*, const char*, int);

// ---------------------------------------------------------------------------
// Глобальные константы
// ---------------------------------------------------------------------------

// Состояния игры
enum GameState { STATE_LOADING, STATE_MENU, STATE_GAME, STATE_RULES, STATE_RESULTS };
static GameState currentState = STATE_LOADING;
static float loadingProgress = 0.0f;
static float loadingTimer = 0.0f;
static int menuSelectedOption = 0; // 0 = Новая игра, 1 = Продолжить (если доступно), 2 = Выход
static bool gameStarted = false; // Флаг: была ли начата игра
static constexpr int   SCREEN_W = 1900;
static constexpr int   SCREEN_H = 1000;

// Результаты игры
enum GameResult { RESULT_NONE, RESULT_WIN, RESULT_LOSE };
static GameResult gameResult = RESULT_NONE;
static float shiftDuration = 5.0f * 60.0f; // 5 минут в секундах
static int resultMenuOption = 0; // 0 = Меню, 1 = Сохранить

static Font cyrillicFont = { 0 };
static Texture2D loadingScreenTexture = { 0 };
static Texture2D menuBackgroundTexture = { 0 };
static Texture2D rulesBackgroundTexture = { 0 };
static Texture2D gameOverTexture = { 0 };
static Texture2D winTexture = { 0 };
static constexpr float PI_F = 3.14159265f;

// Сжатие времени: 1 реальная секунда = TIME_SCALE игровых секунд
// При TIME_SCALE = 30: полные игровые сутки проходят за 48 реальных минут
static constexpr float TIME_SCALE = 30.0f;

// ---------- Реактор --------------------------------------------------------
static constexpr float REACTOR_MIN = 1000.0f; // МВт
static constexpr float REACTOR_MAX = 2000.0f; // МВт
static constexpr float REACTOR_RATE = 80.0f; // МВт / реальная секунда (лимит изменения)

// ---------- Частота сети -------------------------------------------------
static constexpr float FREQ_NOM = 50.0f;  // Гц
static constexpr float FREQ_MIN = 45.0f;
static constexpr float FREQ_MAX = 55.0f;
static constexpr float H_INERTIA = 5.0f; // секунды (уравнение колебаний)
static constexpr float S_NOM = 2000.0f; // МВА

// ---------- Спрос ---------------------------------------------------------
static constexpr float DEMAND_BASE = 1450.0f; // МВт

// ---------- ГАЭС (гидроаккумулирующая электростанция) ---------------------
static constexpr float PHES_TURB_MAX = 180.0f;  // МВт при 100% воды
static constexpr float PHES_PUMP_MAX = 140.0f;  // МВт потребляется при закачке
static constexpr float PHES_EFF = 0.85f; // КПД цикла ГАЭС
static constexpr float PHES_CAP_MWH = 1000.0f;  // полная емкость хранилища в МВт*ч

// ---------- Экономика --------------------------------------------------------
static constexpr float ECON_FREQ_PENALTY = 400.0f;   // USD / Гц / игровая секунда
static constexpr float ECON_INIT_BUDGET = 100000.0f; // Начальный бюджет USD

// ---------- Освещение -------------------------------------------------------
// Для истинного попиксельного окружающего света требуется шейдер (LoadShader + CreateLight).
// AMBIENT_INTENSITY определяет проектное значение; сцена использует аддитивный проход
// для аппроксимации эффекта без использования шейдерного конвейера.
static constexpr float AMBIENT_INTENSITY = 0.3f;

// ---------- График частоты -----------------------------------------------
static constexpr int   FREQ_HIST_LEN = 200;   // количество отсчетов в кольцевом буфере
static constexpr float FREQ_GRAPH_MIN = 47.0f; // Гц, низ оси Y
static constexpr float FREQ_GRAPH_MAX = 53.0f; // Гц, верх оси Y

// ---------------------------------------------------------------------------
// Clamp helper
// ---------------------------------------------------------------------------
template<typename T>
inline T Clamp(T value, T lo, T hi)
{
    return std::max(lo, std::min(value, hi));
}

// ---------------------------------------------------------------------------
// MakeVector3 — вспомогательная функция (в Raylib нет встроенной функции-фабрики)
// ---------------------------------------------------------------------------
inline Vector3 MakeVector3(float x, float y, float z) { return { x, y, z }; }

// ---------------------------------------------------------------------------
// Button3D — физическая кнопка панели: 3D-куб + проецируемая метка на экране
// ---------------------------------------------------------------------------
struct Button3D
{
    Vector3     position;      // центр куба в мировом пространстве
    Vector3     size;          // (ширина X, высота Y, глубина Z)
    Color       color;         // базовый цвет
    Color       colorHover;    // цвет при наведении (= color + 50 на канал)
    const char* label;
    bool        isHovered;     // обновляется каждый кадр функцией CheckHover()
    float       hoverProgress; // 0..1, плавно отслеживает наведение (lerp)
    float       pressProgress; // 1..0, устанавливается в 1 при Press(), затухает со скоростью 5/с

    // Пускает луч мыши и проверяет пересечение с AABB кнопки.
    // Сохраняет результат в isHovered и возвращает его.
    bool CheckHover(Camera3D cam)
    {
        // Берем позицию мыши
        Ray ray = GetMouseRay(GetMousePosition(), cam);

        // Центрируем хитбокс по оси Y ( position.y - size.y / 2.0f )
        Vector3 boxMin = { position.x - size.x / 2.0f, position.y - size.y / 2.0f, position.z - size.z / 2.0f };
        Vector3 boxMax = { position.x + size.x / 2.0f, position.y + size.y / 2.0f, position.z + size.z / 2.0f };

        BoundingBox box = { boxMin, boxMax };

        RayCollision col = GetRayCollisionBox(ray, box);
        isHovered = col.hit;
        return isHovered;
    }

    // Активация анимации нажатия (вызывается при нажатии клавиши E или клике).
    void Press() { pressProgress = 1.0f; }

    // Обновление анимаций наведения и нажатия для каждого кадра.
    // Должно вызываться раз в кадр с реальным значением dt.
    void UpdateAnimation(float dt)
    {
        // hoverProgress плавно стремится к цели наведения
        const float hoverTarget = isHovered ? 1.0f : 0.0f;
        hoverProgress += (hoverTarget - hoverProgress) * dt * 8.0f;
        hoverProgress = Clamp(hoverProgress, 0.0f, 1.0f);

        // pressProgress затухает на 5 единиц/с до 0
        pressProgress -= dt * 5.0f;
        pressProgress = Clamp(pressProgress, 0.0f, 1.0f);
    }

    // Отрисовка кнопки:
    //   • размер при наведении : size *= 1 + hoverProgress * 0.05
    //   • смещение при нажатии : drawPos.y -= pressProgress * 0.05
    //   • пульсация (высота)   : +sin(t*6)*0.02 при hoverProgress > 0
    //   • цвет                 : colorHover при наведении
    void Draw() const
    {
        const float t = (float)GetTime();
        const Color dc = isHovered ? colorHover : color;

        // Равномерный масштаб по hoverProgress
        const float scale = 1.0f + hoverProgress * 0.05f;
        const float pulse = sinf(t * 6.0f) * 0.02f * hoverProgress; // затухает при потере наведения

        const float sx = size.x * scale;
        const float sy = (size.y + pulse) * scale;
        const float sz = size.z * scale;

        // Анимация нажатия: куб опускается пропорционально pressProgress
        const Vector3 drawPos = { position.x,
                                  position.y - pressProgress * 0.05f,
                                  position.z };

        DrawCube(drawPos, sx, sy, sz, dc);
        DrawCubeWires(drawPos, sx, sy, sz, { 255, 255, 255, 50 });

        if (hoverProgress > 0.01f)
        {
            const unsigned char wa = (unsigned char)(hoverProgress * 200.0f);
            DrawCubeWires(drawPos,
                sx + 0.05f, sy + 0.05f, sz + 0.05f,
                { 255, 255, 255, wa });
            BeginBlendMode(BLEND_ADDITIVE);
            DrawCube(drawPos,
                sx + 0.02f, sy + 0.02f, sz + 0.02f,
                { 255, 255, 255, (unsigned char)(hoverProgress * 18.0f) });
            EndBlendMode();
        }
    }
};

// Глобальные определения кнопок — используются в DrawScene3D и в 2D-проходе подписей.
// Кнопки расположены с увеличенным шагом по X для исключения перекрытий подписей.
// Нормальные пропорциональные размеры.
// Поля анимации мутируют каждый кадр.
// colorHover = color + 50 на RGB-канал (с ограничением в 255)
// Нулевые хвосты: isHovered=false, hoverProgress=0, pressProgress=0
static Button3D g_Buttons[] = {
   { { -2.6f, 0.6f,  -1.9f }, { 0.8f, 0.4f, 0.15f }, {  40, 200,  80, 255 }, {  90, 250, 130, 255 }, "РЕАКТОР+", false, 0.0f, 0.0f },
    { { -1.4f, 0.6f,   -1.9f }, { 0.8f, 0.4f, 0.15f }, { 210,  55,  55, 255 }, { 255, 105, 105, 255 }, "РЕАКТОР-", false, 0.0f, 0.0f },
    { { -0.2f, 0.6f,   -1.9f }, { 0.8f, 0.4f, 0.15f }, {  60, 120, 220, 255 }, { 110, 170, 255, 255 }, "ТУРБИНА" , false, 0.0f, 0.0f },
    { {  1.0f, 0.6f,   -1.9f }, { 0.8f, 0.4f, 0.15f }, { 220, 140,  40, 255 }, { 255, 190,  90, 255 }, "НАСОС"    , false, 0.0f, 0.0f },
    { { 0.5f, 1.7f, 4.85f }, { 0.12f, 0.15f, 0.12f }, { 180, 170, 150, 255 }, { 255, 220, 100, 255 }, "ПЕРЕЗАПУСК", false, 0.0f, 0.0f },
};
static const int g_ButtonCount = (int)(sizeof(g_Buttons) / sizeof(g_Buttons[0]));

// ---------------------------------------------------------------------------
// DemandModel — модель спроса: потребление города с утренним/вечерним пиками и ночным спадом
// ---------------------------------------------------------------------------
struct DemandModel
{
    float currentDemand; // МВт
    float factor;        // безразмерный множитель базового значения DEMAND_BASE

    void Init()
    {
        factor = 1.0f;
        currentDemand = DEMAND_BASE;
    }

    // Sinusoidal Hann-windowed bumps for peaks and valley
    static float ComputeFactor(float h)
    {
        float f = 1.0f;

        // Утренний пик: центр h=9, полуширина 2 ч -> коэффициент пика +0.25
        {
            float d = h - 9.0f;
            if (fabsf(d) < 2.0f)
                f += 0.25f * 0.5f * (1.0f + cosf(PI_F * d / 2.0f));
        }

        // Вечерний пик: центр h=19, полуширина 2 ч -> коэффициент пика +0.20
        {
            float d = h - 19.0f;
            if (fabsf(d) < 2.0f)
                f += 0.20f * 0.5f * (1.0f + cosf(PI_F * d / 2.0f));
        }

        // Ночной спад: центр h=3, полуширина 4 ч (переходит через полночь)
        // -> охватывает 23 ч .. 7 ч, коэффициент падает до 0.70
        {
            float hc = (h > 12.0f) ? h - 24.0f : h; // свертка в диапазон [-12, 12]
            float d = hc - 3.0f;
            if (fabsf(d) < 4.0f)
                f -= 0.30f * 0.5f * (1.0f + cosf(PI_F * d / 4.0f));
        }

        return Clamp(f, 0.70f, 1.25f);
    }

    void Update(float gameHour)
    {
        factor = ComputeFactor(gameHour);
        currentDemand = DEMAND_BASE * factor;
    }
};

// ---------------------------------------------------------------------------
// ReactorModel — инерционный отклик первого порядка, ограничение скорости изменения 80 МВт/с
// ---------------------------------------------------------------------------
struct ReactorModel
{
    float targetPower; // МВт — уставка игрока
    float actualPower; // МВт — фактическая мощность (отстает от уставки из-за лимита изменения)

    void Init()
    {
        targetPower = 1500.0f;
        actualPower = 1500.0f;
    }

    // dt в реальных секундах — оборудование реагирует с физической скоростью
    void Update(float dt)
    {
        float maxDelta = REACTOR_RATE * dt;
        float diff = Clamp(targetPower - actualPower, -maxDelta, maxDelta);
        actualPower = Clamp(actualPower + diff, REACTOR_MIN, REACTOR_MAX);
    }
};

// ---------------------------------------------------------------------------
// PHESModel — гидроаккумулирующая электростанция (ГАЭС)
// ---------------------------------------------------------------------------
enum class PHESMode { Idle = 0, Turbine, Pump };

struct PHESModel
{
    float    waterLevel;  // % [0, 100]
    PHESMode mode;
    float    outputPower; // МВт: >0 генерация (турбина), <0 потребление (насос)

    void Init()
    {
        waterLevel = 50.0f;
        mode = PHESMode::Idle;
        outputPower = 0.0f;
    }

    void SetMode(PHESMode m) { mode = m; }

    // gameDt в игровых секундах, чтобы водохранилище истощалось по шкале игрового времени
    void Update(float gameDt)
    {
        // Вспомогательные переменные: изменение уровня в % на каждый переданный МВт*ч
        // 100% == PHES_CAP_MWH МВт*ч -> 1% == PHES_CAP_MWH/100 МВт*ч
        // dLevel(%) = power_MW * gameDt_s / 3600 / (PHES_CAP_MWH/100)
        const float pctPerMWs = 100.0f / (PHES_CAP_MWH * 3600.0f);

        switch (mode)
        {
        case PHESMode::Turbine:
            if (waterLevel > 0.01f)
            {
                outputPower = PHES_TURB_MAX * (waterLevel / 100.0f);
                waterLevel -= outputPower * gameDt * pctPerMWs;
                waterLevel = Clamp(waterLevel, 0.0f, 100.0f);
                if (waterLevel <= 0.0f) { mode = PHESMode::Idle; outputPower = 0.0f; }
            }
            else { outputPower = 0.0f; }
            break;

        case PHESMode::Pump:
            if (waterLevel < 99.99f)
            {
                outputPower = -PHES_PUMP_MAX;
                waterLevel += PHES_PUMP_MAX * PHES_EFF * gameDt * pctPerMWs;
                waterLevel = Clamp(waterLevel, 0.0f, 100.0f);
                if (waterLevel >= 100.0f) { mode = PHESMode::Idle; outputPower = 0.0f; }
            }
            else { outputPower = 0.0f; }
            break;

        default:
            outputPower = 0.0f;
            break;
        }
    }
};

// ---------------------------------------------------------------------------
// FreqModel — уравнение swing (колебаний): df/dt = (P_gen - P_dem) / (2·H·S_nom)
// ---------------------------------------------------------------------------
struct FreqModel
{
    float frequency; // Гц — частота сети
    float dfdt;      // Гц/с (последняя вычисленная производная для отображения)

    void Init()
    {
        frequency = FREQ_NOM;
        dfdt = 0.0f;
    }

    // gameDt берется в игровых секундах, чтобы частота заметно реагировала на изменения нагрузки по шкале игрового времени
    void Update(float pGen, float pDemand, float gameDt)
    {
        dfdt = (pGen - pDemand) / (2.0f * H_INERTIA * S_NOM);
        frequency = frequency + dfdt * gameDt;
    }
};

// ---------------------------------------------------------------------------
// CollisionBox — хитбокс (коробка коллизии) для статических объектов сцены
// ---------------------------------------------------------------------------
struct CollisionBox
{
    Vector3 min;
    Vector3 max;
};

// Хитбоксы коллизий сцены (статические объекты)
static CollisionBox g_CollisionBoxes[] = {
    // Задняя стена (Z = -5.0)
    { { -7.5f, 0.0f, -5.2f }, { 7.5f, 5.0f, -4.8f } },

    // Передняя стена (Z = 5.0)
    { { -7.5f, 0.0f, 4.8f }, { 7.5f, 5.0f, 5.2f } },

    // Левая стена (X = -7.1)
    { { -7.3f, 0.0f, -7.0f }, { -6.9f, 5.0f, 7.0f } },

    // Правая стена (X = 7.1)
    { { 6.9f, 0.0f, -7.0f }, { 7.3f, 5.0f, 7.0f } },

    // Основание стола (центр (0, 0.5, -3), размер (6, 1.2, 2))
    // min: (-3, 0, -4), max: (3, 1.2, -2)
    { { -3.0f, 0.0f, -4.0f }, { 3.0f, 1.2f, -2.0f } },

    // Столешница (центр (0, 1.1, -3), размер (6.5, 0.2, 2.3))
    // min: (-3.25, 1.0, -4.15), max: (3.25, 1.2, -1.85)
    { { -3.25f, 1.0f, -4.15f }, { 3.25f, 1.2f, -1.85f } },

    // Задняя стойка (центр (0, 2.0, -4), размер (6, 1.6, 0.15))
    // min: (-3, 1.2, -4.075), max: (3, 2.8, -3.925)
    { { -3.0f, 1.2f, -4.075f }, { 3.0f, 2.8f, -3.925f } },

    // Клавиатура (центр (0, 1.22, -2.8), размер (1.8, 0.04, 0.7))
    // min: (-0.9, 1.2, -3.15), max: (0.9, 1.24, -2.45)
    { { -0.9f, 1.2f, -3.15f }, { 0.9f, 1.24f, -2.45f } },

    // Мышь (центр (1.3, 1.22, -2.8), размер (0.25, 0.04, 0.4))
    // min: (1.175, 1.2, -3.0), max: (1.425, 1.24, -2.6)
    { { 1.175f, 1.2f, -3.0f }, { 1.425f, 1.24f, -2.6f } },

    // Оголовье наушников (центр (-1.6, 1.35, -2.8), размер (0.6, 0.08, 0.4))
    // min: (-1.9, 1.31, -3.0), max: (-1.3, 1.39, -2.6)
    { { -1.9f, 1.31f, -3.0f }, { -1.3f, 1.39f, -2.6f } },

    // Headphones left ear cup (центр (-1.9, 1.22, -2.8), размер (0.15, 0.25, 0.25))
    // min: (-1.975, 1.095, -2.925), max: (-1.825, 1.345, -2.675)
    { { -1.975f, 1.095f, -2.925f }, { -1.825f, 1.345f, -2.675f } },

    // Headphones right ear cup (центр (-1.3, 1.22, -2.8), размер (0.15, 0.25, 0.25))
    // min: (-1.375, 1.095, -2.925), max: (-1.225, 1.345, -2.675)
    { { -1.375f, 1.095f, -2.925f }, { -1.225f, 1.345f, -2.675f } },

    // Monitor body (центр (0, 2.3, -3.5), размер (5.2, 1.5, 0.3))
    // min: (-2.6, 1.55, -3.65), max: (2.6, 3.05, -3.35)
    { { -2.6f, 1.55f, -3.65f }, { 2.6f, 3.05f, -3.35f } },

    // Monitor stand (центр (0, 1.55, -3.5), размер (0.3, 0.8, 0.2))
    // min: (-0.15, 1.15, -3.6), max: (0.15, 1.95, -3.4)
    { { -0.15f, 1.15f, -3.6f }, { 0.15f, 1.95f, -3.4f } },

    // Monitor base (центр (0, 1.2, -3.5), размер (1.2, 0.1, 0.6))
    // min: (-0.6, 1.15, -3.8), max: (0.6, 1.25, -3.2)
    { { -0.6f, 1.15f, -3.8f }, { 0.6f, 1.25f, -3.2f } },

    // Left rack (центр (-5.5, 1.5, -3.5), размер (1.5, 3.0, 0.5))
    // min: (-6.25, 0, -3.75), max: (-4.75, 3.0, -3.25)
    { { -6.25f, 0.0f, -3.75f }, { -4.75f, 3.0f, -3.25f } },

    // Right rack (центр (5.5, 1.5, -3.5), размер (1.5, 3.0, 0.5))
    // min: (4.75, 0, -3.75), max: (6.25, 3.0, -3.25)
    { { 4.75f, 0.0f, -3.75f }, { 6.25f, 3.0f, -3.25f } },

    // Боковые стойки пульта левая (центр (-3.5, 0.8, -3), размер (0.4, 1.2, 0.6))
    // min: (-3.7, 0.2, -3.3), max: (-3.3, 1.4, -2.7)
    { { -3.7f, 0.2f, -3.3f }, { -3.3f, 1.4f, -2.7f } },

    // Боковые стойки пульта правая (центр (3.5, 0.8, -3), размер (0.4, 1.2, 0.6))
    // min: (3.3, 0.2, -3.3), max: (3.7, 1.4, -2.7)
    { { 3.3f, 0.2f, -3.3f }, { 3.7f, 1.4f, -2.7f } },
};
static const int g_CollisionBoxCount = (int)(sizeof(g_CollisionBoxes) / sizeof(g_CollisionBoxes[0]));

// Проверка попадания точки внутрь любого из хитбоксов коллизии
static bool CheckCollision(Vector3 point, float radius = 0.5f)
{
    for (int i = 0; i < g_CollisionBoxCount; ++i)
    {
        CollisionBox& box = g_CollisionBoxes[i];
        if (point.x + radius > box.min.x && point.x - radius < box.max.x &&
            point.y + radius > box.min.y && point.y - radius < box.max.y &&
            point.z + radius > box.min.z && point.z - radius < box.max.z)
        {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// SceneCamera — камера первого лица с обработкой коллизий
// ---------------------------------------------------------------------------
struct SceneCamera
{
    Camera3D cam;
    float    yaw;      // радианы, горизонтальный поворот вокруг Y
    float    pitch;    // радианы, наклон вверх/вниз (положительный = смотреть вниз)
    Vector3  position; // позиция игрока/камеры
    float    moveSpeed; // скорость перемещения
    bool     cursorLocked;

    void Init()
    {
        position = { 0.0f, 2.5f, 3.0f }; // приподнятый уровень глаз
        yaw = 0.0f;
        pitch = 0.0f;
        moveSpeed = 5.0f;
        cursorLocked = false;

        cam.up = { 0.0f, 1.0f, 0.0f };
        cam.fovy = 60.0f;
        cam.projection = CAMERA_PERSPECTIVE;
        Rebuild();
    }

    void Rebuild()
    {
        cam.position = position;
        // Расчет вектора направления взгляда по yaw/pitch
        Vector3 forward = {
            cosf(pitch) * sinf(yaw),
            sinf(pitch),
            cosf(pitch) * cosf(yaw)
        };
        cam.target = { position.x + forward.x, position.y + forward.y, position.z + forward.z };
    }

    void HandleMouse()
    {
        // Захват/освобождение курсора по правой кнопке мыши
        if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT))
        {
            cursorLocked = !cursorLocked;
            if (cursorLocked)
            {
                DisableCursor();
                // Центрируем курсор мыши
                SetMousePosition(SCREEN_W / 2, SCREEN_H / 2);
            }
            else
                EnableCursor();
        }

        // Применяем вращение мыши при захваченном курсоре
        if (cursorLocked)
        {
            Vector2 delta = GetMouseDelta();
            yaw -= delta.x * 0.003f;
            pitch = Clamp(pitch - delta.y * 0.003f, -PI_F * 0.49f, PI_F * 0.49f);
            Rebuild();
            // Удерживаем мышь в центре
            SetMousePosition(SCREEN_W / 2, SCREEN_H / 2);
        }
    }

    void HandleMovement(float dt)
    {
        if (!cursorLocked) return;

        // Рассчитываем векторы вперед и вправо по yaw (только по горизонтали для движения)
        Vector3 forward = { sinf(yaw), 0.0f, cosf(yaw) };
        Vector3 right = { cosf(yaw), 0.0f, -sinf(yaw) };

        Vector3 movement = { 0.0f, 0.0f, 0.0f };

        // Движение WASD (A/D инвертированы для соответствия направлению)
        if (IsKeyDown(KEY_W)) { movement.x += forward.x; movement.y += forward.y; movement.z += forward.z; }
        if (IsKeyDown(KEY_S)) { movement.x -= forward.x; movement.y -= forward.y; movement.z -= forward.z; }
        if (IsKeyDown(KEY_A)) { movement.x += right.x; movement.y += right.y; movement.z += right.z; }
        if (IsKeyDown(KEY_D)) { movement.x -= right.x; movement.y -= right.y; movement.z -= right.z; }

        // Нормализация вектора и применение скорости
        float len = sqrtf(movement.x * movement.x + movement.y * movement.y + movement.z * movement.z);
        if (len > 0.0f)
        {
            movement.x /= len;
            movement.y /= len;
            movement.z /= len;
            movement.x *= moveSpeed * dt;
            movement.y *= moveSpeed * dt;
            movement.z *= moveSpeed * dt;

            // Проверяем коллизии перед применением перемещения
            Vector3 newPos = { position.x + movement.x, position.y + movement.y, position.z + movement.z };
            // Проверка на уровне ног (высота камеры - 1.7м)
            Vector3 feetPos = { newPos.x, newPos.y - 1.7f, newPos.z };
            if (!CheckCollision(feetPos))
            {
                position = newPos;
                Rebuild();
            }
        }
    }
};

// ---------------------------------------------------------------------------
// Режим игры
// ---------------------------------------------------------------------------
enum class GameMode { Normal = 0, Training, Paused };

// ---------------------------------------------------------------------------
// GameData — состояние симуляции верхнего уровня
// ---------------------------------------------------------------------------
struct GameData
{
    DemandModel  demand;
    ReactorModel reactor;
    PHESModel    phes;
    FreqModel    freq;
    SceneCamera  scene;

    float       survivalTime;              // прошедшее реальное время в секундах
    float       gameHour;                  // игровые часы 0..24
    const char* actionMsg;                 // сообщение о последнем действии с кнопкой (временный показ)
    float       actionTimer;               // оставшиеся секунды показа сообщения actionMsg
    const char* eventMsg;                  // сообщение о случайном событии (временный показ)
    float       eventTimer;                // оставшиеся секунды показа сообщения eventMsg
    float       freqHistory[FREQ_HIST_LEN]; // кольцевой буфер отсчетов частоты
    int         freqHistHead;              // индекс следующей записи частоты

    // ---- Поля модификаторов событий (сбрасываются каждый кадр, накапливаются активными событиями) ----
    float demandMultiplier; // умножается на demand.currentDemand после demand.Update()
    float availablePower;   // ограничивает общую генерацию, передаваемую в FreqModel (МВт)
    float reactorMax;       // динамический потолок реактора, применяется после reactor.Update() (МВт)
    float pumpedEfficiency; // масштабирует phes.outputPower перед расчетом частоты

    // ---- Экран монитора (2D UI рендерится в текстуру, затем накладывается на 3D-монитор) ----
    RenderTexture2D monitorScreen; // текстура рендеринга 800x200 для интерфейса монитора

    GameMode mode;
    bool     running;

    void Init()
    {
        demand.Init();
        reactor.Init();
        phes.Init();
        freq.Init();
        scene.Init();

        survivalTime = 0.0f;
        gameHour = 8.0f; // начинаем в 08:00
        actionMsg = "";
        actionTimer = 0.0f;
        eventMsg = "";
        eventTimer = 0.0f;
        for (int i = 0; i < FREQ_HIST_LEN; ++i)
            freqHistory[i] = FREQ_NOM; // предзаполняем номинальной частотой
        freqHistHead = 0;
        demandMultiplier = 1.0f;
        availablePower = 2000.0f;
        reactorMax = 2000.0f;
        pumpedEfficiency = 1.0f;
        monitorScreen = LoadRenderTexture(800, 200); // соотношение 4:1 для экрана монитора
        mode = GameMode::Normal;
        running = true;
    }
};

// ---------------------------------------------------------------------------
// RandomEvent — самостоятельный сценарий, временно модифицирующий параметры симуляции
// ---------------------------------------------------------------------------
enum class EventType
{
    HeatWave,
    LineTrip,
    Maintenance,
    Storm,
    IndustrialShutdown
};

struct RandomEvent
{
    std::string name;
    std::string description;
    float       probability; // шанс срабатывания в реальную секунду (0.002 .. 0.05)
    float       duration;    // длительность события в реальных секундах (15 .. 60)
    float       timer;       // оставшееся время действия события в секундах
    bool        active;
    EventType   type;

    // Инициализация события: активация и запуск обратного отсчета.
    void Start(GameData& gd)
    {
        active = true;
        timer = duration;
        gd.eventMsg = name.c_str();
        gd.eventTimer = 5.0f; // показывать сообщение в течение 5 секунд
    }

    // Накапливает влияние этого события на параметры GameData.
    // Вызывается каждый кадр, пока событие активно; параметры сбрасываются к значениям по умолчанию
    // перед циклом событий, поэтому несколько одновременных событий корректно сочетаются.
    void ApplyEffect(GameData& gd)
    {
        switch (type)
        {
        case EventType::HeatWave:
            // Аномальная жара — спрос увеличивается на коэффициент 1.2
            gd.demandMultiplier = 1.2f;
            break;

        case EventType::LineTrip:
            // Сбой в сети передач — доступная мощность генерации снижается на 200 МВт
            gd.availablePower -= 200.0f;
            break;

        case EventType::Maintenance:
            // Плановое техобслуживание — динамическое ограничение реактора снижается на 300 МВт
            gd.reactorMax -= 300.0f;
            break;

        case EventType::Storm:
            // Сильный шторм — КПД цикла ГАЭС снижается в два раза
            gd.pumpedEfficiency = 0.5f;
            break;

        case EventType::IndustrialShutdown:
            // Отключение крупного потребителя — вычитаем 300 МВт из текущего спроса
            gd.demand.currentDemand -= 300.0f;
            break;
        }
    }

    // Уменьшает таймер действия и деактивирует событие по его истечении.
    void Update(float dt)
    {
        if (!active) return;
        timer -= dt;
        if (timer <= 0.0f)
        {
            active = false;
            timer = 0.0f;
        }
    }
};

// Глобальный пул событий — каждое событие срабатывает независимо на основе вероятности.
// Поля: имя, описание, вероятность(/с), длительность(с), таймер, активность, тип
static RandomEvent g_Events[] =
{
    { "ЖАРА",
      "Аномальная жара  —  спрос на +20 %",
      0.003f, 40.0f, 0.0f, false, EventType::HeatWave          },

    { "АВАРИЯ ЛИНИИ",
      "Сбой передачи  —  реактор ограничен до 70 %",
      0.002f, 25.0f, 0.0f, false, EventType::LineTrip          },

    { "ТЕХОБСЛУЖИВАНИЕ",
      "Плановый ремонт  —  реактор ограничен до 60 %",
      0.004f, 45.0f, 0.0f, false, EventType::Maintenance        },

    { "ШТОРМ",
      "Сильная погода  —  эффективность ГАЭС снижена",
      0.005f, 30.0f, 0.0f, false, EventType::Storm             },

    { "ОСТАНОВКА ЗАВОДА",
      "Крупный завод отключён  —  спрос падает на 300 МВт",
      0.007f, 20.0f, 0.0f, false, EventType::IndustrialShutdown },
};
static const int g_EventCount = (int)(sizeof(g_Events) / sizeof(g_Events[0]));

// ---------------------------------------------------------------------------
// Обработка пользовательского ввода
// ---------------------------------------------------------------------------
void HandleInput(GameData& gd, float dt)
{
    gd.scene.HandleMouse();
    gd.scene.HandleMovement(dt);

    if (IsKeyPressed(KEY_ESCAPE)) { currentState = STATE_MENU; return; }

    if (IsKeyPressed(KEY_T))
        gd.mode = (gd.mode == GameMode::Training) ? GameMode::Normal : GameMode::Training;

    if (IsKeyPressed(KEY_P))
        gd.mode = (gd.mode == GameMode::Paused) ? GameMode::Normal : GameMode::Paused;

    if (gd.mode == GameMode::Paused)
        return;

    // Регулировка реактора — изменение в реальном времени: 80 МВт/с плавно, 400 МВт/с при зажатом Shift
    const float step = IsKeyDown(KEY_LEFT_SHIFT) ? 400.0f : 80.0f;
    if (IsKeyDown(KEY_UP))
        gd.reactor.targetPower = Clamp(gd.reactor.targetPower + step * dt,
            REACTOR_MIN, REACTOR_MAX);
    if (IsKeyDown(KEY_DOWN))
        gd.reactor.targetPower = Clamp(gd.reactor.targetPower - step * dt,
            REACTOR_MIN, REACTOR_MAX);

    // Выбор режима ГАЭС клавишами 1, 2, 3
    if (IsKeyPressed(KEY_ONE))   gd.phes.SetMode(PHESMode::Turbine);
    if (IsKeyPressed(KEY_TWO))   gd.phes.SetMode(PHESMode::Pump);
    if (IsKeyPressed(KEY_THREE)) gd.phes.SetMode(PHESMode::Idle);

    // [E] или клик мыши — Активация подсвеченной 3D-кнопки
    if (IsKeyPressed(KEY_E) || IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
    {
        for (int i = 0; i < g_ButtonCount; ++i)
        {
            if (!g_Buttons[i].isHovered) continue;

            // Временная переменная: заполняется названием действия при совпадении
            const char* action = nullptr;

            const char* lbl = g_Buttons[i].label;

            if (strcmp(lbl, "РЕАКТОР+") == 0)
            {
                gd.reactor.targetPower = Clamp(gd.reactor.targetPower + 80.0f,
                    REACTOR_MIN, REACTOR_MAX);
                action = ">>> МОЩНОСТЬ РЕАКТОРА +80 МВт";
            }
            else if (strcmp(lbl, "РЕАКТОР-") == 0)
            {
                gd.reactor.targetPower = Clamp(gd.reactor.targetPower - 80.0f,
                    REACTOR_MIN, REACTOR_MAX);
                action = ">>> МОЩНОСТЬ РЕАКТОРА -80 МВт";
            }
            else if (strcmp(lbl, "ТУРБИНА") == 0)
            {
                gd.phes.SetMode(PHESMode::Turbine); // режим генерации
                action = ">>> ГАЭС: ТУРБИНА АКТИВНА";
            }
            else if (strcmp(lbl, "НАСОС") == 0)
            {
                gd.phes.SetMode(PHESMode::Pump);    // режим закачки
                action = ">>> ГАЭС: НАСОС АКТИВЕН";
            }
            else if (strcmp(lbl, "ПЕРЕЗАПУСК") == 0)
            {
                gd.Init(); // полный перезапуск игры
                return;    // выход из функции, так как gd переинициализирован
            }

            if (action)
            {
                g_Buttons[i].Press();          // запуск анимации нажатия кнопки
                gd.actionMsg = action;
                gd.actionTimer = 3.0f;
            }
            break; // только одна кнопка может быть под курсором одновременно
        }
    }
}

// ---------------------------------------------------------------------------
// Шаг обновления симуляции
// ---------------------------------------------------------------------------
void Update(GameData& gd, float dt)
{
    if (gd.mode == GameMode::Paused)
        return;

    gd.survivalTime += dt;

    // Таймер показа сообщения о действии
    if (gd.actionTimer > 0.0f)
        gd.actionTimer -= dt;

    if (gd.eventTimer > 0.0f)
        gd.eventTimer -= dt;

    const float gameDt = dt * TIME_SCALE;

    // Продвигаем игровые часы вперед
    gd.gameHour = fmodf(gd.gameHour + gameDt / 3600.0f, 24.0f);

    // ---- Шаг 1: сброс полей модификаторов к нейтральным значениям ----
    gd.demandMultiplier = 1.0f;
    gd.availablePower = 2000.0f;
    gd.reactorMax = 2000.0f;
    gd.pumpedEfficiency = 1.0f;

    // ---- Шаг 2: базовое обновление подмоделей (спрос рассчитывается здесь) ----
    gd.demand.Update(gd.gameHour);
    gd.reactor.Update(dt);   // физическое изменение в реальном времени
    gd.phes.Update(gameDt);  // расход воды в игровом времени

    // ---- Шаг 3: срабатывание случайных событий и накопление их эффектов ----
    for (int i = 0; i < g_EventCount; ++i)
    {
        RandomEvent& ev = g_Events[i];
        if (!ev.active)
        {
            // Вероятность задается на секунду времени; масштабируем на этот кадр
            const int threshold = (int)(ev.probability * dt * 100000.0f);
            if (GetRandomValue(0, 99999) < threshold)
                ev.Start(gd);
        }
        if (ev.active)
            ev.ApplyEffect(gd); // накапливает эффекты в полях модификаторов
        ev.Update(dt);          // обновляет обратный отсчет события
    }

    // ---- Шаг 4: применение ограничений событий к параметрам симуляции ----
    // Ограничение реактора сверху (при техобслуживании)
    gd.reactor.targetPower = Clamp(gd.reactor.targetPower, REACTOR_MIN, gd.reactorMax);
    gd.reactor.actualPower = Clamp(gd.reactor.actualPower, REACTOR_MIN, gd.reactorMax);

    // Применение множителя спроса (например, при ЖАРЕ)
    gd.demand.currentDemand = Clamp(gd.demand.currentDemand * gd.demandMultiplier,
        100.0f, 5000.0f);

    // Изменение эффективности ГАЭС (например, при ШТОРМЕ)
    gd.phes.outputPower *= gd.pumpedEfficiency;

    // ---- Шаг 5: расчет частоты сети (мощность генерации ограничена availablePower) ----
    const float pGen = Clamp(gd.reactor.actualPower + gd.phes.outputPower,
        0.0f, gd.availablePower);
    const float pDemand = gd.demand.currentDemand;

    gd.freq.Update(pGen, pDemand, gameDt);

    // Record frequency sample into circular history buffer
    gd.freqHistory[gd.freqHistHead] = gd.freq.frequency;
    gd.freqHistHead = (gd.freqHistHead + 1) % FREQ_HIST_LEN;

    // ---- Проверка условий победы/поражения ----
    // Поражение: частота вышла за пределы 45-55 Гц (допуск ±5 Гц от номинала 50 Гц)
    if (gd.freq.frequency <= FREQ_MIN || gd.freq.frequency >= FREQ_MAX)
    {
        gameResult = RESULT_LOSE;
        currentState = STATE_RESULTS;
        return;
    }

    // Победа: время смены истекло (5 минут реального времени)
    if (gd.survivalTime >= shiftDuration)
    {
        gameResult = RESULT_WIN;
        currentState = STATE_RESULTS;
        return;
    }
}

// ---------------------------------------------------------------------------
// Helpers for Draw
// ---------------------------------------------------------------------------
static const char* PhesModeStr(PHESMode m)
{
    switch (m)
    {
    case PHESMode::Turbine: return "ТУРБИНА";
    case PHESMode::Pump:    return "НАСОС";
    default:                return "ОЖИДАНИЕ";
    }
}

static void DrawRow(int x, int& y, int lineH, int fs, Color col, const char* fmt, ...)
{
    char buf[128];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    DrawTextEx(cyrillicFont, buf, { (float)x, (float)y }, fs, 0, col);
    y += lineH;
}

// ---------------------------------------------------------------------------
// DrawScene3D — 3D-геометрия пультовой комнаты управления
// ---------------------------------------------------------------------------
static float alarmBlinkTimer = 0.0f;
const Vector3 LAMP_BASE_POS = { 0.0f, 4.8f, -2.5f }; // Основание крепится к потолку
const Vector3 LAMP_BULB_POS = { 0.0f, 4.5f, -2.5f }; // Сам плафон свисает ниже
const Color BASE_COLOR = DARKGRAY;
const Color BULB_OFF_COLOR = { 30, 30, 30, 255 }; // Цвет, когда лампа выключена
static void DrawAlarmIndicators(float dt, float currentFrequency)
{
    alarmBlinkTimer += dt;
    bool blinkState = (int)(alarmBlinkTimer * 2.0f) % 2 == 0;
    float freqDeviation = fabsf(currentFrequency - FREQ_NOM);

    // 1. Отрисовка основания (патрона) - ВСЕГДА (вне любого if)
    // Оно теперь будет на потолке, так как вы задали Y = 4.9f
    DrawCylinder(LAMP_BASE_POS, 0.15f, 0.15f, 0.3f, 16, BASE_COLOR);

    // 2. Логика цвета
    Color alarmColor = (freqDeviation > 1.5f && blinkState) ? RED : BULB_OFF_COLOR;

    // 3. Плафон (стеклянная часть)
    DrawCylinder(LAMP_BULB_POS, 0.25f, 0.25f, 0.4f, 16, alarmColor);

    // 4. Свечение - ТОЛЬКО при аварии
    if (freqDeviation > 1.5f)
    {
        BeginBlendMode(BLEND_ADDITIVE);
        DrawCylinder(LAMP_BULB_POS, 0.35f, 0.35f, 0.5f, 16, { 255, 0, 0, 100 });
        EndBlendMode();
    }

    // Дополнительная желтая лампа при высоком спросе (баланс отрицательный)
    static float lastImbalance = 0.0f;
    // Для этого нужно передавать imbalance или получать из gd
}

// Объявление глобальных ресурсов
static Model wallModel, ceilModel, wallDoubleModel;
static Texture2D texWall, texWallDouble, texCeil;
static bool resourcesLoaded = false;

// Функция для инициализации ресурсов (вызывается один раз)
void InitSceneResources() {
    texWall = LoadTexture("assets/WALL_.bmp");
    texWallDouble = LoadTexture("assets/WALL_DOUBLE.bmp");
    texCeil = LoadTexture("assets/Tile_for_ceiling.bmp");

    Mesh cubeMesh = GenMeshCube(1.0f, 1.0f, 1.0f);

    wallModel = LoadModelFromMesh(cubeMesh);
    SetMaterialTexture(&wallModel.materials[0], MATERIAL_MAP_DIFFUSE, texWall);

    ceilModel = LoadModelFromMesh(cubeMesh);
    SetMaterialTexture(&ceilModel.materials[0], MATERIAL_MAP_DIFFUSE, texCeil);

    wallDoubleModel = LoadModelFromMesh(cubeMesh);
    SetMaterialTexture(&wallDoubleModel.materials[0], MATERIAL_MAP_DIFFUSE, texWallDouble);

    resourcesLoaded = true;
}

static void DrawScene3D(const GameData& gd)
{
    // ---- Цветовая палитра ----
    const Color cFloor = { 30,  30,  40, 255 };
    const Color cDeskBase = { 50,  50,  70, 255 };
    const Color cDeskTop = { 70,  70,  90, 255 };
    const Color cMonBody = { 30,  30,  40, 255 };
    const Color cScreen = { 0, 100, 150, 255 };
    const Color cRack = { 40,  42,  60, 255 };

    // ---- Симуляция окружающего света (аддитивное заполнение, без использования шейдеров) ----
    // Небольшой аддитивный объем над комнатой аппроксимирует AMBIENT_INTENSITY = 0.3.
    // Может быть заменено шейдерным проходом rlights.h для физически корректных результатов.
    BeginBlendMode(BLEND_ADDITIVE);
    DrawCube({ 0.0f, 2.5f, -2.0f }, 15.0f, 6.0f, 15.0f,
        { 200, 210, 255, (unsigned char)(AMBIENT_INTENSITY * 20.0f) });
    EndBlendMode();

    // ---- Пол ----
    DrawPlane({ 0.0f, 0.0f, 0.0f }, { 16.0f, 14.0f }, cFloor);

    // Плиточный узор на полу
    // Темный: холодный серый с глубоким подтоном
    const Color tileDark = { 55, 58, 60, 255 };
    // Светлый: мягкий светло-серый
    const Color tileLight = { 180, 185, 190, 255 };
    const float tileSize = 2.0f;

    for (float x = -8.0f; x < 8.0f; x += tileSize)
    {
        for (float z = -6.0f; z < 6.0f; z += tileSize)
        {
            int ix = (int)floor(x / tileSize);
            int iz = (int)floor(z / tileSize);

            Color tileColor = ((ix + iz) % 2 == 0) ? tileLight : tileDark;

            DrawCube({ x + tileSize / 2, 0.01f, z + tileSize / 2 }, tileSize, 0.02f, tileSize, tileColor);
        }
    }

    // ---- Стены и потолок ----
    if (!resourcesLoaded) InitSceneResources();

    // Задняя стена
    DrawModelEx(wallModel, { 0.0f, 2.5f, -5.0f }, { 0,1,0 }, 0, { 15.0f, 5.0f, 0.2f }, WHITE);
    DrawCubeWires({ 0.0f, 2.5f, -5.0f }, 15.0f, 5.0f, 0.2f, { 100, 120, 160, 200 });

    // Левая стена
    DrawModelEx(wallModel, { -7.1f, 2.5f, 0.0f }, { 0,1,0 }, 0, { 0.2f, 5.0f, 14.0f }, WHITE);
    DrawCubeWires({ -7.1f, 2.5f, 0.0f }, 0.2f, 5.0f, 14.0f, { 100, 120, 160, 200 });

    // Правая стена
    DrawModelEx(wallModel, { 7.1f, 2.5f, 0.0f }, { 0,1,0 }, 0, { 0.2f, 5.0f, 14.0f }, WHITE);
    DrawCubeWires({ 7.1f, 2.5f, 0.0f }, 0.2f, 5.0f, 14.0f, { 100, 120, 160, 200 });

    // Потолок
    DrawModelEx(ceilModel, { 0.0f, 5.1f, 0.0f }, { 0,1,0 }, 0, { 15.0f, 0.2f, 14.0f }, WHITE);
    DrawCubeWires({ 0.0f, 5.1f, 0.0f }, 15.0f, 0.2f, 14.0f, { 100, 120, 160, 200 });

    // Передняя стена
    DrawModelEx(wallDoubleModel, { 0.0f, 2.5f, 5.0f }, { 0,1,0 }, 0, { 15.0f, 5.0f, 0.2f }, WHITE);
    DrawCubeWires({ 0.0f, 2.5f, 5.0f }, 15.0f, 5.0f, 0.2f, { 100, 120, 160, 200 });

    // ---- Дверь на передней стене (только декор) ----
    const Color cDoor = { 40, 35, 30, 255 };
    const Color cDoorFrame = { 60, 55, 50, 255 };
    // Дверная коробка
    DrawCube({ 0.0f, 1.5f, 4.9f }, 1.5f, 3.5f, 0.1f, cDoorFrame);
    // Полотно двери
    DrawCube({ 0.0f, 1.5f, 4.85f }, 1.3f, 3.3f, 0.05f, cDoor);

    // ---- Пульт управления (координаты через MakeVector3) ----
    static float lastDt = 1.0f / 60.0f;
    DrawAlarmIndicators(lastDt, gd.freq.frequency);
    // 1. Основание пульта — (0, 0.5, -1), размер 6x1.2x2, цвет (50,50,70)
    DrawCube(MakeVector3(0.0f, 0.5f, -3.0f), 6.0f, 1.2f, 2.0f, cDeskBase);
    DrawCubeWires(MakeVector3(0.0f, 0.5f, -3.0f), 6.0f, 1.2f, 2.5f, { 80, 80, 110, 70 });

    // 2. Столешница — (0, 1.1, -1), размер 6.5x0.2x2.3, цвет (70,70,90)
    DrawCube(MakeVector3(0.0f, 1.1f, -3.0f), 6.5f, 0.2f, 2.3f, cDeskTop);

    // ---- Декоративные элементы на пульте ----
    // Клавиатура - по центру перед монитором
    DrawCube(MakeVector3(0.0f, 1.22f, -2.8f), 1.8f, 0.04f, 0.7f, { 35, 35, 45, 255 });
    DrawCubeWires(MakeVector3(0.0f, 1.22f, -2.8f), 1.8f, 0.04f, 0.7f, { 60, 60, 80, 100 });

    // Мышь - справа от клавиатуры
    DrawCube(MakeVector3(1.3f, 1.22f, -2.8f), 0.25f, 0.04f, 0.4f, { 35, 35, 45, 255 });
    DrawCubeWires(MakeVector3(1.3f, 1.22f, -2.8f), 0.25f, 0.04f, 0.4f, { 60, 60, 80, 100 });

    // Наушники - слева от клавиатуры
    // Дужка (оголовье)
    DrawCube(MakeVector3(-1.6f, 1.35f, -2.8f), 0.6f, 0.08f, 0.4f, { 25, 25, 35, 255 });
    // Амбушюры (чашки)
    DrawCube(MakeVector3(-1.9f, 1.22f, -2.8f), 0.15f, 0.25f, 0.25f, { 20, 20, 30, 255 });
    DrawCube(MakeVector3(-1.3f, 1.22f, -2.8f), 0.15f, 0.25f, 0.25f, { 20, 20, 30, 255 });

    // Задняя соединительная панель пульта к монитору
    DrawCube(MakeVector3(0.0f, 2.0f, -4.0f), 6.0f, 1.6f, 0.15f, cDeskBase);

    // ---- Корпус монитора ----
    // 3. Корпус монитора — (0, 2.3, -1.5), размер 5.2x1.5x0.3, цвет (30,30,40)
    DrawCube(MakeVector3(0.0f, 2.3f, -3.5f), 5.2f, 1.5f, 0.3f, cMonBody);

    // Ножка/стойка монитора — соединяет корпус с пультом
    DrawCube(MakeVector3(0.0f, 1.55f, -3.5f), 0.3f, 0.8f, 0.2f, cMonBody);
    DrawCubeWires(MakeVector3(0.0f, 1.55f, -3.5f), 0.3f, 0.8f, 0.2f, { 60, 60, 80, 100 });

    // Подставка монитора на столе
    DrawCube(MakeVector3(0.0f, 1.2f, -3.5f), 1.2f, 0.1f, 0.6f, cMonBody);
    DrawCubeWires(MakeVector3(0.0f, 1.2f, -3.5f), 1.2f, 0.1f, 0.6f, { 60, 60, 80, 100 });

    // 4. Экран — текстурирован интерфейсом с помощью полигональной сетки и UV-координат
    static Mesh screenMesh = { 0 };
    static bool screenMeshInitialized = false;

    if (!screenMeshInitialized)
    {
        // Создаем простую полигональную сетку (2 треугольника, 4 вершины)
        screenMesh.triangleCount = 2;
        screenMesh.vertexCount = 4;
        screenMesh.vertices = (float*)MemAlloc(4 * 3 * sizeof(float));
        screenMesh.texcoords = (float*)MemAlloc(4 * 2 * sizeof(float));
        screenMesh.normals = (float*)MemAlloc(4 * 3 * sizeof(float));
        screenMesh.indices = (unsigned short*)MemAlloc(6 * sizeof(unsigned short));

        // Вершины (квадрат, обращенный по направлению +Z)
        // Сверху-слева, Сверху-справа, Снизу-слева, Снизу-справа
        const float w = 5.0f, h = 1.2f;
        screenMesh.vertices[0] = -w / 2; screenMesh.vertices[1] = h / 2;  screenMesh.vertices[2] = 0;
        screenMesh.vertices[3] = w / 2; screenMesh.vertices[4] = h / 2;  screenMesh.vertices[5] = 0;
        screenMesh.vertices[6] = -w / 2; screenMesh.vertices[7] = -h / 2; screenMesh.vertices[8] = 0;
        screenMesh.vertices[9] = w / 2; screenMesh.vertices[10] = -h / 2; screenMesh.vertices[11] = 0;

        // UV-координаты (от 0,0 до 1,1) - инвертированы по Y и X для отзеркаливания текстуры
        screenMesh.texcoords[0] = 1.0f; screenMesh.texcoords[1] = 1.0f;
        screenMesh.texcoords[2] = 0.0f; screenMesh.texcoords[3] = 1.0f;
        screenMesh.texcoords[4] = 1.0f; screenMesh.texcoords[5] = 0.0f;
        screenMesh.texcoords[6] = 0.0f; screenMesh.texcoords[7] = 0.0f;

        // Индексы треугольников (обход против часовой стрелки)
        screenMesh.indices[0] = 0; screenMesh.indices[1] = 1; screenMesh.indices[2] = 2;  // Треугольник 1
        screenMesh.indices[3] = 2; screenMesh.indices[4] = 1; screenMesh.indices[5] = 3;  // Треугольник 2

        // Нормали (направлены по -Z к камере)
        for (int i = 0; i < 4; i++)
        {
            screenMesh.normals[i * 3 + 0] = 0.0f;
            screenMesh.normals[i * 3 + 1] = 0.0f;
            screenMesh.normals[i * 3 + 2] = -1.0f;
        }

        UploadMesh(&screenMesh, false);
        screenMeshInitialized = true;
    }

    // Отрисовываем текстурированный квад на позиции монитора с помощью модели
    static Model screenModel = { 0 };
    static bool screenModelInitialized = false;

    if (!screenModelInitialized)
    {
        screenModel = LoadModelFromMesh(screenMesh);
        screenModelInitialized = true;
    }

    // Обновляем текстуру каждый кадр
    SetMaterialTexture(&screenModel.materials[0], MATERIAL_MAP_DIFFUSE, gd.monitorScreen.texture);

    // Рисуем модель с вращением к камере
    Vector3 screenPos = { 0.0f, 2.3f, -3.24f };
    DrawModelEx(screenModel, screenPos, { 0.0f, 1.0f, 0.0f }, 180.0f, { 1.0f, 1.0f, 1.0f }, WHITE);

    // Свечение контура
    DrawCubeWires(MakeVector3(0.0f, 2.3f, -3.24f), 4.88f, 1.28f, 0.06f, { 50, 160, 255, 160 });
    // Тонкий светящийся прямоугольник на задней стене за монитором
    DrawCube({ 0.0f, 2.3f, -4.09f }, 6.5f, 3.0f, 0.02f, { 0,  50, 110,  50 });

    // ---- Боковые стойки пульта ближнего радиуса — (±3.5, 0.8, -1), 0.4x1.2x0.6 ----
    DrawCube(MakeVector3(-3.5f, 0.8f, -3.0f), 0.4f, 1.2f, 0.6f, cDeskBase);
    DrawCubeWires(MakeVector3(-3.5f, 0.8f, -3.0f), 0.4f, 1.2f, 0.6f, { 90, 90, 120, 80 });
    DrawCube(MakeVector3(3.5f, 0.8f, -3.0f), 0.4f, 1.2f, 0.6f, cDeskBase);
    DrawCubeWires(MakeVector3(3.5f, 0.8f, -3.0f), 0.4f, 1.2f, 0.6f, { 90, 90, 120, 80 });

    // ---- Верхняя потолочная панель — (0, 2.0, -1), 4x0.1x1.5 ----
    DrawCube(MakeVector3(0.0f, 5.0f, 0.5f), 4.0f, 0.1f, 1.5f, { 45, 45, 65, 255 });
    DrawCubeWires(MakeVector3(0.0f, 5.0f, 0.5f), 4.0f, 0.1f, 1.5f, { 80, 80, 110, 60 });
    // Синяя полоса подсветки на нижней стороне панели
    DrawCube(MakeVector3(0.0f, 4.945f, 0.5f), 3.8f, 0.01f, 1.3f, { 20, 80, 220, 180 });

    // ---- Стойки с оборудованием на дальней стороне ----
    DrawCube({ -5.5f, 1.5f, -3.5f }, 1.5f, 3.0f, 0.5f, cRack);
    DrawCubeWires({ -5.5f, 1.5f, -3.5f }, 1.5f, 3.0f, 0.5f, { 60, 60, 90, 70 });
    DrawCube({ 5.5f, 1.5f, -3.5f }, 1.5f, 3.0f, 0.5f, cRack);
    DrawCubeWires({ 5.5f, 1.5f, -3.5f }, 1.5f, 3.0f, 0.5f, { 60, 60, 90, 70 });

    // Светодиодные индикаторы стоек (5 с каждой стороны, зеленый/оранжевый)
    for (int i = 0; i < 5; ++i)
    {
        float  ledY = 0.5f + i * 0.45f;
        Color  ledCol = (i < 3) ? Color{ 40, 200, 80, 255 } : Color{ 200, 140, 40, 255 };
        DrawCube({ -5.5f, ledY, -3.2f }, 0.26f, 0.14f, 0.05f, ledCol);
        DrawCube({ 5.5f, ledY, -3.2f }, 0.26f, 0.14f, 0.05f, ledCol);
    }

    // ---- Кнопки панели: Button3D::Draw() управляет цветом/пульсацией/свечением ----
    for (int i = 0; i < g_ButtonCount; ++i)
    {
        const Button3D& b = g_Buttons[i];
        b.Draw();

        if (strcmp(b.label, "ПЕРЕЗАПУСК") != 0)
        {
            Vector3 ledPos = { b.position.x + b.size.x * 0.5f + 0.12f,
                               b.position.y,
                               b.position.z };

            DrawSphere(ledPos, 0.055f, { 230, 30, 30, 255 });
            BeginBlendMode(BLEND_ADDITIVE);
            DrawSphere(ledPos, 0.10f, { 255, 60, 60, 40 });
            EndBlendMode();
        }
    }

    // ---- Свечение экрана монитора (аддитивный синий объем перед экраном) ----
    // Плоскость экрана находится при z ≈ -1.315; объем свечения парит между экраном и камерой.
    BeginBlendMode(BLEND_ADDITIVE);
    DrawCube(MakeVector3(0.0f, 2.3f, -3.0f), 5.6f, 1.8f, 0.6f,
        { 0, 100, 255,  22 });  // широкое мягкое свечение
    DrawCube(MakeVector3(0.0f, 2.3f, -3.28f), 5.0f, 1.4f, 0.10f,
        { 40, 160, 255,  45 });  // яркое свечение у самого экрана
    EndBlendMode();
}

// ---------------------------------------------------------------------------
// DrawMonitorContent — рендерит 2D UI в текстуру рендеринга monitorScreen
// ---------------------------------------------------------------------------
static void DrawMonitorContent(GameData& gd)
{
    const int   pad = 10;
    const int   fs = 20;
    const int   lineH = 18;
    const Color hdr = { 100, 200, 255, 255 };
    const Color col = { 180, 220, 255, 255 };
    const Color warn = { 255, 120,  60, 255 };
    const Color dim = { 120, 140, 160, 255 };

    // Очищаем текстуру темным фоном
    ClearBackground({ 5, 8, 18, 255 });

    // ---- Заголовок ----
    DrawTextEx(cyrillicFont, "АЭС ДИСПЕТЧЕР", { (float)pad, (float)pad }, 20, 0, hdr);

    // Игровые часы
    int gh = (int)gd.gameHour;
    int gm = (int)((gd.gameHour - gh) * 60.0f);
    char timeBuf[16];
    snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d", gh, gm);
    DrawTextEx(cyrillicFont, timeBuf, { 700.0f, (float)pad }, fs, 0, dim);

    // ---- Компактное отображение данных (в один столбец) ----
    int y = pad + 30;
    const char* modeStr = (gd.mode == GameMode::Training) ? "ОБУЧ" :
        (gd.mode == GameMode::Paused) ? "ПАУЗА" : "НОРМ";
    DrawRow(pad, y, lineH, fs, col, "Р:%s Реактор:%.0f МВт", modeStr, gd.reactor.actualPower);
    DrawRow(pad, y, lineH, fs, col, "Спрос:%.0f МВт Баланс:%+.0f МВт", gd.demand.currentDemand,
        (gd.reactor.actualPower + gd.phes.outputPower) - gd.demand.currentDemand);
    DrawRow(pad, y, lineH, fs, col, "ГАЭС:%s Вода:%.0f%%", PhesModeStr(gd.phes.mode), gd.phes.waterLevel);

    float fDev = gd.freq.frequency - FREQ_NOM;
    Color freqCol = fabsf(fDev) < 0.2f ? col : warn;
    DrawRow(pad, y, lineH, fs, freqCol, "Частота:%.3f Гц", gd.freq.frequency);

    // ---- Шкала баланса мощности ----
    y += 5;
    const float imbalance = (gd.reactor.actualPower + gd.phes.outputPower) - gd.demand.currentDemand;
    const float MAX_IMBAL = 500.0f;
    const float balPct = Clamp(imbalance / MAX_IMBAL, -1.0f, 1.0f);

    const int bX = pad;
    const int bW = 780;
    const int bH = 12;
    const int bY = y;
    const int mX = bX + bW / 2;

    DrawRectangle(bX, bY, bW / 2, bH, { 160, 30, 30, 80 });
    DrawRectangle(mX, bY, bW / 2, bH, { 30, 160, 60, 80 });
    DrawRectangleLines(bX, bY, bW, bH, { 80, 90, 120, 200 });

    int nX = mX + (int)(balPct * (bW / 2));
    nX = Clamp(nX, bX + 2, bX + bW - 3);
    DrawRectangle(nX - 1, bY - 2, 3, bH + 4, { 255, 225, 50, 255 });

    y = bY + bH + 5;

    // ---- Время выживания ----
    DrawRow(pad, y, lineH, fs, col, "Время:%.1fs", gd.survivalTime);
}

// ---------------------------------------------------------------------------
// Главная функция отрисовки сцены и интерфейса
// ---------------------------------------------------------------------------
void Draw(const GameData& gd)
{
    BeginDrawing();
    ClearBackground({ 5, 8, 18, 255 });

    // ---- Рендеринг интерфейса в текстуру монитора (2D) ----
    BeginTextureMode(gd.monitorScreen);
    DrawMonitorContent(const_cast<GameData&>(gd));
    EndTextureMode();

    // ---- Рендеринг 3D-сцены с текстурированным монитором ----
    BeginMode3D(gd.scene.cam);
    DrawScene3D(gd);
    EndMode3D();

    // ---- Подписи кнопок — проецирование с помощью GetWorldToScreen (2D-проход) ----
    for (int i = 0; i < g_ButtonCount; ++i)
    {
        const Button3D& b = g_Buttons[i];
        // Проецируем точку чуть выше верхней грани кнопки
        Vector3 labelPt = { b.position.x,
                               b.position.y + b.size.y * 0.5f + 0.04f,
                               b.position.z };

        // Исключаем проецирование точек, находящихся позади камеры
        Vector3 camPos = gd.scene.cam.position;
        Vector3 camTarget = gd.scene.cam.target;
        Vector3 lookDir = { camTarget.x - camPos.x, camTarget.y - camPos.y, camTarget.z - camPos.z };
        Vector3 toPt = { labelPt.x - camPos.x, labelPt.y - camPos.y, labelPt.z - camPos.z };
        float dot = toPt.x * lookDir.x + toPt.y * lookDir.y + toPt.z * lookDir.z;

        // Пропускаем, если точка позади камеры
        if (dot <= 0.0f) continue;

        Vector2 scr = GetWorldToScreen(labelPt, gd.scene.cam);
        int     tw = MeasureTextEx(cyrillicFont, b.label, 13, 0).x;

        // Рисуем только если спроецированная точка находится на экране с запасом
        if (scr.x > -50 && scr.x < SCREEN_W + 50 && scr.y > -50 && scr.y < SCREEN_H + 50)
        {
            DrawRectangle((int)scr.x - tw / 2 - 3, (int)scr.y - 2, tw + 6, 17,
                { 0, 0, 0, 140 });
            DrawTextEx(cyrillicFont, b.label, { scr.x - tw / 2, scr.y }, 13, 0, WHITE);
        }
    }

    // ---- Баннер сообщения о событии ----
    if (gd.eventTimer > 0.0f && gd.eventMsg[0] != '\0')
    {
        const int   mfs = 24;
        const int   mw = MeasureTextEx(cyrillicFont, gd.eventMsg, mfs, 0).x;
        const int   mx = SCREEN_W / 2 - mw / 2;
        const int   my = 100;
        const Color mcol = { 255, 100, 100, 255 };

        DrawRectangle(mx - 10, my - 5, mw + 20, 35, { 50, 20, 20, 200 });
        DrawRectangleLines(mx - 10, my - 5, mw + 20, 35, { 255, 100, 100, 150 });
        DrawTextEx(cyrillicFont, gd.eventMsg, { (float)mx, (float)my }, mfs, 0, mcol);
    }

    // ---- Счетчик FPS (отладка) ----
    DrawFPS(SCREEN_W - 120, 36);

    // ---- Перекрестие прицела по центру ----
    const int crosshairSize = 4;
    const int centerX = SCREEN_W / 2;
    const int centerY = SCREEN_H / 2;
    DrawRectangle(centerX - crosshairSize, centerY - 1, crosshairSize * 2, 2, WHITE);
    DrawRectangle(centerX - 1, centerY - crosshairSize, 2, crosshairSize * 2, WHITE);

    EndDrawing();
}

// ---------------------------------------------------------------------------
// Функции игровых состояний
// ---------------------------------------------------------------------------

// Состояние загрузки
void UpdateLoading(float dt)
{
    loadingTimer += dt;
    loadingProgress = (loadingTimer / 5.0f) * 100.0f; // 10 секунд на загрузку
    if (loadingTimer >= 5.0f)
    {
        currentState = STATE_MENU;
    }
}

void DrawLoading()
{
    BeginDrawing();
    ClearBackground(BLACK);

    // 1. Фон
    if (loadingScreenTexture.id != 0)
    {
        DrawTexturePro(loadingScreenTexture,
            { 0, 0, (float)loadingScreenTexture.width, (float)loadingScreenTexture.height },
            { 0, 0, (float)GetScreenWidth(), (float)GetScreenHeight() }, { 0, 0 }, 0.0f, WHITE);
    }

    // 2. Параметры шкалы
    const int barWidth = 400;
    const int barHeight = 25;
    const int barX = GetScreenWidth() / 2 - barWidth / 2;
    const int barY = GetScreenHeight() - 100; // Сдвигаем вниз, чтобы не перекрывать центр

    // 3. Подложка под шкалу (эффект стекла)
    DrawRectangle(barX - 10, barY - 10, barWidth + 20, barHeight + 20, Fade(BLACK, 0.3f));
    DrawRectangleLines(barX - 10, barY - 10, barWidth + 20, barHeight + 20, Fade(WHITE, 0.2f));

    // 4. Шкала загрузки
    int fillWidth = (int)(loadingProgress / 100.0f * barWidth);

    // Фон шкалы
    DrawRectangle(barX, barY, barWidth, barHeight, Fade(BLACK, 0.5f));

    // Заполнение шкалы (бирюзовый цвет)
    if (fillWidth > 0)
    {
        // Основной цвет (используем тот же CYAN/SKYBLUE, что в меню)
        DrawRectangle(barX, barY, fillWidth, barHeight, SKYBLUE);
        // Световой блик
        DrawRectangle(barX, barY, fillWidth, 4, WHITE);
    }

    // Рамка шкалы
    DrawRectangleLines(barX, barY, barWidth, barHeight, WHITE);

    // 5. Текст процентов
    char percentText[32];
    snprintf(percentText, sizeof(percentText), "ЗАГРУЗКА %.0f%%", loadingProgress);

    // Текст с "тенью" для читаемости
    Vector2 textSize = MeasureTextEx(cyrillicFont, percentText, 20, 2);
    DrawTextEx(cyrillicFont, percentText,
        { (float)(GetScreenWidth() / 2 - textSize.x / 2), (float)(barY - 40) },
        20, 2, RAYWHITE);

    EndDrawing();
}

// Правила игры
void UpdateRules(GameData& gd)
{
    // ENTER начинает игру
    if (IsKeyPressed(KEY_ENTER))
    {
        currentState = STATE_GAME;
        return;
    }
    // ESC возвращает в меню
    if (IsKeyPressed(KEY_ESCAPE))
    {
        currentState = STATE_MENU;
        menuSelectedOption = 0;
        return;
    }
}

void DrawRules()
{
    BeginDrawing();
    ClearBackground({ 5, 8, 18, 255 });

    // Отрисовка фона (оставляем без изменений)
    if (rulesBackgroundTexture.id != 0)
    {
        DrawTexturePro(rulesBackgroundTexture, { 0, 0, (float)rulesBackgroundTexture.width, (float)rulesBackgroundTexture.height },
            { 0, 0, (float)GetScreenWidth(), (float)GetScreenHeight() }, { 0, 0 }, 0.0f, WHITE);
    }

    // --- ИЗМЕНЕНИЯ: ПАНЕЛЬ СЛЕВА ---
    const int panelWidth = 680; // Немного сузим, чтобы смотрелось компактнее
    const int panelHeight = 600;
    const int panelX = 40;      // Фиксированный отступ слева
    const int panelY = GetScreenHeight() / 2 - panelHeight / 2;

    DrawRectangle(panelX, panelY, panelWidth, panelHeight, Fade(BLACK, 0.4f));
    DrawRectangleLines(panelX, panelY, panelWidth, panelHeight, Fade(WHITE, 0.3f));

    // Текстовая информация теперь привязана к левому отступу
    const int pad = panelX + 30; // Текст будет внутри панели
    const int fs = 22;
    const int lineH = 32;
    int y = panelY + 30;

    Color txtCol = RAYWHITE;

    DrawTextEx(cyrillicFont, "СМЕНА: 5 МИНУТ", { (float)pad, (float)y }, 28, 0, SKYBLUE);
    y += 40;

    DrawTextEx(cyrillicFont, "УСЛОВИЯ ПОБЕДЫ:", { (float)pad, (float)y }, 24, 0, SKYBLUE);
    y += lineH;
    DrawTextEx(cyrillicFont, "Удерживать частоту сети в диапазоне 45-55 Гц", { (float)pad, (float)y }, fs, 0, txtCol);
    y += lineH;
    DrawTextEx(cyrillicFont, "Продержаться 5 минут без отклонений", { (float)pad, (float)y }, fs, 0, txtCol);
    y += lineH + 20;

    DrawTextEx(cyrillicFont, "УСЛОВИЯ ПОРАЖЕНИЯ:", { (float)pad, (float)y }, 24, 0, SKYBLUE);
    y += lineH;
    DrawTextEx(cyrillicFont, "Частота <= 45 Гц или >= 55 Гц = ПОРАЖЕНИЕ", { (float)pad, (float)y }, fs, 0, txtCol);
    y += lineH + 20;

    DrawTextEx(cyrillicFont, "УПРАВЛЕНИЕ (3D-ПУЛЬТ):", { (float)pad, (float)y }, 24, 0, SKYBLUE);
    y += lineH;
    DrawTextEx(cyrillicFont, "РЕАКТОР+ / РЕАКТОР- : мощность реактора +/- 80 МВт", { (float)pad, (float)y }, fs, 0, txtCol);
    y += lineH;
    DrawTextEx(cyrillicFont, "ТУРБИНА : режим генерации (ГАЭС вырабатывает энергию)", { (float)pad, (float)y }, fs, 0, txtCol);
    y += lineH;
    DrawTextEx(cyrillicFont, "НАСОС : режим закачки (ГАЭС потребляет энергию)", { (float)pad, (float)y }, fs, 0, txtCol);
    y += lineH;
    DrawTextEx(cyrillicFont, "ПЕРЕЗАПУСК : начать игру заново", { (float)pad, (float)y }, fs, 0, txtCol);
    y += lineH;
    DrawTextEx(cyrillicFont, "Наведение мышью - выбор кнопки | E или клик - активация", { (float)pad, (float)y }, fs, 0, txtCol);
    y += lineH;
    DrawTextEx(cyrillicFont, "ESC - меню", { (float)pad, (float)y }, fs, 0, txtCol);

    // Инструкция внизу
    y += 40; // Дополнительный воздух перед подсказкой
    DrawTextEx(cyrillicFont, "Нажмите ENTER для начала или ESC для возврата в меню",
        { (float)pad, (float)y }, 22, 0, LIGHTGRAY);

    EndDrawing();
}

// Menu state
void UpdateMenu(GameData& gd)
{
    // ESC возвращает в игру (если игра была запущена)
    if (IsKeyPressed(KEY_ESCAPE))
    {
        if (gameStarted)
        {
            currentState = STATE_GAME;
        }
        return;
    }

    int maxOption = gameStarted ? 2 : 1; // 0=Продолжить (если доступно), 1=Новая игра, 2=Выход

    if (IsKeyPressed(KEY_UP))
    {
        menuSelectedOption = (menuSelectedOption - 1 + maxOption + 1) % (maxOption + 1);
    }
    if (IsKeyPressed(KEY_DOWN))
    {
        menuSelectedOption = (menuSelectedOption + 1) % (maxOption + 1);
    }

    if (IsKeyPressed(KEY_ENTER))
    {
        if (gameStarted && menuSelectedOption == 0)
        {
            // Продолжить
            currentState = STATE_GAME;
        }
        else if ((!gameStarted && menuSelectedOption == 0) || (gameStarted && menuSelectedOption == 1))
        {
            // Новая игра - инициализируем GameData и показываем правила
            gd.Init();
            currentState = STATE_RULES;
            gameStarted = true;
        }
        else if ((!gameStarted && menuSelectedOption == 1) || (gameStarted && menuSelectedOption == 2))
        {
            // Выход
            gd.running = false;
        }
    }
}

void DrawMenu()
{
    BeginDrawing();
    ClearBackground(BLACK);

    const Color CYAN = { 0, 255, 255, 255 };

    // 1. Отрисовка фона
    if (menuBackgroundTexture.id != 0)
    {
        DrawTexturePro(menuBackgroundTexture,
            { 0, 0, (float)menuBackgroundTexture.width, (float)menuBackgroundTexture.height },
            { 0, 0, (float)GetScreenWidth(), (float)GetScreenHeight() }, { 0, 0 }, 0.0f, WHITE);
    }

    // Добавляем полупрозрачную подложку под меню для читаемости
    DrawRectangle(GetScreenWidth() / 2 - 200, GetScreenHeight() / 2 - 120, 400, 350, Fade(BLACK, 0.4f));

    // 2. Заголовок (Жирный - здесь просто увеличим размер, если шрифт позволяет, или оставим как есть)
    DrawTextEx(cyrillicFont, "АЭС Диспетчер", { (float)(GetScreenWidth() / 2 - 180), (float)(GetScreenHeight() / 2 - 100) }, 50, 2, WHITE);

    // 3. Логика пунктов меню
    int y = GetScreenHeight() / 2;
    int menuX = GetScreenWidth() / 2 - 110;

    // Вспомогательная функция для отрисовки кнопки с рамкой
    auto DrawMenuButton = [&](const char* text, int posX, int posY, bool isSelected) {
        Color textColor = isSelected ? RAYWHITE : LIGHTGRAY;
        Color boxColor = isSelected ? Fade(SKYBLUE, 0.3f) : Fade(WHITE, 0.1f);
        Color borderColor = isSelected ? CYAN : GRAY;

        // Рисуем рамку кнопки
        DrawRectangle(posX - 10, posY - 5, 220, 40, boxColor);
        DrawRectangleLines(posX - 10, posY - 5, 220, 40, borderColor);

        // Рисуем текст
        DrawTextEx(cyrillicFont, text, { (float)posX, (float)posY }, 30, 1, textColor);
        };

    // Отрисовка в зависимости от состояния
    if (gameStarted)
    {
        DrawMenuButton("Продолжить", menuX, y, menuSelectedOption == 0);
        DrawMenuButton("Новая игра", menuX, y + 60, menuSelectedOption == 1);
        DrawMenuButton("Выход", menuX, y + 120, menuSelectedOption == 2);
    }
    else
    {
        DrawMenuButton("Новая игра", menuX, y, menuSelectedOption == 0);
        DrawMenuButton("Выход", menuX, y + 60, menuSelectedOption == 1);
    }

    EndDrawing();
}

// Игровое состояние (перенаправляет вызовы к существующим Update/Draw)
void UpdateGame(GameData& gd, float dt)
{
    HandleInput(gd, dt);
    Update(gd, dt);
}

void DrawGame(const GameData& gd)
{
    Draw(gd);
}

// Экран результатов
void UpdateResults(GameData& gd)
{
    if (IsKeyPressed(KEY_UP))
    {
        resultMenuOption = 0;
    }
    if (IsKeyPressed(KEY_DOWN))
    {
        resultMenuOption = 1;
    }

    if (IsKeyPressed(KEY_ENTER))
    {
        if (resultMenuOption == 0)
        {
            // Вернуться в меню
            currentState = STATE_MENU;
            gameResult = RESULT_NONE;
            resultMenuOption = 0;
        }
        else if (resultMenuOption == 1)
        {
            // Сохранить статистику в файл
            const char* filename = "game_statistics.txt";
            FILE* file = fopen(filename, "a");
            if (file != nullptr)
            {
                fprintf(file, " РЕЗУЛЬТАТ ИГРЫ \n");
                fprintf(file, "Результат: %s\n", (gameResult == RESULT_WIN) ? "ПОБЕДА" : "ПОРАЖЕНИЕ");
                fprintf(file, "Время смены: %.1f сек\n", gd.survivalTime);
                fprintf(file, "Финальная частота: %.2f Гц\n", gd.freq.frequency);
                fprintf(file, "Мощность реактора: %.0f МВт\n", gd.reactor.actualPower);
                fprintf(file, "Спрос: %.0f МВт\n", gd.demand.currentDemand);
                fprintf(file, "Режим ГАЭС: %s\n", PhesModeStr(gd.phes.mode));
                fclose(file);

                char fullPath[512];
                const char* appDir = GetApplicationDirectory();

                snprintf(fullPath, sizeof(fullPath), "%s%s", appDir, filename);

                char logMsg[512];
                snprintf(logMsg, sizeof(logMsg), "Статистика сохранена в: %s", fullPath);
                TraceLog(LOG_INFO, logMsg);

                gd.eventMsg = "Статистика сохранена!";
                gd.eventTimer = 3.0f;
            }
            else
            {
                TraceLog(LOG_ERROR, "Не удалось открыть файл для сохранения статистики");
                gd.eventMsg = "Ошибка сохранения!";
                gd.eventTimer = 2.0f;
            }

            // Вернуться в меню
            currentState = STATE_MENU;
            gameResult = RESULT_NONE;
            resultMenuOption = 0;
        }
    }
}

void DrawResults(const GameData& gd)
{
    BeginDrawing();
    ClearBackground({ 10, 20, 50, 255 });

    // Отрисовка фонового изображения в зависимости от результата
    if (gameResult == RESULT_LOSE && gameOverTexture.id != 0)
    {
        Rectangle source = { 0.0f, 0.0f, (float)gameOverTexture.width, (float)gameOverTexture.height };
        Rectangle dest = { 0.0f, 0.0f, (float)GetScreenWidth(), (float)GetScreenHeight() };
        Vector2 origin = { 0.0f, 0.0f };
        DrawTexturePro(gameOverTexture, source, dest, origin, 0.0f, WHITE);
    }
    else if (gameResult == RESULT_WIN && winTexture.id != 0)
    {
        Rectangle source = { 0.0f, 0.0f, (float)winTexture.width, (float)winTexture.height };
        Rectangle dest = { 0.0f, 0.0f, (float)GetScreenWidth(), (float)GetScreenHeight() };
        Vector2 origin = { 0.0f, 0.0f };
        DrawTexturePro(winTexture, source, dest, origin, 0.0f, WHITE);
    }

    // --- ДОБАВЛЯЕМ ПОДЛОЖКУ И РАМКУ (в том же стиле, что в меню) ---
    const int panelWidth = 400;
    const int panelHeight = 320; // Подбираем высоту под ваш объем текста
    const int panelX = 30;       // Немного отступаем от левого края
    int panelY = SCREEN_H - 280; // Чуть выше ваших координат текста

    // Полупрозрачная подложка
    DrawRectangle(panelX, panelY, panelWidth, panelHeight, Fade(BLACK, 0.5f));
    // Рамка (можно использовать белый цвет или SKYBLUE из меню)
    DrawRectangleLines(panelX, panelY, panelWidth, panelHeight, Fade(WHITE, 0.4f));

    // Далее идет ВАШ код отрисовки текста (он будет теперь поверх этой панели)
    const int fs = 24;
    const int lineH = 35;
    const int padLeft = 50;
    int y = SCREEN_H - 260; // Убедитесь, что y >= panelY + 20

    Color textCol = WHITE;

    // Заголовок
    DrawTextEx(cyrillicFont, "ИТОГОВАЯ СТАТИСТИКА", { (float)padLeft, (float)y }, 30, 0, textCol);
    y += 40;

    // Статистика
    char statLine1[128];
    snprintf(statLine1, sizeof(statLine1), "Время смены: %.1f сек", gd.survivalTime);
    DrawTextEx(cyrillicFont, statLine1, { (float)padLeft, (float)y }, fs, 0, textCol);
    y += lineH;

    char statLine2[128];
    snprintf(statLine2, sizeof(statLine2), "Финальная частота: %.2f Гц", gd.freq.frequency);
    DrawTextEx(cyrillicFont, statLine2, { (float)padLeft, (float)y }, fs, 0, textCol);
    y += lineH;

    char statLine3[128];
    snprintf(statLine3, sizeof(statLine3), "Мощность реактора: %.0f МВт", gd.reactor.actualPower);
    DrawTextEx(cyrillicFont, statLine3, { (float)padLeft, (float)y }, fs, 0, textCol);
    y += lineH;

    char statLine4[128];
    snprintf(statLine4, sizeof(statLine4), "Спрос: %.0f МВт", gd.demand.currentDemand);
    DrawTextEx(cyrillicFont, statLine4, { (float)padLeft, (float)y }, fs, 0, textCol);
    y += lineH + 15;

    // Меню выбора
    Color highlightCol = { 255, 200, 50, 255 };
    Color normalCol = { 200, 200, 200, 255 };
    Color menuCol1 = (resultMenuOption == 0) ? highlightCol : normalCol;
    Color menuCol2 = (resultMenuOption == 1) ? highlightCol : normalCol;

    const char* option1 = "Вернуться в меню";
    const char* option2 = "Сохранить статистику";

    DrawTextEx(cyrillicFont, option1, { (float)padLeft, (float)y }, fs, 0, menuCol1);
    y += lineH;

    DrawTextEx(cyrillicFont, option2, { (float)padLeft, (float)y }, fs, 0, menuCol2);

    // Указатель выбора
    int indicatorY = SCREEN_H - 260 + 40 + (4 * lineH) + 15 + (resultMenuOption * lineH);
    DrawTextEx(cyrillicFont, ">", { (float)(padLeft - 30), (float)indicatorY }, fs, 0, highlightCol);

    EndDrawing();
}

// Диспетчер состояний
void UpdateGameState(GameData& gd, float dt)
{
    // Переключение полноэкранного режима (работает во всех состояниях)
    if (IsKeyPressed(KEY_F11))
    {
        ToggleFullscreen();
    }

    switch (currentState)
    {
    case STATE_LOADING:
        UpdateLoading(dt);
        break;
    case STATE_MENU:
        UpdateMenu(gd);
        break;
    case STATE_RULES:
        UpdateRules(gd);
        break;
    case STATE_GAME:
        if (IsKeyPressed(KEY_F1))
        {
            // Если игра в полном экране, возвращаем её в оконный режим
            if (IsWindowFullscreen())
            {
                ToggleFullscreen();
            }

            ShellExecuteA(nullptr, "open", "NewProject.chm", nullptr, nullptr, 1);
        }
        UpdateGame(gd, dt);
        break;
    case STATE_RESULTS:
        UpdateResults(gd);
        break;
    }
}

void DrawGameState(const GameData& gd)
{
    switch (currentState)
    {
    case STATE_LOADING:
        DrawLoading();
        break;
    case STATE_MENU:
        DrawMenu();
        break;
    case STATE_RULES:
        DrawRules();
        break;
    case STATE_GAME:
        DrawGame(gd);
        break;
    case STATE_RESULTS:
        DrawResults(gd);
        break;
    }
}

// ---------------------------------------------------------------------------
// Точка входа
// ---------------------------------------------------------------------------
int WinMain()
{
    SetConfigFlags(FLAG_MSAA_4X_HINT | FLAG_WINDOW_RESIZABLE);
    InitWindow(SCREEN_W, SCREEN_H, "АЭС Диспетчер Pro");
    ToggleFullscreen(); // Запуск в полноэкранном режиме
    int monitorRefreshRate = GetMonitorRefreshRate(GetCurrentMonitor());
    if (monitorRefreshRate <= 0) monitorRefreshRate = 60; // Защита от ошибок
    SetTargetFPS(monitorRefreshRate);
    SetExitKey(KEY_NULL); // Отключить закрытие окна по ESC

    // Загрузка кириллического шрифта (Cousine-Regular.ttf должен находиться в директории проекта)
    // Явно указываем глифы для поддержки кириллицы
    int fontGlyphs[512];
    int glyphCount = 0;
    // Добавляем базовый ASCII
    for (int i = 32; i < 127; i++) fontGlyphs[glyphCount++] = i;
    // Добавляем весь диапазон кириллицы (0x0400-0x04FF)
    for (int i = 0x0400; i <= 0x04FF; i++) fontGlyphs[glyphCount++] = i;

    cyrillicFont = LoadFontEx("Cousine-Regular.ttf", 48, fontGlyphs, glyphCount);
    if (cyrillicFont.texture.id == 0)
    {
        // Переход на стандартный шрифт в случае отсутствия файла
        cyrillicFont = GetFontDefault();
    }

    // Загрузка изображения экрана загрузки
    Image img = LoadImage("for_kyrsovoi\\forloadmenu.BMP");
    if (img.width == 0)
    {
        img = LoadImage("for_kyrsovoi/forloadmenu.BMP");
    }
    if (img.width == 0)
    {
        img = LoadImage(".\\for_kyrsovoi\\forloadmenu.BMP");
    }

    if (img.width > 0)
    {
        loadingScreenTexture = LoadTextureFromImage(img);
        UnloadImage(img);
    }

    if (loadingScreenTexture.id == 0)
    {
        loadingScreenTexture = LoadTexture("for_kyrsovoi\\forloadmenu.BMP");
    }
    if (loadingScreenTexture.id == 0)
    {
        loadingScreenTexture = LoadTexture("for_kyrsovoi/forloadmenu.BMP");
    }
    if (loadingScreenTexture.id != 0)
    {
        TraceLog(LOG_INFO, "Loading screen loaded: %dx%d", loadingScreenTexture.width, loadingScreenTexture.height);
    }
    else
    {
        TraceLog(LOG_ERROR, "Failed to load loading screen image");
    }

    // Загрузка фонового изображения меню
    Image menuImg = LoadImage("for_kyrsovoi\\menuforgame.BMP");
    if (menuImg.width == 0)
    {
        menuImg = LoadImage("for_kyrsovoi/menuforgame.BMP");
    }
    if (menuImg.width == 0)
    {
        menuImg = LoadImage(".\\for_kyrsovoi\\menuforgame.BMP");
    }

    if (menuImg.width > 0)
    {
        menuBackgroundTexture = LoadTextureFromImage(menuImg);
        UnloadImage(menuImg);
    }

    if (menuBackgroundTexture.id == 0)
    {
        menuBackgroundTexture = LoadTexture("for_kyrsovoi\\menuforgame.BMP");
    }
    if (menuBackgroundTexture.id == 0)
    {
        menuBackgroundTexture = LoadTexture("for_kyrsovoi/menuforgame.BMP");
    }
    if (menuBackgroundTexture.id != 0)
    {
        TraceLog(LOG_INFO, "Menu background loaded: %dx%d", menuBackgroundTexture.width, menuBackgroundTexture.height);
    }
    else
    {
        TraceLog(LOG_ERROR, "Failed to load menu background image");
    }

    // Загрузка фонового изображения записки (пояснительной записки)
    Image rulesImg = LoadImage("for_kyrsovoi\\ForExplanatoryNote.bmp");
    if (rulesImg.width == 0)
    {
        rulesImg = LoadImage("for_kyrsovoi/ForExplanatoryNote.bmp");
    }
    if (rulesImg.width == 0)
    {
        rulesImg = LoadImage(".\\for_kyrsovoi\\ForExplanatoryNote.bmp");
    }

    if (rulesImg.width > 0)
    {
        rulesBackgroundTexture = LoadTextureFromImage(rulesImg);
        UnloadImage(rulesImg);
    }

    if (rulesBackgroundTexture.id == 0)
    {
        rulesBackgroundTexture = LoadTexture("for_kyrsovoi\\ForExplanatoryNote.bmp");
    }
    if (rulesBackgroundTexture.id == 0)
    {
        rulesBackgroundTexture = LoadTexture("for_kyrsovoi/ForExplanatoryNote.bmp");
    }
    if (rulesBackgroundTexture.id != 0)
    {
        TraceLog(LOG_INFO, "Rules background loaded: %dx%d", rulesBackgroundTexture.width, rulesBackgroundTexture.height);
    }
    else
    {
        TraceLog(LOG_WARNING, "Rules background image not found, using default");
    }

    // Загрузка текстуры GameOver (поражение)
    Image gameOverImg = LoadImage("for_kyrsovoi\\GameOver.bmp");
    if (gameOverImg.width == 0)
    {
        gameOverImg = LoadImage("for_kyrsovoi/GameOver.bmp");
    }
    if (gameOverImg.width == 0)
    {
        gameOverImg = LoadImage(".\\for_kyrsovoi\\GameOver.bmp");
    }

    if (gameOverImg.width > 0)
    {
        gameOverTexture = LoadTextureFromImage(gameOverImg);
        UnloadImage(gameOverImg);
    }

    if (gameOverTexture.id == 0)
    {
        gameOverTexture = LoadTexture("for_kyrsovoi\\GameOver.bmp");
    }
    if (gameOverTexture.id == 0)
    {
        gameOverTexture = LoadTexture("for_kyrsovoi/GameOver.bmp");
    }
    if (gameOverTexture.id != 0)
    {
        TraceLog(LOG_INFO, "GameOver texture loaded: %dx%d", gameOverTexture.width, gameOverTexture.height);
    }
    else
    {
        TraceLog(LOG_WARNING, "GameOver texture not found");
    }

    // Загрузка текстуры WIN (победа)
    Image winImg = LoadImage("for_kyrsovoi\\WIN.png");
    if (winImg.width == 0)
    {
        winImg = LoadImage("for_kyrsovoi/WIN.png");
    }
    if (winImg.width == 0)
    {
        winImg = LoadImage(".\\for_kyrsovoi\\WIN.png");
    }

    if (winImg.width > 0)
    {
        winTexture = LoadTextureFromImage(winImg);
        UnloadImage(winImg);
    }

    if (winTexture.id == 0)
    {
        winTexture = LoadTexture("for_kyrsovoi\\WIN.png");
    }
    if (winTexture.id == 0)
    {
        winTexture = LoadTexture("for_kyrsovoi/WIN.png");
    }
    if (winTexture.id != 0)
    {
        TraceLog(LOG_INFO, "WIN texture loaded: %dx%d", winTexture.width, winTexture.height);
    }
    else
    {
        TraceLog(LOG_WARNING, "WIN texture not found");
    }

    GameData gd;
    gd.Init();

    while (!WindowShouldClose() && gd.running)
    {
        const float dt = GetFrameTime();

        if (currentState == STATE_GAME)
        {
            for (int i = 0; i < g_ButtonCount; ++i)
            {
                g_Buttons[i].CheckHover(gd.scene.cam);
                g_Buttons[i].UpdateAnimation(dt);
            }
        }

        UpdateGameState(gd, dt);
        DrawGameState(gd);
    }

    UnloadRenderTexture(gd.monitorScreen);
    UnloadFont(cyrillicFont);
    if (loadingScreenTexture.id != 0)
    {
        UnloadTexture(loadingScreenTexture);
    }
    if (menuBackgroundTexture.id != 0)
    {
        UnloadTexture(menuBackgroundTexture);
    }
    if (rulesBackgroundTexture.id != 0)
    {
        UnloadTexture(rulesBackgroundTexture);
    }
    if (gameOverTexture.id != 0)
    {
        UnloadTexture(gameOverTexture);
    }
    if (winTexture.id != 0)
    {
        UnloadTexture(winTexture);
    }
    CloseWindow();
    return 0;
}