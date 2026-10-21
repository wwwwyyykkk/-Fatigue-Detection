/**
 * display_data.h - Global display data structure for TFT screen
 * Shared between main.cpp (producer) and display task (consumer)
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "perclos.hpp"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Display data structure updated by main task */
typedef struct {
    float    perclos;           // Current PERCLOS percentage (0-100)
    uint8_t  fatigue_level;     // 0=NORMAL, 1=MILD, 2=SEVERE
    bool     face_detected;     // True if face detected in current frame
    int      face_w;            // Face bounding box width
    int      face_h;            // Face bounding box height
    uint32_t total_frames;      // Total processed frames
    uint32_t face_frames;       // Frames with face detected
    uint32_t closed_frames;     // Frames with closed eyes
    int      valid_count;       // PERCLOS window valid count (0-30)
    bool     is_warm;           // PERCLOS window filled (>=30 samples)
    int      driver_id;         // Current driver ID (-1 = unknown)
    float    driver_similarity; // Face recognition similarity (0-1)
    uint32_t driving_minutes;   // Current driving session duration (minutes)
    uint8_t  weather_risk;      // 0=clear, 1=rain/snow, 2=fog/haze (affects thresholds)
    uint8_t  drive_warn_min;    // Weather-adjusted driving warning threshold (minutes/10)
    /* ── VirtualGPS data (received via UART2) ── */
    float    gps_speed;         // km/h from VirtualGPS
    int      gps_location;      // 0=normal, 1=service, 2=emergency
    int      gps_driver_id;     // Driver ID from VirtualGPS
    int      gps_drive_seconds; // Driving seconds from VirtualGPS
    float    gps_lat;           // Latitude
    float    gps_lng;           // Longitude
    bool     gps_connected;     // True = receiving data from VirtualGPS
    uint8_t  gps_mode;          // 0=real GPS(卫星定位), 1=mock(模拟)
    /* ── UI state ── */
    bool     identifying;       // True = face recognition in progress
    bool     camera_ready;      // True = UVC camera streaming
    /* ── SNTP time ── */
    int      time_hour;
    int      time_min;
    int      time_sec;
    int      time_year;
    int      time_month;
    int      time_day;
    int      time_weekday;      // 0=Sun..6=Sat
    bool     time_valid;
    /* ── Rest mode ── */
    bool     rest_active;           // Rest mode active
    int      rest_total_minutes;    // Requested rest duration (minutes)
    int      rest_elapsed_seconds;  // Seconds elapsed since rest started
    int      rest_snap_fatigue;     // Fatigue level snapshot at rest start (-1=none)
    int      rest_snap_drive_min;   // Drive minutes snapshot at rest start
    float    rest_snap_perclos;     // PERCLOS value snapshot at rest start
    int      rest_recovery_pct;     // Recovery percentage 0-100 (-1=not computed yet)
    int      rest_suggested_min;    // System-suggested rest duration
    bool     rest_mandatory;        // Mandatory lock (severe fatigue → cannot early-exit)
    bool     rest_face_ok;          // Post-rest face re-verification passed
    int      rest_extend_count;     // Number of auto-extensions (max 2)
    /* ── Fatigue prediction (linear regression on PERCLOS history) ── */
    float    perclos_history[20];   // PERCLOS snapshots (1 per 30s, last 10 min)
    int      history_count;         // Number of valid snapshots in buffer
    float    perclos_slope;         // PERCLOS trend (percentage points per minute)
    int      pred_minutes;          // Predicted minutes to next fatigue level (-1=no rise / insufficient data)
    bool     pred_ready;            // True once regression has run at least once (real prediction decided);
                                    // false while warming up (UI draws reference projection instead)
    /* ── Yawn detection (mouth dark-ratio method) ── */
    float    mouth_dark_ratio;    // Mouth ROI dark pixel ratio [0.0-1.0]
    int      yawn_frames;         // Consecutive high-dark frames
    int      yawn_count;          // Total yawn events detected
    bool     yawn_active;         // Currently in a yawn event
    int      yawn_boost;          // PERCLOS boost from yawns (0-15%, +5%/yawn, decay 1%/min)
} display_data_t;

/** @brief Global display data (defined in main.cpp) */
extern display_data_t g_display_data;

#ifdef __cplusplus
}
#endif
