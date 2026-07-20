// animation.h - Animation system for MayteraOS GUI
// Provides smooth transitions and effects for the windowing system
// Uses fixed-point math (no floating point) for kernel compatibility
#ifndef ANIMATION_H
#define ANIMATION_H

#include "../types.h"

// ============================================================================
// Fixed-Point Math
// ============================================================================

// Fixed-point format: 16.16 (16 bits integer, 16 bits fraction)
// This gives us range of -32768 to 32767 with precision of 1/65536
typedef int32_t fixed_t;

#define FIXED_SHIFT     16
#define FIXED_ONE       (1 << FIXED_SHIFT)      // 1.0 in fixed-point = 65536
#define FIXED_HALF      (FIXED_ONE >> 1)        // 0.5 in fixed-point = 32768
#define FIXED_MAX       0x7FFFFFFF              // Maximum fixed-point value

// Convert integer to fixed-point
#define INT_TO_FIXED(i)     ((fixed_t)((i) << FIXED_SHIFT))
// Convert fixed-point to integer (truncate)
#define FIXED_TO_INT(f)     ((int32_t)((f) >> FIXED_SHIFT))
// Convert fixed-point to integer (round)
#define FIXED_TO_INT_ROUND(f)  ((int32_t)(((f) + FIXED_HALF) >> FIXED_SHIFT))

// Fixed-point multiply: (a * b) >> 16
static inline fixed_t fixed_mul(fixed_t a, fixed_t b) {
    return (fixed_t)(((int64_t)a * b) >> FIXED_SHIFT);
}

// Fixed-point divide: (a << 16) / b
static inline fixed_t fixed_div(fixed_t a, fixed_t b) {
    if (b == 0) return FIXED_MAX;
    return (fixed_t)(((int64_t)a << FIXED_SHIFT) / b);
}

// ============================================================================
// Animation Constants
// ============================================================================

// Maximum number of concurrent animations
#define ANIM_MAX_ACTIVE     32

// Animation completion threshold (in fixed-point, ~0.999)
#define ANIM_COMPLETE_THRESHOLD (FIXED_ONE - 66)  // ~0.999 in fixed-point

// ============================================================================
// Easing Functions
// ============================================================================

typedef enum {
    EASING_LINEAR = 0,      // Linear interpolation (constant speed)
    EASING_EASE_IN,         // Slow start, fast end (accelerating)
    EASING_EASE_OUT,        // Fast start, slow end (decelerating)
    EASING_EASE_IN_OUT,     // Slow start and end, fast middle
    EASING_EASE_IN_QUAD,    // Quadratic ease in
    EASING_EASE_OUT_QUAD,   // Quadratic ease out
    EASING_EASE_IN_OUT_QUAD,// Quadratic ease in-out
    EASING_EASE_IN_CUBIC,   // Cubic ease in
    EASING_EASE_OUT_CUBIC,  // Cubic ease out
    EASING_EASE_IN_OUT_CUBIC,// Cubic ease in-out
    EASING_BOUNCE,          // Bounce effect at end
    EASING_COUNT
} easing_type_t;

// ============================================================================
// Animation Types
// ============================================================================

typedef enum {
    ANIM_TYPE_NONE = 0,
    ANIM_TYPE_FADE,         // Fade in/out (alpha transition)
    ANIM_TYPE_SCALE,        // Scale transform
    ANIM_TYPE_SLIDE,        // Position change (sliding)
    ANIM_TYPE_RESIZE,       // Size change
    ANIM_TYPE_COLOR,        // Color transition
    ANIM_TYPE_CUSTOM,       // Custom animation with callback
    ANIM_TYPE_COUNT
} animation_type_t;

// Animation state
typedef enum {
    ANIM_STATE_IDLE = 0,    // Animation not started
    ANIM_STATE_RUNNING,     // Animation in progress
    ANIM_STATE_PAUSED,      // Animation paused
    ANIM_STATE_COMPLETE,    // Animation finished
    ANIM_STATE_CANCELLED    // Animation was cancelled
} animation_state_t;

