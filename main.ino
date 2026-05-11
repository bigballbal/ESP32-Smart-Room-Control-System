#include <Wire.h>
#include <LiquidCrystal_I2C.h>

#define AC_ON_TEMP 30
#define AC_OFF_TEMP 25

// --- 1. Type Definitions ---
typedef enum { MANUAL_MODE, AUTO_MODE } Mode;
typedef enum { LOG_INFO, LOG_WARN, LOG_ERROR, LOG_SYSTEM } Loglevel;
typedef enum {
    CMD_INVALID = 0, 
    CMD_MANUAL_CONTROL = 1,
    CMD_INPUT_TEMPERATURE,
    CMD_SWITCH_AUTO,
    CMD_EXIT,
    CMD_SWITCH_MANUAL
} UserCommand;

typedef struct {
    Mode mode;
    int light; 
    int temperature; 
    int ac; 
} RoomSetting;

void logger(Loglevel level, const char* message);


// --- 2. 硬體定義 ---
LiquidCrystal_I2C lcd(0x27, 16, 2);  
const int lightPin = 12; // 模擬燈光的 LED
const int acPin = 13;    // 模擬 AC 的 LED 腳位


RoomSetting myRoom = {MANUAL_MODE, 0, 25, 0};


// --- 4. Control Layer (大腦 & 工具) ---
void logger(Loglevel level, const char* message) { //作用為傳訊息給電腦跟LCD顯示
    Serial.print(level == LOG_ERROR ? "[!] " : "[i] "); //LCD很小 節省空間 !代表有事 i代表沒事
    Serial.println(message);
    

    lcd.setCursor(0, 1);
    lcd.print("                "); // 清除該行 LCD不會自己刷掉上個訊息
    lcd.setCursor(0, 1);//為了放在最左邊第二行
    lcd.print(message);
}

void runAutoControl() {
    if (myRoom.temperature > AC_ON_TEMP) myRoom.ac = 1;
    else if (myRoom.temperature < AC_OFF_TEMP) myRoom.ac = 0;
}

// --- 5. Output Layer ---
void renderHardware() {
    digitalWrite(lightPin, myRoom.light ? HIGH : LOW);//
    digitalWrite(acPin, myRoom.ac ? HIGH : LOW);// 控制電壓 如果myRoom.ac or myRoom.light 是1 就給high電壓 ex. 5V 就會亮
    lcd.setCursor(0, 0); //從最總上開始output
    lcd.print("T:"); lcd.print(myRoom.temperature); //後面用處在於把溫度印出來
    lcd.print(" L:"); lcd.print(myRoom.light ? "ON" : "OFF");
    lcd.print(" M:"); lcd.print(myRoom.mode == AUTO_MODE ? "A" : "M");
}
    

// --- 6. Arduino 核心結構 ---
void setup() {//取代初始化
    Serial.begin(9600); //規定協定
    lcd.init(); //跟LCD說要開始幹的
    lcd.backlight();//開燈?
    
    pinMode(lightPin, OUTPUT);
    pinMode(acPin, OUTPUT);
    
    logger(LOG_SYSTEM, "System Ready");
}


void loop() { //取代while 迴圈
    // 這裡通常會放：讀取感測器數值、檢查 Serial 指令
    if (Serial.available() > 0) {//電腦有沒有透過USB傳東西?
        char cmd = Serial.read();//arduino讀盤
        // 這裡可以根據輸入字元切換模式，例如按 'a' 變 AUTO
        if (cmd == 'a') { myRoom.mode = AUTO_MODE; logger(LOG_SYSTEM, "Mode: AUTO"); }
        if (cmd == 'm') { myRoom.mode = MANUAL_MODE; logger(LOG_SYSTEM, "Mode: MANUAL"); }
    }

    if (myRoom.mode == AUTO_MODE) {
        runAutoControl();
    }
    
    renderHardware();
    delay(500); // 避免跑太快 LCD 閃爍
}


