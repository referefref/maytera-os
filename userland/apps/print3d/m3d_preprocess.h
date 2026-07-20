// m3d_preprocess.h - MayteraOS "3D Print" (#396)
//
// Faithful C port of M33-Fio's "shared library source/preprocessor.cpp"
// (donovan6000, GPL): the host-side g-code preprocessing pipeline the M3D Micro
// requires. All settings and mutable state live in an m3d_ctx_t (the reference
// used file-scope globals). Passes ported, in pipeline order:
//   center-model, validation, preparation (startup/shutdown), wave-bonding,
//   thermal-bonding, bed-compensation, backlash-compensation, skew-compensation,
//   feed-rate persistence, plus collectPrintInformation() (dimensions + center
//   displacement + filament-derived tack/temperature settings).
// Mid-print filament change is ported but off by default.
#ifndef M3D_PREPROCESS_H
#define M3D_PREPROCESS_H

#include <stdint.h>
#include <stdbool.h>
#include "m3d_gcode.h"

// Filament types (M33-Fio filamentTypes enum order).
enum { M3D_FIL_NO_TYPE, M3D_FIL_ABS, M3D_FIL_PLA, M3D_FIL_HIPS, M3D_FIL_OTHER,
       M3D_FIL_FLX, M3D_FIL_TGH, M3D_FIL_CAM, M3D_FIL_ABS_R };

// Firmware types.
enum { M3D_FW_M3D, M3D_FW_M3D_MOD, M3D_FW_IME, M3D_FW_UNKNOWN };

// Printer colors.
enum { M3D_CLR_BLACK, M3D_CLR_WHITE, M3D_CLR_BLUE, M3D_CLR_GREEN,
       M3D_CLR_ORANGE, M3D_CLR_CLEAR, M3D_CLR_SILVER, M3D_CLR_PURPLE };

#define M3D_MAX_FIL_CHANGE_LAYERS 32

typedef struct {
    // ---- Configurable settings (setters in the reference) ----
    double backlashX, backlashY, backlashSpeed;
    double backRightOrientation, backLeftOrientation, frontLeftOrientation, frontRightOrientation;
    double bedHeightOffset;
    double backRightOffset, backLeftOffset, frontLeftOffset, frontRightOffset;
    uint16_t filamentTemperature;
    int filamentType;
    bool useValidationPreprocessor;
    bool usePreparationPreprocessor;
    bool useWaveBondingPreprocessor;
    bool useThermalBondingPreprocessor;
    bool useBedCompensationPreprocessor;
    bool useBacklashCompensationPreprocessor;
    bool useCenterModelPreprocessor;
    bool ignorePrintDimensionLimitations;
    bool usingHeatbed;
    bool printingTestBorder;
    bool printingBacklashCalibration;
    int printerColor;
    bool calibrateBeforePrint;
    bool removeFanCommands;
    bool removeTemperatureCommands;
    bool useGpio;
    uint16_t gpioLayer;
    uint16_t heatbedTemperature;
    double externalBedHeight;
    bool expandPrintableRegion;
    bool changeLedBrightness;
    int firmwareType;
    double skewX, skewY;
    bool useSkewCompensationPreprocessor;
    uint64_t midPrintFilamentChangeLayers[M3D_MAX_FIL_CHANGE_LAYERS];
    int midPrintFilamentChangeLayerCount;

    // ---- Derived / mutable state (reset by m3d_reset_settings) ----
    int16_t detectedFanSpeed;
    bool detectedMidPrintFilamentChange;
    bool objectSuccessfullyCentered;

    double currentE; char currentF[16];
    double currentZ;
    bool layerDetectionRelativeMode;
    bool onNewPrintedLayer;
    double tackPointAngle, tackPointTime;
    uint8_t temperatureStabalizationDelay, fanSpeed;
    int8_t firstLayerTemperatureChange;

    uint64_t midPrintFilamentChangeLayerCounter;

    double displacementX, displacementY;

    bool addedIntro, addedOutro;
    uint64_t preparationLayerCounter;

    uint8_t waveStep;
    bool waveBondingRelativeMode;
    uint8_t waveBondingLayerCounter;
    bool waveBondingChangesPlane;
    double waveBondingPositionRelativeX, waveBondingPositionRelativeY,
           waveBondingPositionRelativeZ, waveBondingPositionRelativeE;

    bool thermalBondingRelativeMode;
    uint8_t thermalBondingLayerCounter;

    bool bedCompensationRelativeMode, bedCompensationChangesPlane;
    double bedCompensationPositionAbsoluteX, bedCompensationPositionAbsoluteY;
    double bedCompensationPositionRelativeX, bedCompensationPositionRelativeY,
           bedCompensationPositionRelativeZ, bedCompensationPositionRelativeE;

    bool backlashCompensationRelativeMode;
    char valueF[16];
    int previousDirectionX, previousDirectionY;
    double compensationX, compensationY;
    double backlashPositionRelativeX, backlashPositionRelativeY,
           backlashPositionRelativeZ, backlashPositionRelativeE;

    bool skewCompensationRelativeMode;
    double skewCompensationPositionAbsoluteZ, skewCompensationPositionRelativeZ;

    // Print dimensions
    double maxXExtruderLow, maxXExtruderMedium, maxXExtruderHigh;
    double maxYExtruderLow, maxYExtruderMedium, maxYExtruderHigh, maxZExtruder;
    double minXExtruderLow, minXExtruderMedium, minXExtruderHigh;
    double minYExtruderLow, minYExtruderMedium, minYExtruderHigh, minZExtruder;

    // wave/thermal reference + previous gcode state
    m3d_gcode_t waveBondingPreviousGcode, waveBondingRefrenceGcode;
    m3d_gcode_t thermalBondingPreviousGcode, thermalBondingRefrenceGcode;
    // scratch layer-tracking for "first time a layer extrudes" detection
    double printedLayers[4096];
    int    printedLayerCount;
} m3d_ctx_t;

// Fill ctx with library defaults (equivalent to a fresh library load: all
// preprocessors off, iMe firmware, PLA at 215C). Call before setting options.
void m3d_ctx_defaults(m3d_ctx_t *ctx);

// Reset the mutable pre-processor state (M33-Fio resetPreprocessorSettings()).
void m3d_reset_settings(m3d_ctx_t *ctx);

// Convenience setters that mirror the reference's string-keyed setters.
void m3d_set_filament_type(m3d_ctx_t *ctx, const char *value);
void m3d_set_firmware_type(m3d_ctx_t *ctx, const char *value);
void m3d_set_printer_color(m3d_ctx_t *ctx, const char *value);

// Scan the file dimensions, compute the center-model displacement, and derive
// filament tack/temperature settings. Returns false if the print is out of the
// bed's printable region (and dimension limits are enforced). applyPreprocessors
// mirrors the reference argument.
bool m3d_collect_print_information(m3d_ctx_t *ctx, const char *const *lines,
                                   int nlines, bool applyPreprocessors);

// Run the full pipeline over the in-memory g-code. Each produced ASCII command
// line is passed to emit(). Returns 0 on success, <0 on allocation failure.
int m3d_preprocess(m3d_ctx_t *ctx, const char *const *lines, int nlines,
                   void (*emit)(void *user, const char *ascii_line), void *user);

#endif // M3D_PREPROCESS_H
