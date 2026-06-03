#include "PIC16F88Backend.h"

#include "IntexSWG.h"

namespace {

constexpr uint32_t kCyclesPerUs = 240;  // CPU fixed at 240 MHz
constexpr uint32_t kStartLowUs = 200;
constexpr uint32_t kZeroHighUs = 200;
constexpr uint32_t kOneHighUs = 800;
constexpr uint32_t kBitLowUs = 200;
constexpr uint32_t kEndLowUs = 200;
constexpr uint32_t kZeroMinUs = 120;
constexpr uint32_t kZeroMaxUs = 360;
constexpr uint32_t kOneMinUs = 560;
constexpr uint32_t kOneMaxUs = 1100;
constexpr uint32_t kFrameGapUs = 1500;

// Gap (signal HIGH) inserted between repeated frames. Must exceed the panel
// decoder's frame-boundary threshold (kFrameGapUs = 1500 µs) so each repeated
// frame is recognized as complete.
constexpr uint32_t kInterFrameGapUs = 2000;

enum : uint8_t {
    PIC_TX_PHASE_IDLE = 0,
    PIC_TX_PHASE_START_LOW = 1,
    PIC_TX_PHASE_BIT_HIGH = 2,
    PIC_TX_PHASE_BIT_LOW = 3,
    PIC_TX_PHASE_END_LOW = 4,
    PIC_TX_PHASE_INTER_GAP = 5,   // HIGH gap between continuously repeated frames
};

constexpr uint8_t kPicBtnRelease = 0xFF;
constexpr uint8_t kPicBtnC = 0xFB;   // Power
constexpr uint8_t kPicBtnE = 0xFD;   // Timer
constexpr uint8_t kPicBtnW = 0xFE;   // Lock
constexpr uint8_t kPicBtnN = 0xEF;   // Boost
constexpr uint8_t kPicBtnS = 0xF7;   // Self-clean

inline bool isDurationInRange(uint32_t clocks, uint32_t minUs, uint32_t maxUs) {
    uint32_t minCycles = minUs * kCyclesPerUs;
    uint32_t maxCycles = maxUs * kCyclesPerUs;
    return clocks >= minCycles && clocks <= maxCycles;
}

inline uint8_t mapPicButtonByteToTm1650Code(uint8_t picBtn) {
    switch (picBtn) {
        case kPicBtnC: return BUTTON_POWER;
        case kPicBtnE: return BUTTON_TIMER;
        case kPicBtnW: return BUTTON_LOCK;
        case kPicBtnN: return BUTTON_BOOST;
        case kPicBtnS: return BUTTON_SELF_CLEAN;
        case kPicBtnRelease:
        default:
            return 0x00;
    }
}

inline uint32_t usToClocks(uint32_t us) {
    return us * kCyclesPerUs;
}

// PIC wire format (DP G F E D C B A) → local format (E D G F B A DP C, MSB→LSB)
inline uint8_t convertPicSegToLocal(uint8_t picSeg) {
    uint8_t out = 0;
    if (picSeg & (1u << 0)) out |= (1u << 2);  // A → local A (bit2)
    if (picSeg & (1u << 1)) out |= (1u << 3);  // B → local B (bit3)
    if (picSeg & (1u << 2)) out |= (1u << 0);  // C → local C (bit0)
    if (picSeg & (1u << 3)) out |= (1u << 6);  // D → local D (bit6)
    if (picSeg & (1u << 4)) out |= (1u << 7);  // E → local E (bit7)
    if (picSeg & (1u << 5)) out |= (1u << 4);  // F → local F (bit4)
    if (picSeg & (1u << 6)) out |= (1u << 5);  // G → local G (bit5)
    if (picSeg & (1u << 7)) out |= (1u << 1);  // DP → local DP (bit1)
    return out;
}

// Local format (E D G F B A DP C, MSB→LSB) → PIC wire format (DP G F E D C B A)
inline uint8_t convertLocalSegToPic(uint8_t localSeg) {
    uint8_t out = 0;
    if (localSeg & (1u << 2)) out |= (1u << 0);  // local A → PIC A (bit0)
    if (localSeg & (1u << 3)) out |= (1u << 1);  // local B → PIC B (bit1)
    if (localSeg & (1u << 0)) out |= (1u << 2);  // local C → PIC C (bit2)
    if (localSeg & (1u << 6)) out |= (1u << 3);  // local D → PIC D (bit3)
    if (localSeg & (1u << 7)) out |= (1u << 4);  // local E → PIC E (bit4)
    if (localSeg & (1u << 4)) out |= (1u << 5);  // local F → PIC F (bit5)
    if (localSeg & (1u << 5)) out |= (1u << 6);  // local G → PIC G (bit6)
    if (localSeg & (1u << 1)) out |= (1u << 7);  // local DP → PIC DP (bit7)
    return out;
}

inline uint8_t mapPicLedWordToStatus(uint16_t ledWord) {
    uint8_t status = 0;

    if (ledWord & (1u << 15)) status |= (1u << LED_BOOST);         // o
    if (ledWord & (1u << 12)) status |= (1u << LED_SLEEP);         // s
    if (ledWord & (1u << 11)) status |= (1u << LED_POWER);         // w (mapped to local power slot)
    if (ledWord & (1u << 10)) status |= (1u << LED_OZONE);         // z
    if (ledWord & (1u << 6)) status |= (1u << LED_PUMP_LOW_FLOW);  // p
    if (ledWord & (1u << 5)) status |= (1u << LED_LOW_SALT);       // l
    if (ledWord & (1u << 1)) status |= (1u << LED_HIGH_SALT);      // i
    if (ledWord & (1u << 0)) status |= (1u << LED_SERVICE);        // r

    return status;
}

inline uint16_t mapLocalStatusToPicLedWord(uint8_t status) {
    uint16_t ledWord = 0;

    if (status & (1u << LED_BOOST)) ledWord |= (1u << 15);
    if (status & (1u << LED_SLEEP)) ledWord |= (1u << 12);
    if (status & (1u << LED_POWER)) ledWord |= (1u << 11);  // local power slot reused as working LED
    if (status & (1u << LED_OZONE)) ledWord |= (1u << 10);
    if (status & (1u << LED_PUMP_LOW_FLOW)) ledWord |= (1u << 6);
    if (status & (1u << LED_LOW_SALT)) ledWord |= (1u << 5);
    if (status & (1u << LED_HIGH_SALT)) ledWord |= (1u << 1);
    if (status & (1u << LED_SERVICE)) ledWord |= (1u << 0);

    return ledWord;
}

}  // namespace