// Animation direction
typedef enum {
    ANIM_DIR_NORMAL = 0,    // Play forward
    ANIM_DIR_REVERSE,       // Play backward
    ANIM_DIR_ALTERNATE,     // Alternate forward/backward
    ANIM_DIR_ALTERNATE_REVERSE // Start backward, then alternate
} animation_direction_t;

// ============================================================================
// Animation Structures
// ============================================================================

// Forward declaration
struct animation;

// Animation update callback
// Called each frame with current animation progress (0 to FIXED_ONE)
typedef void (*anim_update_fn)(struct animation *anim, fixed_t progress);

// Animation completion callback
// Called when animation finishes or is cancelled
typedef void (*anim_complete_fn)(struct animation *anim, bool cancelled);

// Animation value union for different animation types
typedef union {
    // Fade animation (fixed-point alpha values)
    struct {
        fixed_t from_alpha;     // 0 to FIXED_ONE
        fixed_t to_alpha;
    } fade;

    // Scale animation (fixed-point scale values, FIXED_ONE = 1.0x)
    struct {
        fixed_t from_scale_x;
        fixed_t from_scale_y;
        fixed_t to_scale_x;
        fixed_t to_scale_y;
    } scale;

    // Slide animation (integer positions)
    struct {
        int32_t from_x;
        int32_t from_y;
        int32_t to_x;
        int32_t to_y;
    } slide;

    // Resize animation (integer dimensions)
    struct {
        int32_t from_width;
        int32_t from_height;
        int32_t to_width;
        int32_t to_height;
    } resize;

    // Color animation
    struct {
        uint32_t from_color;
        uint32_t to_color;
    } color;
} anim_value_t;

// Current interpolated values
typedef struct {
    fixed_t alpha;          // Current alpha (0 to FIXED_ONE)
    fixed_t scale_x;        // Current X scale (FIXED_ONE = 1.0x)
    fixed_t scale_y;        // Current Y scale
    int32_t x;              // Current X position
    int32_t y;              // Current Y position
    int32_t width;          // Current width
    int32_t height;         // Current height
    uint32_t color;         // Current color
} anim_current_t;

// Animation structure
typedef struct animation {
    uint32_t id;                    // Unique animation ID
    animation_type_t type;          // Animation type
    animation_state_t state;        // Current state
    easing_type_t easing;           // Easing function
    animation_direction_t direction;// Play direction

    // Timing
    uint32_t duration_ms;           // Total duration in milliseconds
    uint32_t elapsed_ms;            // Elapsed time in milliseconds
    uint32_t delay_ms;              // Delay before starting
    int32_t repeat_count;           // Number of repeats (-1 = infinite)
    int32_t current_repeat;         // Current repeat iteration

    // Values
    anim_value_t values;            // Start and end values
    anim_current_t current;         // Current interpolated values

    // Callbacks
    anim_update_fn on_update;       // Update callback (optional)
    anim_complete_fn on_complete;   // Completion callback (optional)
    void *user_data;                // User-defined data pointer
    void *target;                   // Animation target (e.g., window pointer)

    // Internal
    fixed_t progress;               // Current progress (0 to FIXED_ONE)
    bool active;                    // Is this animation slot in use?
    bool forward;                   // Current direction (for alternating)
} animation_t;

// ============================================================================
// Animation System API
// ============================================================================

/**
 * Initialize the animation system
 */
void anim_init(void);

/**
 * Create a new animation
 * @param type      Animation type
 * @param duration  Duration in milliseconds
 * @param easing    Easing function to use
 * @return          Animation pointer, or NULL on failure
 */
animation_t *anim_create(animation_type_t type, uint32_t duration_ms, easing_type_t easing);

/**
 * Destroy an animation and free its slot
 * @param anim      Animation to destroy
 */
void anim_destroy(animation_t *anim);

/**
 * Start an animation
 * @param anim      Animation to start
 */
void anim_start(animation_t *anim);

/**
 * Stop an animation
 * @param anim      Animation to stop
 * @param reset     If true, reset to initial state
 */
void anim_stop(animation_t *anim, bool reset);

/**
 * Pause an animation
 * @param anim      Animation to pause
 */
void anim_pause(animation_t *anim);

/**
 * Resume a paused animation
 * @param anim      Animation to resume
 */
void anim_resume(animation_t *anim);

/**
 * Check if animation is running
 * @param anim      Animation to check
 * @return          true if running
 */
