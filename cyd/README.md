# Funday Live on the CYD

The dashboard, shrunk to a 2.8" ESP32-2432S028R ("Cheap Yellow Display"). It pulls the same Sleeper league and ESPN scoreboard as `index.html`, directly from the board, with no server in between.

Tap anywhere to flip screens. Hold for 1.5 seconds to pick a different team.

| Screen | What's on it |
| --- | --- |
| Scorebug | Your team and your opponent with big scores, the win bar, who's live and who's still to play, and a lineup alert strip (red when a starter can't score, gold for questionables) |
| My lineup | All starters. Gold edge = has the ball, red = red zone, blue = defense on the field. Injury chips, game score and clock on the right |
| Opponent | Same view for the other side |
| NFL | Every game this week. Green edge means one of your starters is in it, red means theirs, half and half means both |

A scores ticker runs along the bottom of every screen. A touchdown or red zone trip involving either lineup takes over the whole display for 6 seconds (tap to dismiss), flashes the RGB LED on the back, and can play the board's jingles on the speaker connector.

## Setup (Arduino IDE)

1. Boards Manager: install **esp32** by Espressif.
2. Library Manager: install **TFT_eSPI** (Bodmer), **ArduinoJson** 7.x (Benoit Blanchon), **XPT2046_Touchscreen** (Paul Stoffregen).
3. Copy `cyd/User_Setup.h` over `Arduino/libraries/TFT_eSPI/User_Setup.h`.
4. Open `cyd/FundayCYD/FundayCYD.ino`. Put your Wi-Fi name and password in the `config.h` tab.
5. Tools menu: Board **ESP32 Dev Module**, Partition Scheme **Huge APP (3MB No OTA)**, then Upload.

On first boot it asks which team is yours. That choice is saved on the board. Set `MY_ROSTER_ID` in `config.h` if you'd rather hard-code it.

The first load takes a minute or two. Sleeper's player list is about 15 MB, and the board streams all of it to keep the ~100 names and injury tags it needs. After that it refreshes every 6 hours when no games are on (12 hours at most), or sooner when a new player turns up in a lineup. Scores update every 15 seconds while games are live and every minute otherwise.

## If something looks off

- **White or black screen:** check the `User_Setup.h` copy. If it crashes on boot with esp32 core 3.x, install core 2.0.17 from Boards Manager.
- **Colours look like a photo negative:** set `INVERT_COLORS 1` in `config.h`.
- **Taps land in the wrong spot on the team picker:** open Serial Monitor at 115200, tap the corners, and adjust the `TOUCH_*` numbers in `config.h`.
- **"OFFLINE" in the header:** the reason is in Serial Monitor (Wi-Fi, HTTP code, or JSON error).
