#include <stdio.h>
#include <string>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_task_wdt.h"
#include "freertos/portmacro.h"
#include <soc/rtc.h>
#include "esp_event.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "wifi_manager.h"

#include <string.h>
#include <esp_wifi.h>
#include <esp_netif.h>
#include "esp_system.h"
#include "esp_log.h"

#include <inttypes.h>

#include "RestServer.h"
#include "IntexSWG.h"
#include "PanelBackend.h"
#include "utils.h"

static IPanelBackend* panelBackend = nullptr;

/* @brief tag used for ESP serial console messages */
static const char TAG[] = "main";

TaskHandle_t xHandle1;
TaskHandle_t xHandle2;
TaskHandle_t TaskA;

volatile bool displayON = true;
volatile bool machineON = false;
volatile uint8_t displayIntensity = 3;      // (range 0-7)
volatile uint8_t statusLedsIntensity = 3;   // (range 0-7)
volatile bool sendingKeyCode = false;
volatile bool keyCodeSetByAPI = false;
volatile uint8_t buttonStatus = 0x2E;
volatile uint8_t nextButtonStatus = 0x00;
volatile uint8_t prevButtonStatus = 0x00;
volatile uint8_t dataReceived[2];
volatile uint8_t statusDigit1;
volatile uint8_t statusDigit2;
volatile uint8_t statusDigit3;
volatile uint8_t displayingDigit1;
volatile uint8_t displayingDigit2;
volatile uint8_t powerStatus;
volatile bool displayBlinking;
volatile bool removeWifiConfig = false;
volatile bool otaUpdating = false;
volatile bool wifiReconnecting = false;
volatile bool delayedPowerOff = false;
volatile bool readingMaster = false;
volatile uint8_t selfCleanTime = 10;

volatile bool serviceLedBlinkRequested = false;

volatile uint16_t virtualPressButtonTime = 250;
volatile uint32_t checkpoint=0;
volatile bool waitForWifiConfig = true;
volatile uint8_t dataReceivedBuffer[128][2];
volatile int totalbytes = 0;

/* Set by the WiFi callback when a new IP is acquired; RTOS_1 then scrolls the
 * IP string across the 2-digit display once. ipScrollBuffer is filled before
 * the flag is set (simple producer/consumer handshake). */
volatile bool scrollIPRequested = false;
char ipScrollBuffer[16];

volatile bool picInjectionActive = false;
volatile uint8_t picInjectionCurrentPhase = 0;
volatile uint8_t picInjectionLastTmButton = 0x00;
volatile uint8_t picInjectionLastPicButton = 0xFF;
volatile uint32_t picInjectionFramesStarted = 0;
volatile uint32_t picInjectionFramesCompleted = 0;
volatile uint32_t picInjectionFramesRepeated = 0;
volatile bool picReleaseProbeRequested = false;

char getDisplayDigitFromCode(uint8_t code){
    switch (code) {
        case DISP_BLANK: return 0;
        case DISP_0: return '0';
        case DISP_1: return '1';
        case DISP_2: return '2';
        case DISP_3: return '3';
        case DISP_4: return '4';
        case DISP_5: return '5';
        case DISP_6: return '6';
        case DISP_7: return '7';
        case DISP_8: return '8';
        case DISP_9: return '9';
        case DISP_DP: return '.';
        case DISP_1_CLEAN_06P: return '6';
        case DISP_1_CLEAN_10P: return '0';
        case DISP_1_CLEAN_14P: return '4';
        default: return 0;
    }
}

uint8_t getCodeFromDisplayDigit(char displayDigit){
    switch (displayDigit) {
        case 0: return DISP_BLANK;
        case '0': return DISP_0;
        case '1': return DISP_1;
        case '2': return DISP_2;
        case '3': return DISP_3;
        case '4': return DISP_4;
        case '5': return DISP_5;
        case '6': return DISP_6;
        case '7': return DISP_7;
        case '8': return DISP_8;
        case '9': return DISP_9;
        case '.': return DISP_DP;
        default: return DISP_BLANK;
    }
}

void IRAM_ATTR machinePower(bool powerON){
    if (powerON) {
        machineON = true;
#if defined(CONFIG_INTSWG_POWER_RELAY)
        delayMicroseconds(2500);
        GPIO_Clear(powerRelayPin);
#endif
    }
    else {
#if defined(CONFIG_INTSWG_POWER_RELAY)
        GPIO_Set(powerRelayPin);
        delayMicroseconds(200);
#endif
        machineON = false;
        statusDigit1 = DISP_BLANK;
        statusDigit2 = DISP_BLANK;
        statusDigit3 = DISP_BLANK;
    }
}

void feedTheDog(){
    esp_task_wdt_reset();
}

void notifyApiRequest(void){
    serviceLedBlinkRequested = true;
}

/* ---- API command serialization (declared in IntexSWG.h) ---------------- */
typedef enum {
    API_CMD_POWER_ON,
    API_CMD_POWER_OFF,
    API_CMD_STANDBY,
    API_CMD_SELF_CLEAN,
} api_command_type_t;

typedef struct {
    api_command_type_t type;
    uint8_t param;            /* self-clean display code for API_CMD_SELF_CLEAN */
} api_command_t;

static QueueHandle_t apiCommandQueue = NULL;

void apiCommandQueueInit(void){
    if (apiCommandQueue == NULL) {
        apiCommandQueue = xQueueCreate(8, sizeof(api_command_t));
    }
}

static bool apiCommandEnqueue(const api_command_t *cmd){
    if (apiCommandQueue == NULL) {
        return false;
    }
    return xQueueSend(apiCommandQueue, cmd, 0) == pdTRUE;
}

bool apiCommandPower(bool on){
#if !defined(CONFIG_INTSWG_POWER_RELAY)
    // Without a power relay the unit cannot be switched fully off.
    if (!on) {
        return false;
    }
#endif
    api_command_t cmd = { on ? API_CMD_POWER_ON : API_CMD_POWER_OFF, 0 };
    return apiCommandEnqueue(&cmd);
}

bool apiCommandStandby(void){
    api_command_t cmd = { API_CMD_STANDBY, 0 };
    return apiCommandEnqueue(&cmd);
}

bool apiCommandSelfClean(uint8_t selfCleanCode){
    api_command_t cmd = { API_CMD_SELF_CLEAN, selfCleanCode };
    return apiCommandEnqueue(&cmd);
}

inline uint32_t IRAM_ATTR clocks(){
    uint32_t ccount;
    asm volatile ( "rsr %0, ccount" : "=a" (ccount) );
    return ccount;
}

inline void delayClocks(uint32_t clks){
    uint32_t c = clocks();
    while( (clocks() - c ) < clks ){
        asm(" nop");
    }
}

/**
 * @brief Task in charge of ESP32<->SWG serial BUS
 */