bool anim_is_running(animation_t *anim);

/**
 * Check if animation is complete
 * @param anim      Animation to check
 * @return          true if complete
 */
bool anim_is_complete(animation_t *anim);

/**
 * Update all active animations
 * @param delta_ms  Time elapsed since last update in milliseconds
 */
void anim_tick(uint32_t delta_ms);

/**
 * Get animation progress (fixed-point)
 * @param anim      Animation
 * @return          Progress value (0 to FIXED_ONE)
 */
fixed_t anim_get_progress(animation_t *anim);

/**
 * Set animation progress manually (fixed-point)
 * @param anim      Animation
 * @param progress  Progress value (0 to FIXED_ONE)
 */
void anim_set_progress(animation_t *anim, fixed_t progress);

// ============================================================================
// Animation Configuration
// ============================================================================

/**
 * Set animation delay
 * @param anim      Animation
 * @param delay_ms  Delay in milliseconds before animation starts
 */
void anim_set_delay(animation_t *anim, uint32_t delay_ms);

/**
 * Set animation repeat count
 * @param anim      Animation
 * @param count     Number of times to repeat (-1 for infinite)
 */
void anim_set_repeat(animation_t *anim, int32_t count);

/**
 * Set animation direction
 * @param anim      Animation
 * @param dir       Animation direction
 */
void anim_set_direction(animation_t *anim, animation_direction_t dir);

/**
 * Set animation target (e.g., window pointer)
 * @param anim      Animation
 * @param target    Target pointer
 */
void anim_set_target(animation_t *anim, void *target);

/**
 * Set animation user data
 * @param anim      Animation
 * @param data      User data pointer
 */
void anim_set_user_data(animation_t *anim, void *data);

/**
 * Set animation callbacks
 * @param anim      Animation
 * @param on_update Update callback (called each frame)
 * @param on_complete Completion callback
 */
void anim_set_callbacks(animation_t *anim, anim_update_fn on_update, anim_complete_fn on_complete);

// ============================================================================
// Animation Value Setup
// ============================================================================

/**
 * Set fade animation values (fixed-point)
 * @param anim      Animation
 * @param from      Starting alpha (0 to FIXED_ONE)
 * @param to        Ending alpha (0 to FIXED_ONE)
 */
void anim_set_fade(animation_t *anim, fixed_t from, fixed_t to);

/**
 * Set fade animation values (integer percent 0-100)
 * @param anim      Animation
 * @param from_pct  Starting alpha percent (0-100)
 * @param to_pct    Ending alpha percent (0-100)
 */
void anim_set_fade_percent(animation_t *anim, int from_pct, int to_pct);

/**
 * Set scale animation values (fixed-point)
 * @param anim      Animation
 * @param from_x    Starting X scale (FIXED_ONE = 1.0x)
 * @param from_y    Starting Y scale
 * @param to_x      Ending X scale
 * @param to_y      Ending Y scale
 */
void anim_set_scale(animation_t *anim, fixed_t from_x, fixed_t from_y, fixed_t to_x, fixed_t to_y);

/**
 * Set scale animation values (integer percent 0-100 where 100 = 1.0x)
 * @param anim      Animation
 * @param from_x_pct Starting X scale percent
 * @param from_y_pct Starting Y scale percent
 * @param to_x_pct   Ending X scale percent
 * @param to_y_pct   Ending Y scale percent
 */
void anim_set_scale_percent(animation_t *anim, int from_x_pct, int from_y_pct, int to_x_pct, int to_y_pct);

/**
 * Set slide animation values
 * @param anim      Animation
 * @param from_x    Starting X position
 * @param from_y    Starting Y position
 * @param to_x      Ending X position
 * @param to_y      Ending Y position
 */
void anim_set_slide(animation_t *anim, int32_t from_x, int32_t from_y, int32_t to_x, int32_t to_y);

/**
 * Set resize animation values
 * @param anim      Animation
 * @param from_w    Starting width
 * @param from_h    Starting height
 * @param to_w      Ending width
 * @param to_h      Ending height
 */
void anim_set_resize(animation_t *anim, int32_t from_w, int32_t from_h, int32_t to_w, int32_t to_h);

