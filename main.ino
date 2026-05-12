#include <Wire.h>
#include <LiquidCrystal_I2C.h>

#define AC_ON_TEMP 30
#define AC_OFF_TEMP 25

typedef enum { MANUAL_MODE, AUTO_MODE } Mode;
typedef enum { LOG_INFO, LOG_WARN, LOG_ERROR, LOG_SYSTEM } Loglevel;
typedef struct { Mode mode; int light; int temperature; int ac; } RoomSetting;

LiquidCrystal_I2C lcd(0x27, 16, 2);
const int lightPin = 12, acPin = 13;
RoomSetting myRoom = {MANUAL_MODE, 0, 25, 0};

void logger(Loglevel level, const char* msg) {
    switch(level) {
        case LOG_INFO:   Serial.print("[INFO] ");   break;
        case LOG_WARN:   Serial.print("[WARN] ");   break;
        case LOG_ERROR:  Serial.print("[ERROR] ");  break;
        case LOG_SYSTEM: Serial.print("[SYS] ");    break;
    }
    Serial.println(msg);
    lcd.setCursor(0, 1); lcd.print("                ");
    lcd.setCursor(0, 1); lcd.print(msg);
}

void runAutoControl() {
    if      (myRoom.temperature > AC_ON_TEMP)  myRoom.ac = 1;
    else if (myRoom.temperature < AC_OFF_TEMP) myRoom.ac = 0;
}

void renderHardware() {
    digitalWrite(lightPin, myRoom.light ? HIGH : LOW);
    digitalWrite(acPin,    myRoom.ac    ? HIGH : LOW);
    lcd.setCursor(0, 0); lcd.print("                ");
    lcd.setCursor(0, 0);
    lcd.print("T:"); lcd.print(myRoom.temperature);
    lcd.print(" L:"); lcd.print(myRoom.light ? "ON" : "OF");
    lcd.print(" AC:"); lcd.print(myRoom.ac   ? "ON" : "OF");
    lcd.setCursor(14, 0); lcd.print(myRoom.mode == AUTO_MODE ? " A" : " M");
}

void setup() {
    Serial.begin(9600);
    lcd.init(); lcd.backlight();
    pinMode(lightPin, OUTPUT); pinMode(acPin, OUTPUT);
    logger(LOG_SYSTEM, "System Ready");
    Serial.println("a=AUTO m=MANUAL l=Light c=AC t<num>=Temp");
}

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
            case 'l':
                if (myRoom.mode != MANUAL_MODE) { logger(LOG_ERROR, "AUTO! No ctrl"); break; }
                myRoom.light = !myRoom.light;
                logger(LOG_INFO, myRoom.light ? "Light ON" : "Light OFF");
                break;
            case 'c':
                if (myRoom.mode != MANUAL_MODE) { logger(LOG_ERROR, "AUTO! No ctrl"); break; }
                myRoom.ac = !myRoom.ac;
                logger(LOG_INFO, myRoom.ac ? "AC ON" : "AC OFF");
                break;
            case 't': {
                int temp = Serial.parseInt();
                myRoom.temperature = temp;
                Serial.print("[INFO] Temp: "); Serial.println(temp);
                if (myRoom.mode == AUTO_MODE) runAutoControl();
                break;
            }
            default:
                if (cmd != '\n' && cmd != '\r') logger(LOG_WARN, "Unknown cmd");
                break;
        }
    }
    if (myRoom.mode == AUTO_MODE) runAutoControl();
    renderHardware();
    delay(500);
}
