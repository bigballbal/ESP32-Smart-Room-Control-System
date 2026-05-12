#include <Wire.h>
#include <LiquidCrystal_I2C.h>

#define AC_ON_TEMP 30
#define AC_OFF_TEMP 25

typedef enum { MANUAL_MODE, AUTO_MODE } Mode;
typedef enum { LOG_INFO, LOG_WARN, LOG_ERROR, LOG_SYSTEM } Loglevel;

typedef struct {
    Mode mode;
    int light;
    int temperature;
    int ac;
} RoomSetting;

LiquidCrystal_I2C lcd(0x27, 16, 2);
const int lightPin = 12;
const int acPin = 13;

RoomSetting myRoom = {MANUAL_MODE, 0, 25, 0};

// --- Logger ---
void logger(Loglevel level, const char* message) {
    Serial.print(level == LOG_ERROR ? "[!] " : "[i] ");
    Serial.println(message);

    lcd.setCursor(0, 1);
    lcd.print("                ");
    lcd.setCursor(0, 1);
    lcd.print(message);
}

// --- Auto Control ---
void runAutoControl() {
    if (myRoom.temperature > AC_ON_TEMP) myRoom.ac = 1;
    else if (myRoom.temperature < AC_OFF_TEMP) myRoom.ac = 0;
}

// --- Output Layer ---
void renderHardware() {
    digitalWrite(lightPin, myRoom.light ? HIGH : LOW);
    digitalWrite(acPin,    myRoom.ac    ? HIGH : LOW);

    lcd.setCursor(0, 0);
    lcd.print("                "); // 先清第一行，避免殘字
    lcd.setCursor(0, 0);

    // 16格排法： "T:28 L:ON AC:ON " (16字元)
    lcd.print("T:");
    lcd.print(myRoom.temperature);
    lcd.print(" L:");
    lcd.print(myRoom.light ? "ON" : "OF");   // 2字元省空間
    lcd.print(" AC:");
    lcd.print(myRoom.ac   ? "ON" : "OF");
    // 右上角顯示模式
    lcd.setCursor(14, 0);
    lcd.print(myRoom.mode == AUTO_MODE ? " A" : " M");
}

// --- Setup ---
void setup() {
    Serial.begin(9600);
    lcd.init();
    lcd.backlight();
    pinMode(lightPin, OUTPUT);
    pinMode(acPin, OUTPUT);
    logger(LOG_SYSTEM, "System Ready");

    // 提示使用者可用指令
    Serial.println("Commands: a=AUTO m=MANUAL l=Light c=AC t<num>=Temp");
    Serial.println("Example: t28 sets temperature to 28");
}

// --- Loop ---
void loop() {
    if (Serial.available() > 0) {
        char cmd = Serial.read();

        switch (cmd) {
            case 'a':
                myRoom.mode = AUTO_MODE;
                logger(LOG_SYSTEM, "Mode: AUTO");
                break;

            case 'm':
                myRoom.mode = MANUAL_MODE;
                logger(LOG_SYSTEM, "Mode: MANUAL");
                break;

            // ★補丁1：燈光切換，只有 MANUAL 才能動
            case 'l':
                if (myRoom.mode == MANUAL_MODE) {
                    myRoom.light = !myRoom.light;         // toggle
                    logger(LOG_INFO, myRoom.light ? "Light ON" : "Light OFF");
                } else {
                    logger(LOG_ERROR, "AUTO mode! No manual ctrl");
                }
                break;

            // ★補丁2：AC 切換，只有 MANUAL 才能動
            case 'c':
                if (myRoom.mode == MANUAL_MODE) {
                    myRoom.ac = !myRoom.ac;               // toggle
                    logger(LOG_INFO, myRoom.ac ? "AC ON" : "AC OFF");
                } else {
                    logger(LOG_ERROR, "AUTO mode! No manual ctrl");
                }
                break;

            // ★補丁3：模擬溫度輸入，格式 t28
            case 't': {
                int temp = Serial.parseInt();             // 讀緊接在 't' 後面的數字
                if (temp == 0 && Serial.peek() != '0') { // parseInt 失敗時回傳 0
                    logger(LOG_ERROR, "Bad temp format");
                } else {
                    myRoom.temperature = temp;
                    Serial.print("[i] Temp set to ");
                    Serial.println(temp);
                    // 如果此時是 AUTO，立刻觸發溫控（跟 C 版行為一致）
                    if (myRoom.mode == AUTO_MODE) runAutoControl();
                }
                break;
            }

            default:
                // 忽略換行符號，其他字元才警告
                if (cmd != '\n' && cmd != '\r') {
                    logger(LOG_WARN, "Unknown cmd");
                }
                break;
        }
    }

    if (myRoom.mode == AUTO_MODE) {
        runAutoControl();
    }

    renderHardware();
    delay(500);
}
