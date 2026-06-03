#ifndef PANEL_BACKEND_H
#define PANEL_BACKEND_H

#include <stdint.h>

class IPanelBackend {
  public:
    virtual ~IPanelBackend() = default;

    // Called from the display task to read the current physical button state.
    virtual uint8_t readButtonCode(bool keyCodeSetByApi, uint8_t currentButtonCode) = 0;

    // Called whenever the firmware pushes one value to a display register.
    virtual void writeDisplay(uint8_t digitAddress, uint8_t value, uint8_t intensity, bool displayOn) = 0;

    // Enables a backend-specific temporary display override. Backends that do
    // not need explicit control can ignore this.
    virtual void setDisplayOverride(bool enabled) = 0;

    // Called during task startup to initialize display state.
    virtual void onDisplayTaskStart() = 0;

    // Called from Core1 with sampled main-bus line levels (data=DIO, clock=CLK).
    virtual void onBusSample(uint8_t dataLevel, uint8_t clockLevel, uint32_t nowClocks) = 0;

    // Called from Core1 to determine what level should be driven onto the
    // display output line. Backends that don't actively drive the line should
    // just return passthroughLevel unchanged.
    virtual uint8_t getDisplayOutputLevel(uint8_t passthroughLevel, uint32_t nowClocks) = 0;

    // True while the backend is actively bit-banging a frame on the display
    // output line. Core1 uses this to avoid opening its cooperative interrupt
    // window mid-frame (an ISR preemption would stretch a bit and corrupt the
    // frame). Backends that send frames atomically can leave this at false.
    virtual bool isDisplayBusy() const { return false; }

    // Human-readable backend name for logging/debug.
    virtual const char* name() const = 0;
};

IPanelBackend* getPanelBackend();

#endif