#if !defined(CONFIG_INTSWG_PANEL_BACKEND_PIC16F88)
static void IRAM_ATTR Core1Tm1650( void* p){

    vTaskDelay(pdMS_TO_TICKS(1000));
    // Core1 owns CPU1 with interrupts disabled for the whole bit-banging loop,
    // so it must NOT be registered with the task watchdog (and the loop must
    // not call the heavyweight esp_task_wdt_reset(), which would slow the tight
    // timing down and break the keycode response). CPU1 watchdog checks are
    // disabled in sdkconfig to match the original v4.4.8 behavior.
    machinePower(true);

    portDISABLE_INTERRUPTS();
    uint8_t sdaValue = 1, sclValue = 0, prevSda = 1, prevScl = 0, totalClocks = 0, totalBitsSent = 0;
    bool receivingData = false, bytePosition = false, sendingACK = false;
    uint8_t receivedByte = 0; 
    uint32_t lastIntWindowClocks = clocks();

    sdaValue = prevSda = GPIO_IN_Get(dataPin);
    prevScl = GPIO_IN_Get(clockPin); 

    while(1) {

        if (totalbytes == 127) {
            totalbytes = 0;
        }
        
        if (otaUpdating || removeWifiConfig || !machineON || wifiReconnecting) {
            readingMaster = false;
            portENABLE_INTERRUPTS();

            sdaValue = 1, sclValue = 0, prevSda = 1, prevScl = 0, totalClocks = 0, totalBitsSent = 0;
            receivingData = false;
            bytePosition = true;
            sendingACK = false;
            sendingKeyCode = false;
            receivedByte = 0;

            while (otaUpdating || removeWifiConfig || !machineON || wifiReconnecting) {
                feedTheDog();
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
            portDISABLE_INTERRUPTS();

            sdaValue = prevSda = GPIO_IN_Get(dataPin);
            prevScl = GPIO_IN_Get(clockPin); 
        }
        readingMaster = true;

        // Get next SCL and SDA values
        if (!sendingKeyCode && !sendingACK) {
            sdaValue = GPIO_IN_Get(dataPin);
        }
        sclValue = GPIO_IN_Get(clockPin);

        if (panelBackend != nullptr) {
            panelBackend->onBusSample(sdaValue, sclValue, clocks());
        }


        // START Condition **********************************************************************************************************
        if (sclValue == HIGH && prevSda == HIGH && sdaValue == LOW) {
            receivingData = true;
            bytePosition = false;            
        }
        // START Condition **********************************************************************************************************


        // STOP Condition ***********************************************************************************************************
        else if (sclValue == HIGH && prevSda == LOW && sdaValue == HIGH) {            
            receivingData = false;
            sendingKeyCode = false;
            bytePosition = false;
            totalClocks = 0;
            receivedByte = 0;
        }
        // STOP Condition ***********************************************************************************************************            


        // Detect CLK RISING EDGE AFTER START CONDITION (0 -> 1) ********************************************************************
        else if (prevScl == LOW && sclValue == HIGH && receivingData) {
            if (!sendingKeyCode) {
                // RISING edge 1-8                    
                if (totalClocks < 8) {
                    // Read SDA bit
                    receivedByte <<= 1;   // MSB first on TM1650, so shift left                            
                    if (sdaValue == HIGH) {
                        receivedByte |= 1;	// MSB first on TM1650, so set lowest bit
                    }                                         
                }
                // RISING edge 9
                else {                      
                    // Save received byte
                    dataReceived[(int)bytePosition] = receivedByte;                                                                                                

                    // Position 0 (first byte)
                    if (!bytePosition) {
                        dataReceivedBuffer[totalbytes][0] = receivedByte;                        
                        // Read Keyboard code request received
                        if (dataReceived[0] == 0x4F) {
                            sendingKeyCode = true;
                            bytePosition = !bytePosition;
                            dataReceivedBuffer[totalbytes][1] = buttonStatus;
                            totalbytes = totalbytes + 1;
                        }
                    }
                    // Position 1 (second byte)
                    else {
                        dataReceivedBuffer[totalbytes][1] = receivedByte;
                        totalbytes = totalbytes + 1;
                        // Check received byte for corresponding digit
                        switch (dataReceived[0])
                        {
                            case DIGIT1:
                                statusDigit1 = dataReceived[1];
                                break;
                            case DIGIT2:
                                statusDigit2 = dataReceived[1];
                                break;
                            case DIGIT3:
                                statusDigit3 = dataReceived[1];
                                break;                            
                            default:
                                break;
                        }
                    }               
                    receivedByte = 0;                    
                    bytePosition = !bytePosition;
                }
                totalClocks++;
            }            
        }
        // Detect CLK RISING EDGE AFTER START CONDITION (0 -> 1) ********************************************************************


        // Detect CLK FALLING EDGE (1 -> 0) *****************************************************************************************
        else if (prevScl == HIGH && sclValue == LOW  && receivingData) {

            // FALLING edge 8
            if (totalClocks == 8 && !sendingKeyCode) {
                // BEGIN SEND ACK
                digitalWrite(dataPin, LOW);
                pinMode(dataPin, GPIO_MODE_OUTPUT);
                sendingACK = true;
            }

            // FALLING edge 9
            else if (sendingACK && totalClocks == 9) {
                // STOP SENDING ACK (Keep dataPin as output if we are about to send key code to master)
                if (sendingKeyCode == false) {
                    // Release SDA, Stop Send ACK 
                    pinMode(dataPin, GPIO_MODE_INPUT);
                    digitalWrite(dataPin, HIGH);
                }
                sendingACK = false;
                totalClocks = 0;
            }

            // SEND KEY CODE (Master requested to read keys with command 0x4F)
            // First bit to send in FALLING edge 9 of first received BYTE
            if (sendingKeyCode) {                  
                if (totalBitsSent < 8) {                    
                    digitalWrite(dataPin, (buttonStatus << totalBitsSent) & 0x80 ? HIGH : LOW);
                    totalBitsSent++;
                }
                // FALLING edge 8 while sending keycode
                else if (totalBitsSent == 8) {
                    // Receive ACK
                    pinMode(dataPin, GPIO_MODE_INPUT);                        
                    digitalWrite(dataPin, HIGH);
                    totalBitsSent++;                    
                }
                // FALLING edge 9 while sending keycode
                else {
                    sendingKeyCode = false;
                    totalBitsSent = 0;
                }
            }
        }
        // Detect CLK FALLING EDGE (1 -> 0) *****************************************************************************************


        // Save previous SDA and SCL values
        if (!sendingKeyCode) {
            prevSda = sdaValue;            
        }        
        prevScl = sclValue;        

        // Cooperative interrupt window (ESP-IDF v5/v6 only).
        // Flash/NVS operations on CPU0 (e.g. wifi_manager_save_sta_config)
        // stall this core via esp_ipc_call_nonblocking() and busy-wait until
        // CPU1 acknowledges. While interrupts are permanently disabled here,
        // CPU1 can never acknowledge -> CPU0 deadlocks and the task watchdog
        // fires (IDLE0). In ESP-IDF 4.x this worked because the flash stall
        // used a high-priority ISR that bypassed portDISABLE_INTERRUPTS().
        // When the bus is completely idle (no transfer in progress) it is safe
        // to briefly enable interrupts so a pending flash operation can run.
        //
        // This window is THROTTLED to roughly once every 50 ms: every time it
        // opens, any pending interrupt (in particular the 1 kHz FreeRTOS tick
        // and WiFi/network ISRs) preempts Core1 and can corrupt an in-flight
        // bus transaction -> swallowed / half-applied API commands. Opening it
        // only ~20x/s keeps that disruption negligible while still servicing
        // the rare flash stall far below the 5 s task watchdog limit.
        if (!receivingData && !sendingKeyCode && !sendingACK) {
            uint32_t nowClocks = clocks();
            if ((nowClocks - lastIntWindowClocks) > CLOCKS_50_ms) {
                lastIntWindowClocks = nowClocks;
                portENABLE_INTERRUPTS();
                portDISABLE_INTERRUPTS();
            }
        }
    }
}
#endif

#if defined(CONFIG_INTSWG_PANEL_BACKEND_PIC16F88)
namespace {

constexpr uint32_t kPicInjStartLowUs = PIC_INJ_START_LOW_US;
constexpr uint32_t kPicInjZeroHighUs = PIC_INJ_ZERO_HIGH_US;
constexpr uint32_t kPicInjOneHighUs = PIC_INJ_ONE_HIGH_US;
constexpr uint32_t kPicInjBitLowUs = PIC_INJ_BIT_LOW_US;
constexpr uint32_t kPicInjEndLowUs = PIC_INJ_END_LOW_US;
constexpr uint32_t kPicInjRepeatGapUs = PIC_INJ_REPEAT_GAP_US;
// Debounce gap between the press frame and the release frame of a single tap
// (matches the 50 ms DEBOUNCE in the reference protocol implementation).
constexpr uint32_t kPicInjTapGapUs = 50000;
constexpr uint8_t kPicBtnSelfCleanByte = 0xF7;
constexpr uint8_t kPicBtnReleaseByte = 0xFF;

enum : uint8_t {
    PIC_INJ_PHASE_START_LOW = 0,
    PIC_INJ_PHASE_BIT_HIGH = 1,
    PIC_INJ_PHASE_BIT_LOW = 2,
    PIC_INJ_PHASE_END_LOW = 3,
    PIC_INJ_PHASE_REPEAT_GAP = 4,
    PIC_INJ_PHASE_TAP_GAP = 5,     // debounce gap before the release frame
};

struct PicInjectorState {
    bool active = false;
    uint8_t buttonByte = 0xFF;
    uint16_t frame = 0xFF00;
    int8_t bitPos = 15;
    uint8_t phase = PIC_INJ_PHASE_START_LOW;
    uint32_t phaseStart = 0;
    // Tap mode: a button press is emulated as one press frame + debounce gap +
    // one release frame, then idle (matches the reference demo). Used for all
    // momentary buttons (power/standby). Self-clean instead holds via repeats.
    bool tapMode = false;
    bool releasePhase = false;
    bool tapDone = false;
};

inline uint8_t mapTmButtonToPicButton(uint8_t tmBtn) {
    switch (tmBtn) {
        case BUTTON_POWER: return 0xFB;      // C / Power
        case BUTTON_TIMER: return 0xFD;      // E / Timer
        case BUTTON_LOCK: return 0xFE;       // W / Lock
        case BUTTON_BOOST: return 0xEF;      // N / Boost
        case BUTTON_SELF_CLEAN: return 0xF7; // S / Self-clean
        default: return 0xFF;                // release
    }
}

inline uint32_t picUsToClocks(uint32_t us) {
    return us * 240;
}

inline void picSetPhase(PicInjectorState &st, uint8_t phase) {
    st.phase = phase;
    picInjectionCurrentPhase = phase;
}

void IRAM_ATTR picStartFrame(PicInjectorState &st, uint8_t picButton, uint32_t nowClocks) {
    st.active = true;
    picInjectionActive = true;
    st.buttonByte = picButton;
    picInjectionLastPicButton = picButton;
    st.frame = ((uint16_t)picButton << 8) | (uint8_t)(~picButton);
    st.bitPos = 15;
    picSetPhase(st, PIC_INJ_PHASE_START_LOW);
    st.phaseStart = nowClocks;
    picInjectionFramesStarted = picInjectionFramesStarted + 1;
    digitalWrite(clockPin, LOW);
}

// Returns true if injector currently owns clockPin, false if pass-through should be used.
bool IRAM_ATTR picInjectionStep(PicInjectorState &st, uint8_t passthroughLevel, uint32_t nowClocks) {
    bool wantInject = keyCodeSetByAPI && buttonStatus != 0x00;
    picInjectionLastTmButton = buttonStatus;
    uint8_t targetButton = mapTmButtonToPicButton(buttonStatus);

    if (!wantInject || targetButton == 0xFF) {
        st.active = false;
        st.tapMode = false;
        st.releasePhase = false;
        st.tapDone = false;
        picInjectionActive = false;
        picInjectionCurrentPhase = PIC_INJ_PHASE_START_LOW;
        digitalWrite(clockPin, passthroughLevel);
        return false;
    }

    // A momentary tap (press + release) has already been delivered; stay idle
    // (passthrough) for the rest of the key-hold window so the press is not
    // repeated and re-toggled.
    if (st.tapDone) {
        digitalWrite(clockPin, passthroughLevel);
        return false;
    }

    bool isHold = (targetButton == kPicBtnSelfCleanByte);

    // Start a fresh press when idle or when the requested button changed
    // (but not while we are emitting the release frame of a tap).
    if (!st.active || (!st.releasePhase && st.buttonByte != targetButton)) {
        st.tapMode = !isHold;
        st.releasePhase = false;
        picStartFrame(st, targetButton, nowClocks);
        return true;
    }

    uint32_t elapsed = nowClocks - st.phaseStart;

    switch (st.phase) {
        case PIC_INJ_PHASE_START_LOW:
            if (elapsed >= picUsToClocks(kPicInjStartLowUs)) {
                picSetPhase(st, PIC_INJ_PHASE_BIT_HIGH);
                st.phaseStart = nowClocks;
                digitalWrite(clockPin, HIGH);
            }
            break;

        case PIC_INJ_PHASE_BIT_HIGH: {
            uint32_t highDur = ((st.frame >> st.bitPos) & 0x01)
                ? picUsToClocks(kPicInjOneHighUs)
                : picUsToClocks(kPicInjZeroHighUs);
            if (elapsed >= highDur) {
                picSetPhase(st, PIC_INJ_PHASE_BIT_LOW);
                st.phaseStart = nowClocks;
                digitalWrite(clockPin, LOW);
            }
            break;
        }

        case PIC_INJ_PHASE_BIT_LOW:
            if (elapsed >= picUsToClocks(kPicInjBitLowUs)) {
                st.bitPos--;
                st.phaseStart = nowClocks;
                if (st.bitPos >= 0) {
                    picSetPhase(st, PIC_INJ_PHASE_BIT_HIGH);
                    digitalWrite(clockPin, HIGH);
                } else {
                    picSetPhase(st, PIC_INJ_PHASE_END_LOW);
                    digitalWrite(clockPin, LOW);
                }
            }
            break;

        case PIC_INJ_PHASE_END_LOW:
            if (elapsed >= picUsToClocks(kPicInjEndLowUs)) {
                picInjectionFramesCompleted = picInjectionFramesCompleted + 1;
                digitalWrite(clockPin, HIGH);
                st.phaseStart = nowClocks;
                if (st.tapMode && st.releasePhase) {
                    // Release frame finished → single tap complete. Stay idle
                    // (passthrough) until the key-hold window ends.
                    st.tapDone = true;
                    st.active = false;
                    picInjectionActive = false;
                    picInjectionCurrentPhase = PIC_INJ_PHASE_START_LOW;
                    digitalWrite(clockPin, passthroughLevel);
                    return false;
                }
                // Press frame finished: tap → debounce gap before release;
                // hold (self-clean) → repeat gap before resending.
                picSetPhase(st, st.tapMode ? PIC_INJ_PHASE_TAP_GAP
                                           : PIC_INJ_PHASE_REPEAT_GAP);
            }
            break;

        case PIC_INJ_PHASE_TAP_GAP:
            // Debounce, then emit one release frame to complete the tap.
            if (elapsed >= picUsToClocks(kPicInjTapGapUs)) {
                st.releasePhase = true;
                picStartFrame(st, kPicBtnReleaseByte, nowClocks);
            }
            break;

        case PIC_INJ_PHASE_REPEAT_GAP:
            // Repeat while virtual key press is held by RTOS_2 timing logic.
            if (elapsed >= picUsToClocks(kPicInjRepeatGapUs)) {
                picInjectionFramesRepeated = picInjectionFramesRepeated + 1;
                picStartFrame(st, st.buttonByte, nowClocks);
            }
            break;

        default:
            st.active = false;
            picInjectionActive = false;
            picInjectionCurrentPhase = PIC_INJ_PHASE_START_LOW;
            digitalWrite(clockPin, passthroughLevel);
            return false;
    }

    return true;
}

}  // namespace

// Send a single button-release frame [0xFF][0x00] synchronously on TO_MAIN.
// Blocks for ~11 ms. Used to prompt the main board to refresh the display.
static void IRAM_ATTR sendPicReleaseSync(void) {
    constexpr uint16_t kReleaseFrame = 0xFF00U;
    digitalWrite(clockPin, LOW);
    delayClocks(picUsToClocks(kPicInjStartLowUs));
    for (int8_t i = 15; i >= 0; i--) {
        uint32_t highDur = ((kReleaseFrame >> i) & 1u)
            ? picUsToClocks(kPicInjOneHighUs)
            : picUsToClocks(kPicInjZeroHighUs);
        digitalWrite(clockPin, HIGH);
        delayClocks(highDur);
        digitalWrite(clockPin, LOW);
        delayClocks(picUsToClocks(kPicInjBitLowUs));
    }
    digitalWrite(clockPin, HIGH);
}

static void IRAM_ATTR Core1Pic16f88(void* p) {
    vTaskDelay(pdMS_TO_TICKS(1000));
    machinePower(true);

    portDISABLE_INTERRUPTS();
    PicInjectorState injector;
    uint32_t lastIntWindowClocks = clocks();

    while (1) {
        if (otaUpdating || removeWifiConfig || !machineON || wifiReconnecting) {
            readingMaster = false;
            portENABLE_INTERRUPTS();
            while (otaUpdating || removeWifiConfig || !machineON || wifiReconnecting) {
                feedTheDog();
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
            portDISABLE_INTERRUPTS();
            injector.active = false;
            picInjectionActive = false;
            picInjectionCurrentPhase = PIC_INJ_PHASE_START_LOW;
            digitalWrite(clockPin, HIGH);
        }

        readingMaster = true;

        uint8_t mainDataLevel = GPIO_IN_Get(dataPin);       // FROM_MAIN
        uint8_t displayClockLevel = GPIO_IN_Get(clockDispPin); // FROM_DISP
        uint32_t now = clocks();

        uint8_t displayOutputLevel = panelBackend != nullptr
            ? panelBackend->getDisplayOutputLevel(mainDataLevel, now)
            : mainDataLevel;
        digitalWrite(dataDispPin, displayOutputLevel);

        bool injecting = picInjectionStep(injector, displayClockLevel, now);
        uint8_t outClockLevel = injecting ? GPIO_IN_Get(clockPin) : displayClockLevel;

        if (panelBackend != nullptr) {
            panelBackend->onBusSample(mainDataLevel, outClockLevel, now);
        }

        // Send release probe synchronously when requested and not mid-injection.
        if (picReleaseProbeRequested && !injecting) {
            picReleaseProbeRequested = false;
            sendPicReleaseSync();
        }

        // Same cooperative interrupt window strategy used by the TM1650 path.
        // Never open it mid-frame: an ISR preemption would freeze the output
        // line and stretch the current bit, corrupting the frame the display
        // panel decodes (the cause of boot-animation flicker).
        bool displayBusy = panelBackend != nullptr && panelBackend->isDisplayBusy();
        if (!injecting && !displayBusy) {
            uint32_t nowClocks = clocks();
            if ((nowClocks - lastIntWindowClocks) > CLOCKS_50_ms) {
                lastIntWindowClocks = nowClocks;
                portENABLE_INTERRUPTS();
                portDISABLE_INTERRUPTS();
            }
        }
    }
}
#endif

void IRAM_ATTR Core1(void* p) {
#if defined(CONFIG_INTSWG_PANEL_BACKEND_PIC16F88)
    Core1Pic16f88(p);
#else
    Core1Tm1650(p);
#endif
}

/**
 * @brief Task that shows a countdown en restarts ESP32 when wifi configuration has been saved successfully
 */
void reset_esp(void *pvParameter){
    waitForWifiConfig = false;
    statusDigit2 = DISP_0;
    for (int i = 3; i >= 0; i--) {
        printf("Restarting in %d seconds...\n", i);
        switch (i)
        {
        case 3:
            statusDigit1 = DISP_3;
            break;
        case 2:
            statusDigit1 = DISP_2;
            break;
        case 1:
            statusDigit1 = DISP_1;
            break;
        case 0:
            statusDigit1 = DISP_0;
            break;        
        default:
            break;
        }        
        vTaskDelay(pdMS_TO_TICKS(1000));
        feedTheDog();
    }
    otaUpdating = false;
    removeWifiConfig = false;
    printf("Restarting now.\n");
    fflush(stdout);
    esp_restart();
}

void sendDataToDisplay(uint8_t digit, uint8_t value, uint8_t intensity){
    if (panelBackend == nullptr) {
        return;
    }
    buttonStatus = panelBackend->readButtonCode(keyCodeSetByAPI, buttonStatus);
    panelBackend->writeDisplay(digit, value, intensity, displayON);
}

// Ends an override animation. On PIC16F88 the main board is event-driven and
// may be silent (standby), so the panel latches the last frame we sent. Leaving
// it blank would hide the standby ".": instead show the last decoded main-board
// state (or the assumed-standby default) so something sensible remains until the
// main board transmits again. TM1650 drives its chip directly, so just clear it.
static void finishOverrideDisplay(){
#if defined(CONFIG_INTSWG_PANEL_BACKEND_PIC16F88)
    sendDataToDisplay(DIGIT1, statusDigit1, displayIntensity);
    sendDataToDisplay(DIGIT2, statusDigit2, displayIntensity);
    sendDataToDisplay(DIGIT3, statusDigit3, statusLedsIntensity);
    vTaskDelay(pdMS_TO_TICKS(60));  // let the serializer emit the final frame
#else
    sendDataToDisplay(DIGIT1, DISP_BLANK, displayIntensity);
    sendDataToDisplay(DIGIT2, DISP_BLANK, displayIntensity);
    sendDataToDisplay(DIGIT3, DISP_BLANK, statusLedsIntensity);
#endif
    if (panelBackend != nullptr) {
        panelBackend->setDisplayOverride(false);
    }
}

/**
 * @brief Startup animation shown once when the system boots:
 *        1. each status LED lights up in turn (round robin),
 *        2. the two-digit display counts up from 0 to 10.
 *        Called from RTOS_1 so it shares the display bus with normal mirroring.
 */
void startupAnimation(){
    if (panelBackend != nullptr) {
        panelBackend->setDisplayOverride(true);
    }

    // 1. Round-robin all status LEDs (DIGIT3 carries the 8 LED bits).
    for (int round = 0; round < 2; round++) {
        for (int led = 0; led < 8; led++) {
            sendDataToDisplay(DIGIT3, (uint8_t)(1 << led), statusLedsIntensity);
            vTaskDelay(pdMS_TO_TICKS(120));
        }
    }
    sendDataToDisplay(DIGIT3, DISP_BLANK, statusLedsIntensity);
    vTaskDelay(pdMS_TO_TICKS(200));

    // 2. Count from 0 to 10 on the two digits.
    // Note: DIGIT2 is the LEFT (tens) digit, DIGIT1 is the RIGHT (ones) digit.
    static const uint8_t digitCodes[10] = {
        DISP_0, DISP_1, DISP_2, DISP_3, DISP_4,
        DISP_5, DISP_6, DISP_7, DISP_8, DISP_9
    };
    for (int value = 0; value <= 10; value++) {
        uint8_t tens = (value >= 10) ? DISP_1 : DISP_BLANK;
        uint8_t ones = digitCodes[value % 10];
        sendDataToDisplay(DIGIT2, tens, displayIntensity);   // left digit
        sendDataToDisplay(DIGIT1, ones, displayIntensity);   // right digit
        vTaskDelay(pdMS_TO_TICKS(300));
    }
    vTaskDelay(pdMS_TO_TICKS(300));

    finishOverrideDisplay();
}

/**
 * @brief Scrolls an IP string (e.g. "192.168.40.230") across the 2-digit
 *        display as a marquee. Dots are merged onto the preceding digit's
 *        decimal point so they don't consume a separate position.
 *        Note: DIGIT2 is the LEFT digit, DIGIT1 is the RIGHT digit.
 *        Called from RTOS_1 so it shares the display bus with normal mirroring.
 */
void scrollIPOnDisplay(const char *ip){
    if (panelBackend != nullptr) {
        panelBackend->setDisplayOverride(true);
    }

    uint8_t codes[24];
    int n = 0;

    // Leading blanks so the text scrolls in from the right.
    codes[n++] = DISP_BLANK;
    codes[n++] = DISP_BLANK;

    for (int i = 0; ip[i] != '\0' && n < (int)sizeof(codes) - 3; i++) {
        if (ip[i] == '.') {
            // Merge the dot onto the previous digit (free decimal-point segment).
            if (n > 0 && codes[n - 1] != DISP_BLANK) {
                codes[n - 1] |= DISP_DP;
            } else {
                codes[n++] = DISP_DP;
            }
        } else {
            codes[n++] = getCodeFromDisplayDigit(ip[i]);
        }
    }

    // Trailing blanks so the text scrolls fully out to the left.
    codes[n++] = DISP_BLANK;
    codes[n++] = DISP_BLANK;

    // Slide a 2-digit window across the code array.
    // codes[pos] is the left character (DIGIT2), codes[pos+1] the right (DIGIT1).
    for (int pos = 0; pos <= n - 2; pos++) {
        sendDataToDisplay(DIGIT2, codes[pos], displayIntensity);       // left digit
        sendDataToDisplay(DIGIT1, codes[pos + 1], displayIntensity);   // right digit
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    finishOverrideDisplay();
}

/**
 * @brief Task in charge of ESP32<->Display serial BUS
 */
void RTOS_1(void *p){

    vTaskDelay(pdMS_TO_TICKS(1000));

    // Play the boot animation once before normal display mirroring starts.
    startupAnimation();

    // Service-LED blink state (one short flash per incoming API request).
    bool serviceBlinking = false;
    unsigned long serviceBlinkStart = 0;

    while(1) {
        // Scroll the IP across the display once when a new one was acquired.
        if (scrollIPRequested) {
            scrollIPRequested = false;
            scrollIPOnDisplay(ipScrollBuffer);
        }

        // Flash the SERVICE LED once per incoming API request.
        if (serviceLedBlinkRequested) {
            serviceLedBlinkRequested = false;
#if defined(CONFIG_INTSWG_PANEL_BACKEND_PIC16F88)
            // PIC: the display is passthrough-driven in normal mode, so briefly
            // override it to show the current state with the SERVICE LED set,
            // then release back to passthrough (finishOverrideDisplay restores
            // the latest decoded state).
            if (panelBackend != nullptr) {
                panelBackend->setDisplayOverride(true);
                sendDataToDisplay(DIGIT1, statusDigit1, displayIntensity);
                sendDataToDisplay(DIGIT2, statusDigit2, displayIntensity);
                sendDataToDisplay(DIGIT3, (uint8_t)(statusDigit3 | (1u << LED_SERVICE)), statusLedsIntensity);
                vTaskDelay(pdMS_TO_TICKS(150));
                finishOverrideDisplay();
            }
#else
            serviceBlinking = true;
            serviceBlinkStart = millis();
#endif
        }

#if defined(CONFIG_INTSWG_PANEL_BACKEND_PIC16F88)
        // PIC mirrors nothing in normal mode (passthrough drives the panel);
        // just idle so the watchdog is fed and IP-scroll/blink stay responsive.
        (void)serviceBlinking;
        (void)serviceBlinkStart;
        vTaskDelay(pdMS_TO_TICKS(20));
#else
        // Toggle the SERVICE LED bit for ~150 ms so a single request produces
        // one visible blink regardless of the LED's underlying state.
        uint8_t digit3 = statusDigit3;
        if (serviceBlinking) {
            if (millis() - serviceBlinkStart < 150) {
                digit3 ^= (uint8_t)(1 << LED_SERVICE);
            } else {
                serviceBlinking = false;
            }
        }

        sendDataToDisplay(DIGIT1, statusDigit1, displayIntensity);
        vTaskDelay(pdMS_TO_TICKS(10));
        sendDataToDisplay(DIGIT2, statusDigit2, displayIntensity);
        vTaskDelay(pdMS_TO_TICKS(10));
        sendDataToDisplay(DIGIT3, digit3, statusLedsIntensity);
        vTaskDelay(pdMS_TO_TICKS(10));
#endif

        taskYIELD();
    }
}

void select_self_clean_period(){
    unsigned long time1;
    unsigned long time2;
    time1 = time2 = millis();
    if (selfCleanTime != displayingDigit1) {
        buttonStatus = 0x00;
        ESP_LOGI(TAG, "selfCleanTime = 0x%02X / displayingDigit1 = 0x%02X", selfCleanTime, displayingDigit1);
        while(selfCleanTime != displayingDigit1) {
            displayingDigit1 = (statusDigit2 > 0x00) ? statusDigit1 : displayingDigit1;
            if (time2 - time1 > 250) {
                buttonStatus = (buttonStatus == 0x00) ? BUTTON_SELF_CLEAN : 0x00;                
                time1 = time2 = millis();
            } 
            else {
                time2 = millis();
            }
        }
    }
}

void press_lock_button(){
    unsigned long time1;
    unsigned long time2;
    time1 = time2 = millis();    
    buttonStatus = BUTTON_LOCK;
    while(time2 - time1 < 250) {
        time2 = millis();            
    }
    buttonStatus = 0x00;    
}

/**
 * @brief Task that controls virtual key press (API), power status info and display information to be retrieved by API
 */
void RTOS_2(void *p){

    vTaskDelay(pdMS_TO_TICKS(1000));
    
    unsigned long time1_keycode_api = 0;
    unsigned long time2_keycode_api = 0;
    unsigned long time1_displ = 0;
    unsigned long time2_displ = 0;
    unsigned long time1_delayed_keycode = 0;
    unsigned long time2_delayed_keycode = 0;
    unsigned long time1_delayed_off = 0;
    unsigned long time2_delayed_off = 0;    
    bool statusDisplayLed = false;
    bool prevStatusDisplayLed = false;
    uint8_t displayBlinks = 0;
    bool probeScheduled = false;
    time1_displ = time2_displ = millis();

    while(1) { 
        if (removeWifiConfig) {            
            wifi_manager_clear_wifi_configuration();

            reset_esp(NULL);
        }
        
        // After startup animation (~8 s), send one button-release probe if the
        // main board hasn't sent any display frames yet (event-driven protocol).
#if defined(CONFIG_INTSWG_PANEL_BACKEND_PIC16F88)
        if (!probeScheduled && machineON && millis() > 8000 && statusDigit2 == DISP_BLANK) {
            picReleaseProbeRequested = true;
            probeScheduled = true;
        }
#endif

        // Check and update Power status ***********************************************************
        powerStatus = (machineON) ? ((statusDigit2 == DISP_DP) ? POWER_STATUS_STANDBY : (statusDigit2 != DISP_BLANK) ? POWER_STATUS_ON : POWER_STATUS_BOOTING) : POWER_STATUS_OFF;
        // Check and update Power status ***********************************************************

        // Process one queued API command, but only while no other virtual key
        // press is in progress. This serializes commands coming from multiple
        // clients (e.g. several Home Assistant instances) so they can't clobber
        // each other; the power-state decision is evaluated here, at apply time,
        // against the current powerStatus. ***********************************
        if (apiCommandQueue != NULL && !keyCodeSetByAPI && nextButtonStatus == 0 && !delayedPowerOff) {
            api_command_t cmd;
            if (xQueueReceive(apiCommandQueue, &cmd, 0) == pdTRUE) {
                switch (cmd.type) {
                    case API_CMD_POWER_ON:
                        switch (powerStatus) {
                            case POWER_STATUS_OFF:
                                machinePower(true);
                                nextButtonStatus = BUTTON_POWER;
                                break;
                            case POWER_STATUS_STANDBY:
                            case POWER_STATUS_BUS_ERROR:
                            case POWER_STATUS_BOOTING:
                                keyCodeSetByAPI = true;
                                virtualPressButtonTime = PIC_SINGLE_PRESS_MS;
                                buttonStatus = BUTTON_POWER;
                                break;
                            default:
                                break;
                        }
                        break;
                    case API_CMD_POWER_OFF:
                        switch (powerStatus) {
                            case POWER_STATUS_BOOTING:
                            case POWER_STATUS_ON:
                                keyCodeSetByAPI = true;
                                virtualPressButtonTime = PIC_SINGLE_PRESS_MS;
                                buttonStatus = BUTTON_POWER;
                                delayedPowerOff = true;
                                break;
                            case POWER_STATUS_STANDBY:
                                machinePower(false);
                                break;
                            default:
                                break;
                        }
                        break;
                    case API_CMD_STANDBY:
                        switch (powerStatus) {
                            case POWER_STATUS_ON:
                            case POWER_STATUS_BUS_ERROR:
                            case POWER_STATUS_BOOTING:
                                keyCodeSetByAPI = true;
                                virtualPressButtonTime = PIC_SINGLE_PRESS_MS;
                                buttonStatus = BUTTON_POWER;
                                break;
                            case POWER_STATUS_OFF:
                                machinePower(true);
                                break;
                            default:
                                break;
                        }
                        break;
                    case API_CMD_SELF_CLEAN:
                        keyCodeSetByAPI = true;
                        virtualPressButtonTime = 6000;
                        buttonStatus = BUTTON_SELF_CLEAN;
                        selfCleanTime = cmd.param;
                        break;
                }
            }
        }
        // Process one queued API command ************************************

        // Check and update Display status ***********************************************************
        displayingDigit1 = (statusDigit1 == 0x00 && displayBlinking) ? displayingDigit1 : statusDigit1;
        displayingDigit2 = (statusDigit2 == 0x00 && displayBlinking) ? displayingDigit2 : statusDigit2;
        if (time2_displ - time1_displ < 600) {
            statusDisplayLed = (statusDigit2 > 0x00) ? true : false;
            if (prevStatusDisplayLed != statusDisplayLed) {
                displayBlinks++;
                prevStatusDisplayLed = statusDisplayLed;
            }
            time2_displ = millis();
        }
        else {
            displayBlinking = (displayBlinks > 0) ? true : false;
            displayBlinks = 0;
            time1_displ = time2_displ = millis();
        }
        // Check and update Display status ***********************************************************

        // Delayed Virtual Button press **************************************************************
        if (nextButtonStatus > 0) {
            // Start counting time
            if (time1_delayed_keycode == 0 && time2_delayed_keycode == 0) {
                time1_delayed_keycode = time2_delayed_keycode = millis();
            }
            else if (time2_delayed_keycode - time1_delayed_keycode > 2000) {
                buttonStatus = nextButtonStatus;
                nextButtonStatus = 0x00;
                keyCodeSetByAPI = true;
                time1_delayed_keycode = time2_delayed_keycode = 0;
            } 
            else {
                time2_delayed_keycode = millis();
            } 
        }
        // Delayed Virtual Button press **************************************************************        

        // Keep API keycode for ms (Virtual Press Button defined in virtualPressButtonTime) **********
        if (keyCodeSetByAPI) {
            // Start counting time
            if (time1_keycode_api == 0 && time2_keycode_api == 0) {
                time1_keycode_api = time2_keycode_api = millis();
            }
            if (time2_keycode_api - time1_keycode_api > virtualPressButtonTime) {
                // Self cleaning macro
                if (buttonStatus == BUTTON_SELF_CLEAN) {
                    select_self_clean_period();
                    press_lock_button();
                }
                // TODO: Manage programming macro
                keyCodeSetByAPI = false;
                time1_keycode_api = time2_keycode_api = 0;
#if defined(CONFIG_INTSWG_PANEL_BACKEND_PIC16F88)
                picReleaseProbeRequested = true;
#endif
            } 
            else {
                time2_keycode_api = millis();
            }
        }
        // Keep API keycode for ms (Virtual Press Button defined in virtualPressButtonTime) **********

        // Turn off the machine after a delay of 2 sec ***********************************************
        if (delayedPowerOff) {
            // Start counting time
            if (time1_delayed_off == 0 && time2_delayed_off == 0) {
                time1_delayed_off = time2_delayed_off = millis();
            }
            else if (time2_delayed_off - time1_delayed_off > 2000) {
                delayedPowerOff = false;                
                time1_delayed_off = time2_delayed_off = 0;
                machinePower(false);
            } 
            else {
                time2_delayed_off = millis();
            }           
        }
        // Turn off the machine after a delay of 2 sec ***********************************************        
        
        // Short delay so the CPU0 idle task can run (feeds the task watchdog).
        // The millis()-based timing above (600ms / 2000ms windows) is unaffected
        // by a few ms of loop period. A bare taskYIELD() never yields to the
        // lowest-priority idle task and would starve IDLE0 -> task_wdt timeout.
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

void wifi_watchdog_task(void *pvParameter){
    char ip[IP4ADDR_STRLEN_MAX] = {0};

    while (1) {
		vTaskDelay(pdMS_TO_TICKS(30000));

        if (wifi_manager_lock_sta_ip_string(1000)) {
	        strcpy(ip, wifi_manager_get_sta_ip_string());
			wifi_manager_unlock_sta_ip_string();

			if (strcmp(ip, "0.0.0.0") == 0) {
				ESP_LOGW(TAG, "WiFi appears disconnected (IP = 0.0.0.0)");
			}
		}		
    }
}

/**
 * @brief Show blinking "AP" in the display while waiting for wifi configuration
 */
void ConfigureWifi(void *p){
    vTaskDelay(pdMS_TO_TICKS(1000));

    int i = 0;
    while(waitForWifiConfig) {
        //statusDigit2 = (i % 2 == 0) ? 0x02 : 00;
        statusDigit1 = (i % 2 == 0) ? DISP_P : DISP_BLANK;
        statusDigit2 = (i % 2 == 0) ? DISP_A : DISP_BLANK;
        //statusDigit3 = 1 << i;
        i = (i < 7) ? i + 1 : 0;
        delayMicroseconds(300000);
        feedTheDog();
        taskYIELD();
    } 
    vTaskDelete( NULL );
}

void startCore0(void){
    xTaskCreatePinnedToCore(
        systemRebootTask,      // Function that implements the task.
        "rebootTask",           // Text name for the task.
        2048,                   // Stack size in bytes, not words.
        NULL,                   // Parameter passed into the task.
        tskIDLE_PRIORITY + 5,   // Priority
        NULL,                   // Variable to hold the task's data structure.
        0);                     // Core 0
}

void startCore0a(void){ // ESP32<->Display serial BUS
    xTaskCreatePinnedToCore(
        RTOS_1,                 // Function that implements the task.
        "RTOS-1",               // Text name for the task.
        STACK_SIZE,             // Stack size in bytes, not words.
        ( void * ) 1,           // Parameter passed into the task.
        tskIDLE_PRIORITY + 4,   // Priority
        &xHandle1,              // Variable to hold the task's data structure.
        0);                     // Core 0
}

void startCore0b(void){ // Task that controls virtual key press (API), power status info and display information
    xTaskCreatePinnedToCore(
        RTOS_2,                 // Function that implements the task.
        "RTOS-2",               // Text name for the task.
        STACK_SIZE,             // Stack size in bytes, not words.
        ( void * ) 1,           // Parameter passed into the task.
        tskIDLE_PRIORITY + 3,   // Priority
        &xHandle2,              // Variable to hold the task's data structure.
        0);                     // Core 0

    if (panelBackend != nullptr) {
        panelBackend->onDisplayTaskStart();
    }
}

// Function that creates the superloop Core1 to be pinned at Core 1
void startCore1(void){
    xTaskCreatePinnedToCore(
        Core1,                  // Function that implements the task.
        "Core1",                // Text name for the task.
        STACK_SIZE,             // Stack size in bytes, not words.
        ( void * ) 1,           // Parameter passed into the task.
        tskIDLE_PRIORITY + 5,   // Priority
        &TaskA,                 // Task
        1);                     // Core 1
}

// Function that creates the superloop Core1 to be pinned at Core 1
void configureWifiTask(void){
    xTaskCreate(
        ConfigureWifi,          // Function that implements the task.
        "ConfigureWifi",        // Text name for the task.
        STACK_SIZE,             // Stack size in bytes, not words.
        ( void * ) 1,           // Parameter passed into the task.
        tskIDLE_PRIORITY + 2,   // Priority
        &TaskA);                // Task
}

void configureWifiWatchdog(void){
    xTaskCreate(
        wifi_watchdog_task,    // Function that implements the task.
        "wifi_watchdog",        // Text name for the task.
        4096,                   // Stack size in bytes, not words.
        NULL,                   // Parameter passed into the task.
        5,                      // Priority
        NULL);                  // Task
}

/**
 * @brief this is an exemple of a callback that you can setup in your own app to get notified of wifi manager event.
 */
void cb_connection_ok(void *pvParameter){
    //wifiReconnecting = false;

	/* transform IP to human readable string */
	ip_event_got_ip_t* param = (ip_event_got_ip_t*)pvParameter;
	
    char str_ip[16];
	esp_ip4addr_ntoa(&param->ip_info.ip, str_ip, IP4ADDR_STRLEN_MAX);
    ESP_LOGI(TAG, "Connected. IP acquired: %s", str_ip);

    /* Hand the IP to RTOS_1 so it scrolls across the display once. */
    strncpy(ipScrollBuffer, str_ip, sizeof(ipScrollBuffer) - 1);
    ipScrollBuffer[sizeof(ipScrollBuffer) - 1] = '\0';
    scrollIPRequested = true;

    start_rest_server();
}

/**
 * @brief this is an exemple of a callback that you can setup in your own app to get notified of wifi manager event.
 */
void cb_connection_ko(void *pvParameter){    

    wifi_event_sta_disconnected_t *event = (wifi_event_sta_disconnected_t *)pvParameter;

    ESP_LOGW(TAG, "Connection lost. Reason: %d", event->reason);

    if (event->reason == WIFI_REASON_BEACON_TIMEOUT) {
        ESP_LOGW(TAG, "Beacon timeout detected. WiFi manager retry timer will reconnect.");
        esp_wifi_set_ps(WIFI_PS_NONE);
    }
}

extern "C" void app_main(void){

    panelBackend = getPanelBackend();
    ESP_LOGI(TAG, "Panel backend: %s", panelBackend->name());

    apiCommandQueueInit();

#if defined(CONFIG_INTSWG_PANEL_BACKEND_PIC16F88)
    // PIC16F88 board wiring (as in protocol demos):
    // FROM_MAIN=dataPin(input), TO_DISP=dataDispPin(output),
    // FROM_DISP=clockDispPin(input), TO_MAIN=clockPin(output).
    pinMode(dataPin, GPIO_MODE_INPUT);
    GPIO_Set(dataPin);

    pinMode(clockDispPin, GPIO_MODE_INPUT);
    GPIO_Set(clockDispPin);

    pinMode(clockPin, GPIO_MODE_OUTPUT);
    GPIO_Set(clockPin);

    pinMode(dataDispPin, GPIO_MODE_OUTPUT);
    GPIO_Set(dataDispPin);
#else
    pinMode(dataPin, GPIO_MODE_INPUT);
    GPIO_Set(dataPin);

    pinMode(clockPin, GPIO_MODE_INPUT);
    GPIO_Set(clockPin);

    pinMode(dataDispPin, GPIO_MODE_OUTPUT);
    pinMode(clockDispPin, GPIO_MODE_OUTPUT);
#endif

    pinMode(powerRelayPin, GPIO_MODE_OUTPUT);
#if defined(CONFIG_INTSWG_POWER_RELAY)
    GPIO_Set(powerRelayPin);   // open relay; Core1 closes it after 1 s
#else
    GPIO_Clear(powerRelayPin); // relay pin unused — drive low (inactive)
    machineON = true;          // SWG is always powered; treat as on from boot
#endif

#if defined(CONFIG_INTSWG_PANEL_BACKEND_PIC16F88)
    // Event-driven main board: until it transmits a frame we cannot know the
    // real state. Assume standby (".") so the display and API report something
    // sensible; the first decoded frame corrects it (e.g. ON when transmitting).
    statusDigit2 = DISP_DP;
#endif

    /* Print chip information */
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    printf("This is %s chip with %d CPU cores, WiFi%s%s, ",
            CONFIG_IDF_TARGET,
            chip_info.cores,
            (chip_info.features & CHIP_FEATURE_BT) ? "/BT" : "",
            (chip_info.features & CHIP_FEATURE_BLE) ? "/BLE" : "");

    printf("silicon revision %d, ", chip_info.revision);

    uint32_t flash_size = 0;
    esp_err_t flash_size_result = esp_flash_get_size(esp_flash_default_chip, &flash_size);
    if (flash_size_result == ESP_OK) {
        printf("%" PRIu32 "MB %s flash\n", flash_size / (1024 * 1024),
                (chip_info.features & CHIP_FEATURE_EMB_FLASH) ? "embedded" : "external");
    } else {
        printf("unknown %s flash size: %s\n",
                (chip_info.features & CHIP_FEATURE_EMB_FLASH) ? "embedded" : "external",
                esp_err_to_name(flash_size_result));
    }

    printf("Free heap: %" PRIu32 "\n", esp_get_free_heap_size());

    /* start the wifi manager */
	wifi_manager_start();

    esp_wifi_set_ps(WIFI_PS_NONE);
    ESP_LOGI(TAG, "WiFi Power Save disabled (WIFI_PS_NONE)");

    wifi_config_t sta_config;
    esp_wifi_get_config(WIFI_IF_STA, &sta_config);
    sta_config.sta.listen_interval = 10;
    esp_wifi_set_config(WIFI_IF_STA, &sta_config);

    //wifi_manager_clear_wifi_configuration();
    
    if (wifi_manager_fetch_wifi_sta_config())
    {
        //wifi_manager_set_callback(WM_EVENT_WIFI_CONFIG_CLEARED, &reset_esp);

        wifi_manager_set_callback(WM_EVENT_STA_DISCONNECTED, &cb_connection_ko);
        wifi_manager_set_callback(WM_EVENT_STA_GOT_IP, &cb_connection_ok);

        startCore1(); // ESP32<->SWG serial BUS
        startCore0(); // OTA
    }
    else {
        //wifi_manager_set_callback(WM_EVENT_WIFI_CONFIG_SAVED, &reset_esp);
        configureWifiTask();
    }

    startCore0a(); // ESP32<->Display serial BUS  
    startCore0b(); // Task that controls virtual key press (API), power status info and display information

    configureWifiWatchdog();
}
