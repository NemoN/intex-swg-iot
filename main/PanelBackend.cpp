#include "PanelBackend.h"

#include "PIC16F88Backend.h"
#include "TM1650Backend.h"

IPanelBackend* getPanelBackend() {
#if defined(CONFIG_INTSWG_PANEL_BACKEND_PIC16F88)
    static PIC16F88PanelBackend pic16f88Backend;
    return &pic16f88Backend;
#else
    static TM1650PanelBackend tm1650Backend;
    return &tm1650Backend;
#endif
}
