# IoT-Based Security System for Openly Displayed Assets

A low-cost IoT security system that protects valuables on open display. A fingerprint sensor identifies authorised users, a load cell monitors the item's weight, and a finite state machine links the two. If an unauthorised weight change occurs, the system sounds an alarm, captures a photo, and sends it with a timestamped weight report to the owner's Telegram.

![Prototype](<img width="3048" height="4064" alt="IMG_20260830_084850_060" src="https://github.com/user-attachments/assets/ece0ddd0-d93c-45dc-8317-05c2d95483f5" />
)

## Features
- Fingerprint authentication with the AS608 optical sensor
- Continuous weight monitoring with an HX711 and a 20 kg load cell
- Exponential moving average (EMA) filter to suppress sensor noise
- Five-state finite state machine: ESTABLISHING, ARMED, DISARMED, ALARM, STAY_DISARMED
- Photo capture with the ESP32-S3 CAM and real-time alerts through the Telegram Bot API
- Local buzzer alarm and a 20x4 I2C LCD for status messages

## Hardware
| Component | Purpose |

| ESP32-S3 CAM (N16R8) | Main controller, camera, Wi-Fi |
| AS608 fingerprint sensor | Biometric authentication (UART2) |
| HX711 + 20 kg load cell | Weight sensing |
| 20x4 I2C LCD | Status display |
| Buzzer + BC547 transistor | Local alarm |
| LM2596 buck converter | Power regulation |

The full circuit schematic is in `docs/schematic.png`.

## How it works
![Flowchart](<img width="1024" height="1536" alt="file_00000000cab07243a14765dc35e68e2a" src="https://github.com/user-attachments/assets/aba6b621-4e87-4d1c-8344-ce6e1849c638" />
)

1. On start-up the system connects to Wi-Fi and calibrates the load cell baseline.
2. In the ARMED state, any weight change beyond the threshold triggers the ALARM state.
3. A valid fingerprint moves the system to DISARMED, so the item can be handled.
4. In the ALARM state the buzzer sounds, a photo is captured, and a Telegram alert is sent.

## Setup
1. Install the Arduino IDE with ESP32 board support.
2. Install the required libraries (Adafruit fingerprint library, HX711 library, LiquidCrystal_I2C).
3. Create a Telegram bot with BotFather and note the bot token and your chat ID.
4. Copy `secrets.example.h` to `secrets.h` and fill in your own Wi-Fi name, Wi-Fi password, bot token, and chat ID.
5. Select the ESP32-S3 board, upload the sketch in `firmware/`, and enrol fingerprints.

**Never commit your real `secrets.h`.**

## Test results
| Metric | Result |

| False Acceptance Rate (100 trials) | 0.0% |
| False Rejection Rate (100 trials) | 16.0% |
| Weight detection floor | 20 g |
| Weight measurement accuracy (up to 2 kg) | within 2.5% |
| Mean Telegram alert latency (10 trials) | 12.4 s |

## Limitations and future work
- The camera cannot capture usable images in complete darkness (infrared illumination planned)
- Multiple fingerprint templates per finger to reduce false rejections
- GSM fallback alerts, adaptive thresholds, and multi-item monitoring

## Author
Ameh Eliezer Otabo, Department of Electrical and Electronics Engineering, Federal University of Technology, Minna, Nigeria.
