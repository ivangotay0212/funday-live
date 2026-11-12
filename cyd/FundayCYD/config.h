#pragma once
// ------------------------------------------------------------------ Funday Live on the CYD
// Edit these, then upload. Everything else lives in FundayCYD.ino.

#define WIFI_SSID      "your-wifi-name"
#define WIFI_PASS      "your-wifi-password"

#define LEAGUE_ID      "1389328536841519104"   // same league as the web page
#define MY_ROSTER_ID   0      // 0 = pick your team on the screen at first boot (saved after that)

// POSIX time zone string, used for kickoff times. Eastern by default.
#define TZ_INFO        "EST5EDT,M3.2.0,M11.1.0"

#define SOUND_ON       0      // 1 = play the board's jingles on the speaker connector (GPIO 26)
#define LED_ON         1      // 1 = flash the RGB LED on the back for touchdowns and red zone trips
#define INVERT_COLORS  0      // set to 1 if colours come out as a photo negative on your unit
#define AUTO_CYCLE_SEC 0      // >0 = flip to the next screen every N seconds on its own

// Touch calibration (raw XPT2046 readings at the screen edges). Defaults suit most 2432S028R boards.
// If taps land in the wrong place on the team picker, watch Serial Monitor while tapping corners.
#define TOUCH_X_MIN    200
#define TOUCH_X_MAX    3700
#define TOUCH_Y_MIN    240
#define TOUCH_Y_MAX    3800