/**
 * Set color animation values
 * @param anim      Animation
 * @param from      Starting color (ARGB)
 * @param to        Ending color (ARGB)
 */
void anim_set_color(animation_t *anim, uint32_t from, uint32_t to);

// ============================================================================
// Easing Functions
// ============================================================================

/**
 * Apply easing function to a linear progress value (fixed-point)
 * @param t         Linear progress (0 to FIXED_ONE)
 * @param easing    Easing function to apply
 * @return          Eased progress value (0 to FIXED_ONE)
 */
fixed_t easing_apply(fixed_t t, easing_type_t easing);

// ============================================================================
// Convenience Functions
// ============================================================================

/**
 * Create and start a fade-in animation for a target
 * @param target    Target object (e.g., window)
 * @param duration  Duration in milliseconds
 * @param on_complete Completion callback (optional)
 * @return          Animation pointer
 */
animation_t *anim_fade_in(void *target, uint32_t duration_ms, anim_complete_fn on_complete);

/**
 * Create and start a fade-out animation for a target
 * @param target    Target object
 * @param duration  Duration in milliseconds
 * @param on_complete Completion callback (optional)
 * @return          Animation pointer
 */
animation_t *anim_fade_out(void *target, uint32_t duration_ms, anim_complete_fn on_complete);

/**
 * Create and start a slide animation for a target
 * @param target    Target object
 * @param from_x    Starting X position
 * @param from_y    Starting Y position
 * @param to_x      Ending X position
 * @param to_y      Ending Y position
 * @param duration  Duration in milliseconds
 * @param on_complete Completion callback (optional)
 * @return          Animation pointer
 */
animation_t *anim_slide_to(void *target, int32_t from_x, int32_t from_y,
                           int32_t to_x, int32_t to_y, uint32_t duration_ms,
                           anim_complete_fn on_complete);

/**
 * Create and start a scale animation for a target (pop-in effect)
 * @param target    Target object
 * @param duration  Duration in milliseconds
 * @param on_complete Completion callback (optional)
 * @return          Animation pointer
 */
animation_t *anim_pop_in(void *target, uint32_t duration_ms, anim_complete_fn on_complete);

/**
 * Create and start a scale animation for a target (pop-out effect)
 * @param target    Target object
 * @param duration  Duration in milliseconds
 * @param on_complete Completion callback (optional)
 * @return          Animation pointer
 */
animation_t *anim_pop_out(void *target, uint32_t duration_ms, anim_complete_fn on_complete);

// ============================================================================
// Global Animation Settings
// ============================================================================

/**
 * Enable or disable all animations globally
 * When disabled, all animations complete instantly (retro mode)
 * @param enabled   true to enable, false to disable
 */
void anim_set_enabled(bool enabled);

/**
 * Check if animations are globally enabled
 * @return          true if enabled
 */
bool anim_is_enabled(void);

/**
 * Set global animation speed multiplier (fixed-point)
 * @param speed     Speed multiplier (FIXED_ONE = normal, 2*FIXED_ONE = double speed)
 */
void anim_set_speed(fixed_t speed);

/**
 * Set global animation speed multiplier (percent)
 * @param percent   Speed percent (100 = normal, 200 = double speed)
 */
void anim_set_speed_percent(int percent);

/**
 * Get global animation speed multiplier
 * @return          Current speed multiplier (fixed-point)
 */
fixed_t anim_get_speed(void);

/**
 * Cancel all running animations
 */
void anim_cancel_all(void);

/**
 * Get number of active animations
 * @return          Count of currently running animations
 */
uint32_t anim_get_active_count(void);

// ============================================================================
// Utility Functions
// ============================================================================

/**
 * Convert alpha (0-FIXED_ONE) to 8-bit alpha (0-255)
 */
static inline uint8_t anim_alpha_to_byte(fixed_t alpha) {
    return (uint8_t)((alpha * 255) >> FIXED_SHIFT);
}

/**
 * Convert 8-bit alpha (0-255) to fixed-point alpha
 */
static inline fixed_t anim_byte_to_alpha(uint8_t byte) {
    return (fixed_t)((uint32_t)byte << FIXED_SHIFT) / 255;
}

#endif // ANIMATION_H
