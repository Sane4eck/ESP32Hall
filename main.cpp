// #include <Arduino.h>

// constexpr uint8_t HALL_PIN = 25;

// int lastState = -1;

// void setup()
// {
//     Serial.begin(115200);

//     pinMode(HALL_PIN, INPUT);

//     delay(100);

//     Serial.println();
//     Serial.println("=== HALL SENSOR TEST ===");
//     Serial.println("GPIO25");
//     Serial.println("HIGH = output released");
//     Serial.println("LOW  = sensor active");
//     Serial.println();
// }

// void loop()
// {
//     int state = digitalRead(HALL_PIN);

//     // Виводимо тільки при зміні стану
//     if (state != lastState)
//     {
//         lastState = state;

//         Serial.print("OUT = ");

//         if (state == LOW)
//         {
//             Serial.println("LOW  -> ACTIVE");
//         }
//         else
//         {
//             Serial.println("HIGH -> INACTIVE");
//         }
//     }

//     delay(1);
// }

#include <Arduino.h>

// ======================================================
// НАЛАШТУВАННЯ
// ======================================================

constexpr uint8_t HALL_PIN = 25;

// Кількість імпульсів на один оберт
constexpr uint32_t PULSES_PER_REV = 1;

// Частота виводу ~10 Гц
constexpr uint32_t REPORT_PERIOD_US = 100000;

// Робочий діапазон
constexpr float MIN_RPM = 500.0f;

// Максимально фізично допустимі оберти.
// Все, що вище — вважаємо хибним імпульсом.
constexpr float MAX_VALID_RPM = 170000.0f;

// Перемикання режимів
constexpr float SWITCH_TO_COUNT_RPM  = 30000.0f;
constexpr float SWITCH_TO_PERIOD_RPM = 25000.0f;

// Мінімально допустимий час між імпульсами
constexpr uint32_t MIN_VALID_PERIOD_US =
    (uint32_t)(60000000.0f /
    (MAX_VALID_RPM * PULSES_PER_REV));


// ======================================================
// ISR DATA
// ======================================================

volatile uint32_t totalPulses = 0;

volatile uint32_t lastPulseUs = 0;
volatile uint32_t lastPeriodUs = 0;

volatile bool periodValid = false;

portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;


// ======================================================
// РЕЖИМ
// ======================================================

enum MeasureMode
{
    MODE_PERIOD,
    MODE_COUNT
};

MeasureMode mode = MODE_PERIOD;


// ======================================================
// INTERRUPT
// ======================================================

void IRAM_ATTR hallISR()
{
    uint32_t now = micros();

    portENTER_CRITICAL_ISR(&mux);

    // Перший імпульс
    if (lastPulseUs == 0)
    {
        lastPulseUs = now;
        totalPulses++;

        portEXIT_CRITICAL_ISR(&mux);
        return;
    }

    uint32_t dt = now - lastPulseUs;

    // --------------------------------------------
    // Захист від дуже коротких хибних імпульсів
    // --------------------------------------------

    if (dt < MIN_VALID_PERIOD_US)
    {
        portEXIT_CRITICAL_ISR(&mux);
        return;
    }

    lastPeriodUs = dt;
    lastPulseUs = now;

    totalPulses++;

    periodValid = true;

    portEXIT_CRITICAL_ISR(&mux);
}


// ======================================================
// SETUP
// ======================================================

void setup()
{
    Serial.begin(115200);

    pinMode(HALL_PIN, INPUT);

    attachInterrupt(
        digitalPinToInterrupt(HALL_PIN),
        hallISR,
        RISING
    );

    delay(500);

    Serial.println();
    Serial.println("=== ESP32 RPM METER ===");

    Serial.printf(
        "PPR: %lu\n",
        PULSES_PER_REV
    );

    Serial.printf(
        "Valid RPM: %.0f ... %.0f\n",
        MIN_RPM,
        MAX_VALID_RPM
    );

    Serial.printf(
        "Min period: %lu us\n",
        MIN_VALID_PERIOD_US
    );

    Serial.println();
}


// ======================================================
// LOOP
// ======================================================

