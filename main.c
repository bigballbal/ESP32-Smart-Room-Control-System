
#include <stdio.h>

#define AC_ON_TEMP 30
#define AC_OFF_TEMP 25

// --- 1. Type Definitions ---
typedef enum { MANUAL_MODE, AUTO_MODE } Mode;
typedef enum { LOG_INFO, LOG_WARN, LOG_ERROR, LOG_SYSTEM } Loglevel;
typedef enum {
    CMD_INVALID = 0, // 增加一個無效指令
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

// --- 2. Prototype (前置聲明) ---
// 這樣下面的 Input Layer 才能認識 logger
void logger(Loglevel level, const char* message);

// --- 3. Input Layer ---
UserCommand readUserCommand() {
    int choice;
    printf("\nEnter Choice(1-5): ");
    scanf("%d", &choice);
    if(choice < 1 || choice > 5) {
        // Logger 會由 Control Layer 統一管理，這裡只負責回傳
        return CMD_INVALID; 
    }
    return (UserCommand)choice;
}

void readManualDeviceControl(RoomSetting* sys) {
    printf("--- Manual Control ---\n");
    printf("Control Light (1 ON / 0 OFF): ");
    scanf("%d", &sys->light);
    printf("Control AC (1 ON / 0 OFF): ");
    scanf("%d", &sys->ac);
}

int readTemperature() {
    int temp;
    printf("Enter Temperature: ");
    scanf("%d", &temp);
    return temp;
}

void showMenu() {
    printf("\n===== Smart Room System =====");
    printf("\n1. Manual Control (Light/AC)");
    printf("\n2. Input Temperature");
    printf("\n3. Switch to Auto Mode");
    printf("\n4. Exit");
    printf("\n5. Switch to Manual Mode");
    printf("\n=============================\n");
}

// --- 4. Control Layer (大腦 & 工具) ---
void logger(Loglevel level, const char* message) {
    switch(level) {
        case LOG_INFO:   printf("[INFO] %s\n", message); break;
        case LOG_WARN:   printf("[WARNING] %s\n", message); break;
        case LOG_ERROR:  printf("[ERROR] %s\n", message); break;
        case LOG_SYSTEM: printf(">>> SYSTEM: %s <<<\n", message); break;
    }
}

void runAutoControl(RoomSetting* sys) {
    if (sys->temperature > AC_ON_TEMP) sys->ac = 1;
    else if (sys->temperature < AC_OFF_TEMP) sys->ac = 0;
}

// 將所有邏輯判斷（Logic）全部收納在這裡
void handleSystemLogic(RoomSetting *sys, UserCommand choice) {
    switch(choice) {
        case CMD_INVALID:
            logger(LOG_WARN, "Invalid input detected! Please enter 1-5.");
            break;

        case CMD_MANUAL_CONTROL:
            // 建議 3：手動控制的判斷逻辑也收進來
            if (sys->mode == AUTO_MODE) {
                logger(LOG_ERROR, "Manual control blocked! System is in AUTO mode.");
            } else {
                readManualDeviceControl(sys);
            }
            break;

        case CMD_SWITCH_AUTO:
            sys->mode = AUTO_MODE;
            logger(LOG_SYSTEM, "System Mode changed to AUTO");
            break;
        
        case CMD_SWITCH_MANUAL:
            sys->mode = MANUAL_MODE;
            logger(LOG_SYSTEM, "System Mode changed to MANUAL");
            break;

        case CMD_INPUT_TEMPERATURE:
            sys->temperature = readTemperature();
            // 溫度改變後，如果是在 AUTO 模式要立刻觸發溫控
            break;

        case CMD_EXIT:
            logger(LOG_SYSTEM, "User requested exit. Shutting down.");
            break;

        default:
            break;
    }

    // 只要是 AUTO 模式，每一輪都要跑自動檢查
    if (sys->mode == AUTO_MODE) {
        runAutoControl(sys);
    }
}

// --- 5. Output Layer ---
void renderDisplay(RoomSetting sys) {
    printf("\n----- System Status -----");
    printf("\nMode: %s", sys.mode == AUTO_MODE ? "AUTO" : "MANUAL");
    printf("\nLight: %s", sys.light ? "ON" : "OFF");
    printf("\nAC: %s", sys.ac ? "ON" : "OFF");
    printf("\nTemperature: %d", sys.temperature);
    printf("\n-------------------------\n");
}

// --- 6. Main (Dispatch Layer) ---
int main() {
    RoomSetting myRoom = {MANUAL_MODE, 0, 25, 0}; 
    UserCommand currentOpt;

    logger(LOG_SYSTEM, "Smart Room System Initialized.");

    while (1) {
        showMenu();
        currentOpt = readUserCommand();
        
        // 建議 1：這裡只負責轉發 (Dispatch)，不要寫 if (mode == ...)
        if (currentOpt == CMD_EXIT) {
            handleSystemLogic(&myRoom, currentOpt); // 讓大腦處理退出前的 Log
            break;
        }

        // 統一交給大腦處理所有事情
        handleSystemLogic(&myRoom, currentOpt);
        
        // 顯示結果
        renderDisplay(myRoom);
    }
    return 0;
}
