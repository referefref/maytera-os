// presets.h - built-in slicing profile for curaslice (MayteraOS).
//
// A single fixed preset for P1 bring-up: 0.2 mm layers, 2 perimeters, 20 percent
// grid infill, no support. Values are in CuraEngine 15.04 native units (microns
// for lengths, enum ordinals for patterns) and are expressed as the same
// "key=value" strings the engine accepts via its -s option and its
// ConfigSettings::setSetting(key, value) entry point. Only the fields that
// differ from settings.cpp's constructor defaults, plus the load-bearing ones,
// are listed so the intent is explicit.
//
// Apply by splitting each entry on '=' and calling config.setSetting(key, val),
// exactly as main.cpp does for a -s argument. Later phases (settings UI, M3D
// profile) will layer additional presets on top of this baseline.
#ifndef CURASLICE_PRESETS_H
#define CURASLICE_PRESETS_H

// 0.2 mm / 2 walls / 20 percent grid infill / no support.
static const char* const CURASLICE_PRESET_DEFAULT[] = {
    "layerThickness=200",          // 0.2 mm layers (default is 100 = 0.1 mm)
    "initialLayerThickness=200",   // 0.2 mm first layer for a flat baseline
    "nozzleSize=400",              // 0.4 mm nozzle
    "extrusionWidth=400",          // 0.4 mm lines
    "layer0extrusionWidth=600",    // wider, better-adhering first layer
    "insetCount=2",                // 2 perimeter walls
    "downSkinCount=3",             // ~0.6 mm solid bottom at 0.2 mm layers
    "upSkinCount=3",               // ~0.6 mm solid top at 0.2 mm layers
    "sparseInfillLineDistance=2000", // 2.0 mm spacing = 20 percent at 0.4 mm width
    "infillPattern=1",             // INFILL_GRID
    "infillOverlap=15",            // 15 percent wall/infill overlap
    "skirtLineCount=1",            // single skirt loop for priming
    "skirtDistance=6000",          // 6 mm skirt offset
    "supportType=0",               // SUPPORT_TYPE_GRID (unused while disabled)
    "supportAngle=-1",             // -1 disables support generation
    "supportEverywhere=0",         // no support
    "spiralizeMode=0",             // solid model, not vase mode
    "gcodeFlavor=0",               // GCODE_FLAVOR_REPRAP (standard g-code)
};

static const unsigned CURASLICE_PRESET_DEFAULT_COUNT =
    sizeof(CURASLICE_PRESET_DEFAULT) / sizeof(CURASLICE_PRESET_DEFAULT[0]);

#endif // CURASLICE_PRESETS_H
