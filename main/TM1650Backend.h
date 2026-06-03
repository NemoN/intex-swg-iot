#ifndef TM1650_BACKEND_H
#define TM1650_BACKEND_H

#include "PanelBackend.h"
#include "TM1650.h"

class TM1650PanelBackend final : public IPanelBackend {
  public:
    TM1650PanelBackend();

    uint8_t readButtonCode(bool keyCodeSetByApi, uint8_t currentButtonCode) override;
    void writeDisplay(uint8_t digitAddress, uint8_t value, uint8_t intensity, bool displayOn) override;
    void setDisplayOverride(bool enabled) override;
    void onDisplayTaskStart() override;
    void onBusSample(uint8_t dataLevel, uint8_t clockLevel, uint32_t nowClocks) override;
    uint8_t getDisplayOutputLevel(uint8_t passthroughLevel, uint32_t nowClocks) override;
    const char* name() const override;

  private:
    TM1650 module;
};

#endif