PIC16F88PanelBackend::PIC16F88PanelBackend()
    : desiredLeftDigit(DISP_BLANK),
      desiredRightDigit(DISP_BLANK),
      desiredStatusLeds(0),
      desiredFrameBits(0),
      displayFrameDirty(false),
      displayEnabled(true),
    displayOverrideActive(false),
      latestButtonCode(0x00),
      writeIndex(0) {}

void PIC16F88PanelBackend::PicLineDecoder::reset() {
    highPulseStarted = false;
    frameBits = 0;
    bitCount = 0;
    lastFalling = 0;
}

uint8_t PIC16F88PanelBackend::readButtonCode(bool, uint8_t currentButtonCode) {
    // Passive mode: expose last decoded physical button; keep previous
    // value if no valid PIC button frame has been received yet.
    if (latestButtonCode == 0x00) {
        return currentButtonCode;
    }
    return latestButtonCode;
}

void PIC16F88PanelBackend::writeDisplay(uint8_t digitAddress, uint8_t value, uint8_t, bool displayOn) {
    if (!displayOverrideActive) {
        return;
    }

    switch (digitAddress) {
        case DIGIT1:
            desiredRightDigit = value;
            break;
        case DIGIT2:
            desiredLeftDigit = value;
            break;
        case DIGIT3:
            desiredStatusLeds = value;
            break;
        default:
            return;
    }

    displayEnabled = displayOn;
    rebuildDesiredFrame(displayOn);
}

