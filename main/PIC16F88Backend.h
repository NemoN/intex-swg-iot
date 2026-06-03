#ifndef PIC16F88_BACKEND_H
#define PIC16F88_BACKEND_H

#include <stdint.h>
#include "PanelBackend.h"

class PIC16F88PanelBackend final : public IPanelBackend {
  public:
    PIC16F88PanelBackend();

    uint8_t readButtonCode(bool keyCodeSetByApi, uint8_t currentButtonCode) override;
    void writeDisplay(uint8_t digitAddress, uint8_t value, uint8_t intensity, bool displayOn) override;
    void setDisplayOverride(bool enabled) override;
    void onDisplayTaskStart() override;
    void onBusSample(uint8_t dataLevel, uint8_t clockLevel, uint32_t nowClocks) override;
    uint8_t getDisplayOutputLevel(uint8_t passthroughLevel, uint32_t nowClocks) override;
    bool isDisplayBusy() const override;
    const char* name() const override;

  private:
    struct PicDisplaySerializer {
        bool active = false;
        uint32_t frameBits = 0;
        int8_t bitPos = 31;
        uint8_t phase = 0;
        uint32_t phaseStart = 0;
    };

    struct PicLineDecoder {
        uint8_t prevLevel = 1;
        bool highPulseStarted = false;
        uint32_t highPulseStart = 0;
        uint32_t lastFalling = 0;
        uint32_t frameBits = 0;
        uint8_t bitCount = 0;

        void reset();
    };

    void decodeLine(PicLineDecoder &decoder, uint8_t level, uint32_t nowClocks, bool isDisplayToMain);
    void finalizeFrame(PicLineDecoder &decoder, bool isDisplayToMain);
    void pushFrameToDebugRing(uint8_t b0, uint8_t b1);
    void startDisplayFrame(uint32_t frameBits, uint32_t nowClocks);
    void rebuildDesiredFrame(bool displayOn);

    PicLineDecoder displayToMainDecoder;
    PicLineDecoder mainToDisplayDecoder;
    PicDisplaySerializer displaySerializer;
    volatile uint8_t desiredLeftDigit;
    volatile uint8_t desiredRightDigit;
    volatile uint8_t desiredStatusLeds;
    volatile uint32_t desiredFrameBits;
    volatile bool displayFrameDirty;
    volatile bool displayEnabled;
    volatile bool displayOverrideActive;
    volatile uint8_t latestButtonCode;
    uint8_t writeIndex;
};

#endif
