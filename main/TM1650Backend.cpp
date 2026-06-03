#include "TM1650Backend.h"

#include "IntexSWG.h"

TM1650PanelBackend::TM1650PanelBackend()
    : module(dataDispPin, clockDispPin, 2, true, 3, TM1650_DISPMODE_4x8) {}

uint8_t TM1650PanelBackend::readButtonCode(bool keyCodeSetByApi, uint8_t currentButtonCode) {
    if (keyCodeSetByApi) {
        return currentButtonCode;
    }
    return module.getButtonPressedCode();
}

void TM1650PanelBackend::writeDisplay(uint8_t digitAddress, uint8_t value, uint8_t intensity, bool displayOn) {
    module.setupDisplay(displayOn, intensity);
    module.setSegments(value, (digitAddress & 0b111) >> 1);
}

void TM1650PanelBackend::setDisplayOverride(bool) {
    // TM1650 backend always drives its dedicated display bus directly.
}

void TM1650PanelBackend::onDisplayTaskStart() {
    module.clearDisplay();
    module.setupDisplay(true, 4);
}

void TM1650PanelBackend::onBusSample(uint8_t, uint8_t, uint32_t) {
    // TM1650 path is decoded in Core1 itself.
}

uint8_t TM1650PanelBackend::getDisplayOutputLevel(uint8_t passthroughLevel, uint32_t) {
    return passthroughLevel;
}

const char* TM1650PanelBackend::name() const {
    return "TM1650";
}