void PIC16F88PanelBackend::setDisplayOverride(bool enabled) {
    displayOverrideActive = enabled;
    displayFrameDirty = enabled;
    if (!enabled) {
        displaySerializer.active = false;
        displaySerializer.phase = PIC_TX_PHASE_IDLE;
    }
}

void PIC16F88PanelBackend::onDisplayTaskStart() {
    desiredLeftDigit = DISP_BLANK;
    desiredRightDigit = DISP_BLANK;
    desiredStatusLeds = 0;
    displayEnabled = true;
    displayOverrideActive = false;
    displayFrameDirty = false;
}

void PIC16F88PanelBackend::onBusSample(uint8_t dataLevel, uint8_t clockLevel, uint32_t nowClocks) {
    decodeLine(displayToMainDecoder, clockLevel, nowClocks, true);
    decodeLine(mainToDisplayDecoder, dataLevel, nowClocks, false);
}

bool PIC16F88PanelBackend::isDisplayBusy() const {
    // Busy only while bits are on the wire — not during the inter-frame gap,
    // so Core1 can open its interrupt window between frames (watchdog).
    return displaySerializer.active && displaySerializer.phase != PIC_TX_PHASE_INTER_GAP;
}

const char* PIC16F88PanelBackend::name() const {
    return "PIC16F88-ACTIVE";
}

uint8_t PIC16F88PanelBackend::getDisplayOutputLevel(uint8_t passthroughLevel, uint32_t nowClocks) {
    if (!displayOverrideActive) {
        displaySerializer.active = false;
        displaySerializer.phase = PIC_TX_PHASE_IDLE;
        return passthroughLevel;
    }

    if (!displaySerializer.active && displayFrameDirty) {
        startDisplayFrame(desiredFrameBits, nowClocks);
    }

    if (!displaySerializer.active) {
        return 1;
    }

    uint32_t elapsed = nowClocks - displaySerializer.phaseStart;

    switch (displaySerializer.phase) {
        case PIC_TX_PHASE_START_LOW:
            if (elapsed >= usToClocks(kStartLowUs)) {
                displaySerializer.phase = PIC_TX_PHASE_BIT_HIGH;
                displaySerializer.phaseStart = nowClocks;
                return 1;
            }
            return 0;

        case PIC_TX_PHASE_BIT_HIGH: {
            uint32_t highDur = ((displaySerializer.frameBits >> displaySerializer.bitPos) & 0x01u)
                ? usToClocks(kOneHighUs)
                : usToClocks(kZeroHighUs);
            if (elapsed >= highDur) {
                displaySerializer.phase = PIC_TX_PHASE_BIT_LOW;
                displaySerializer.phaseStart = nowClocks;
                return 0;
            }
            return 1;
        }

        case PIC_TX_PHASE_BIT_LOW:
            if (elapsed >= usToClocks(kBitLowUs)) {
                displaySerializer.bitPos--;
                displaySerializer.phaseStart = nowClocks;
                if (displaySerializer.bitPos >= 0) {
                    displaySerializer.phase = PIC_TX_PHASE_BIT_HIGH;
                    return 1;
                }
                displaySerializer.phase = PIC_TX_PHASE_END_LOW;
                return 0;
            }
            return 0;

        case PIC_TX_PHASE_END_LOW:
            if (elapsed >= usToClocks(kEndLowUs)) {
                // Enter the inter-frame gap (HIGH). The panel needs continuous
                // refresh; if frames stop it reverts to the main board's default
                // ("." in standby). isDisplayBusy() reports false during the gap
                // so Core1's interrupt window may open between frames.
                displaySerializer.phase = PIC_TX_PHASE_INTER_GAP;
                displaySerializer.phaseStart = nowClocks;
                return 1;
            }
            return 0;

        case PIC_TX_PHASE_INTER_GAP:
            if (elapsed >= usToClocks(kInterFrameGapUs)) {
                // Repeat continuously: pick up the updated frame if one was
                // queued, otherwise resend the last frame.
                uint32_t nextBits = displayFrameDirty
                    ? desiredFrameBits
                    : displaySerializer.frameBits;
                startDisplayFrame(nextBits, nowClocks);
                return 0;
            }
            return 1;

        case PIC_TX_PHASE_IDLE:
        default:
            displaySerializer.active = false;
            return 1;
    }
}