void loop()
{
    static uint32_t lastReportUs = micros();
    static uint32_t lastPulseCount = 0;

    uint32_t now = micros();

    uint32_t reportDt =
        now - lastReportUs;

    if (reportDt < REPORT_PERIOD_US)
        return;


    // ==================================================
    // КОПІЮЄМО ISR DATA
    // ==================================================

    uint32_t pulses;
    uint32_t pulseUs;
    uint32_t periodUs;
    bool valid;

    portENTER_CRITICAL(&mux);

    pulses   = totalPulses;
    pulseUs  = lastPulseUs;
    periodUs = lastPeriodUs;
    valid    = periodValid;

    portEXIT_CRITICAL(&mux);


    // ==================================================
    // КІЛЬКІСТЬ ІМПУЛЬСІВ ЗА ОСТАННЄ ВІКНО
    // ==================================================

    uint32_t pulseDelta =
        pulses - lastPulseCount;

    lastPulseCount = pulses;


    // ==================================================
    // COUNT RPM
    // ==================================================

    float rpmCount = 0.0f;

    if (pulseDelta > 0)
    {
        rpmCount =
            ((float)pulseDelta * 60000000.0f) /
            ((float)reportDt * PULSES_PER_REV);
    }


    // ==================================================
    // PERIOD RPM
    // ==================================================

    float rpmPeriod = 0.0f;

    uint32_t sinceLastPulse = 0;

    if (pulseUs != 0)
        sinceLastPulse = now - pulseUs;


    if (valid && periodUs > 0)
    {
        /*
         * При уповільненні новий імпульс ще не прийшов.
         *
         * Тому якщо після останнього імпульсу вже минуло
         * більше часу, ніж попередній період,
         * використовуємо цей час.
         *
         * Це прибирає "зависання" RPM при зупинці.
         */

        uint32_t effectivePeriod = periodUs;

        if (sinceLastPulse > effectivePeriod)
            effectivePeriod = sinceLastPulse;


        rpmPeriod =
            60000000.0f /
            ((float)effectivePeriod *
             PULSES_PER_REV);
    }

    // // На великих RPM обидва методи повинні давати
    // // хоча б приблизно однаковий результат.
    // if (rpmPeriod >= SWITCH_TO_COUNT_RPM)
    // {
    //     if (rpmCount < rpmPeriod * 0.5f ||
    //         rpmCount > rpmPeriod * 1.5f)
    //     {
    //         rpmPeriod = 0.0f;
    //     }
    // }
    // ==================================================
    // ВИЗНАЧЕННЯ ЗУПИНКИ
    // ==================================================

    bool stopped = false;

    if (valid)
    {
        uint32_t stopTimeout =
            periodUs * 3;

        // Не менше 100 мс
        if (stopTimeout < 100000)
            stopTimeout = 100000;

        // Не більше 500 мс
        if (stopTimeout > 500000)
            stopTimeout = 500000;

        if (sinceLastPulse > stopTimeout)
            stopped = true;
    }


    // ==================================================
    // ВИБІР РЕЖИМУ
    // ==================================================

    if (mode == MODE_PERIOD)
    {
        /*
         * Можемо перейти на COUNT:
         *
         * 1. за period RPM
         * або
         * 2. якщо одразу стартували на великих RPM
         */

        if (rpmPeriod >= SWITCH_TO_COUNT_RPM ||
            rpmCount >= SWITCH_TO_COUNT_RPM)
        {
            mode = MODE_COUNT;
        }
    }

    else
    {
        if (rpmCount < SWITCH_TO_PERIOD_RPM)
        {
            mode = MODE_PERIOD;
        }
    }


    // ==================================================
    // ФІНАЛЬНИЙ RPM
    // ==================================================

    float rpm = 0.0f;

    if (!stopped)
    {
        if (mode == MODE_PERIOD)
            rpm = rpmPeriod;
        else
            rpm = rpmCount;
    }


    // Дуже низькі значення вважаємо зупинкою
    if (rpm > 0 && rpm < MIN_RPM)
        rpm = 0;

        
    // // Якщо протягом вікна немає імпульсів
    // // і останній імпульс вже старий — гарантовано 0 RPM.
    // if (pulseDelta == 0 && sinceLastPulse > 150000)
    // {
    //     rpm = 0.0f;
    //     mode = MODE_PERIOD;
    // }

    // ==================================================
    // OUTPUT
    // ==================================================

    // Serial.printf(
    //     "RPM:%8.1f | mode:%s | period:%8.1f | count:%8.1f | P:%3lu | T:%lu us\n",
    //     rpm,
    //     mode == MODE_PERIOD ? "PERIOD" : "COUNT ",
    //     rpmPeriod,
    //     rpmCount,
    //     pulseDelta,
    //     periodUs
    // );

    uint32_t sampleMs = reportDt / 1000;

    Serial.printf(
        "RPM:%.1f,P:%lu,T:%lu\n",
        rpm,
        (unsigned long)pulseDelta,
        (unsigned long)sampleMs
    );


    lastReportUs = now;
}