void PIC16F88PanelBackend::decodeLine(PicLineDecoder &decoder, uint8_t level, uint32_t nowClocks, bool isDisplayToMain) {
    if (decoder.prevLevel == 0 && level == 1) {
        decoder.highPulseStarted = true;
        decoder.highPulseStart = nowClocks;
    } else if (decoder.prevLevel == 1 && level == 0) {
        if (decoder.highPulseStarted) {
            uint32_t highDur = nowClocks - decoder.highPulseStart;
            if (isDurationInRange(highDur, kZeroMinUs, kZeroMaxUs)) {
                decoder.frameBits = (decoder.frameBits << 1);
                decoder.bitCount++;
            } else if (isDurationInRange(highDur, kOneMinUs, kOneMaxUs)) {
                decoder.frameBits = (decoder.frameBits << 1) | 1u;
                decoder.bitCount++;
            } else {
                decoder.reset();
            }
        }
        decoder.highPulseStarted = false;
        decoder.lastFalling = nowClocks;
    }

    if (level == 1 && decoder.bitCount > 0 && decoder.lastFalling != 0) {
        uint32_t gap = nowClocks - decoder.lastFalling;
        if (gap > (kFrameGapUs * kCyclesPerUs)) {
            finalizeFrame(decoder, isDisplayToMain);
            decoder.reset();
        }
    }

    decoder.prevLevel = level;
}

void PIC16F88PanelBackend::finalizeFrame(PicLineDecoder &decoder, bool isDisplayToMain) {
    if (isDisplayToMain) {
        if (decoder.bitCount == 16) {
            uint8_t first = (uint8_t)((decoder.frameBits >> 8) & 0xFFu);
            uint8_t second = (uint8_t)(decoder.frameBits & 0xFFu);
            if ((uint8_t)(first ^ second) == 0xFFu) {
                latestButtonCode = mapPicButtonByteToTm1650Code(first);
                pushFrameToDebugRing(first, second);
            }
        }
        return;
    }

    if (decoder.bitCount == 32) {
        uint8_t left = (uint8_t)((decoder.frameBits >> 24) & 0xFFu);
        uint8_t right = (uint8_t)((decoder.frameBits >> 16) & 0xFFu);
        uint16_t leds = (uint16_t)(decoder.frameBits & 0xFFFFu);

        statusDigit2 = convertPicSegToLocal(left);
        statusDigit1 = convertPicSegToLocal(right);
        statusDigit3 = mapPicLedWordToStatus(leds);

        pushFrameToDebugRing((uint8_t)(decoder.frameBits >> 24), (uint8_t)(decoder.frameBits >> 16));
        pushFrameToDebugRing((uint8_t)(decoder.frameBits >> 8), (uint8_t)(decoder.frameBits));
    }
}

void PIC16F88PanelBackend::pushFrameToDebugRing(uint8_t b0, uint8_t b1) {
    dataReceivedBuffer[writeIndex][0] = b0;
    dataReceivedBuffer[writeIndex][1] = b1;
    writeIndex = (uint8_t)((writeIndex + 1) & 0x7Fu);
}

void PIC16F88PanelBackend::startDisplayFrame(uint32_t frameBits, uint32_t nowClocks) {
    displaySerializer.active = true;
    displaySerializer.frameBits = frameBits;
    displaySerializer.bitPos = 31;
    displaySerializer.phase = PIC_TX_PHASE_START_LOW;
    displaySerializer.phaseStart = nowClocks;
    displayFrameDirty = false;
}

void PIC16F88PanelBackend::rebuildDesiredFrame(bool displayOn) {
    uint8_t left = displayOn ? convertLocalSegToPic(desiredLeftDigit) : 0;
    uint8_t right = displayOn ? convertLocalSegToPic(desiredRightDigit) : 0;
    uint16_t leds = displayOn ? mapLocalStatusToPicLedWord(desiredStatusLeds) : 0;

    desiredFrameBits = ((uint32_t)left << 24)
        | ((uint32_t)right << 16)
        | leds;
    displayFrameDirty = true;
}
