# Smart Parking System using STM32 and ESP32

An embedded Smart Parking System that monitors parking slot availability, controls vehicle entry, supports RFID-based authentication for booked slots, and provides Wi-Fi-based online slot booking.

## 1. Project Overview

The system uses two microcontrollers:

* **STM32F401CCU6:** Main controller responsible for vehicle detection, slot monitoring, RFID authentication, gate control, LED indicators, OLED display, buzzer, and automatic parking lights.
* **ESP32 Dev Module:** Provides Wi-Fi connectivity, hosts the parking booking webpage, manages reservations, and communicates with STM32 through UART.

The system currently supports three parking slots.

## 2. Repository Structure

```text
Smart-Parking-System/
│
├── stmcode.c
├── espcode.ino
├── README.md
└── docs/
    ├── flowchart.png
    ├── circuit-diagram.png
    └── prototype.jpg
```

## 3. Hardware Requirements

| Component              |                               Quantity |
| ---------------------- | -------------------------------------: |
| STM32F401CCU6          |                                      1 |
| ESP32 Dev Module       |                                      1 |
| IR Sensors             | 4 (3 slot sensors + 1 entrance sensor) |
| RC522 RFID Module      |                                      1 |
| RFID Cards             |                                    2–4 |
| SG90 Servo Motor       |                                      1 |
| 0.96-inch I2C OLED     |                                      1 |
| LEDs                   |                                      3 |
| Active Buzzer          |                                      1 |
| LDR Module             |                                      1 |
| Jumper Wires           |                            As required |
| Breadboard             |                            As required |
| Regulated Power Supply |                            As required |

## 4. Software Requirements

Install the following software:

1. **Keil MDK-ARM / Keil uVision** – STM32 firmware development.
2. **STM32CubeF4 HAL and CMSIS packages** – Required STM32 libraries.
3. **Arduino IDE** – ESP32 programming.
4. **ESP32 Board Package by Espressif Systems** – ESP32 board support.
5. USB drivers, if required by your development boards.

## 5. Hardware Connections

### A. STM32 to ESP32 UART Communication

| STM32F401CCU6   | ESP32        |
| --------------- | ------------ |
| PA2 (USART2 TX) | GPIO25 (RX2) |
| PA3 (USART2 RX) | GPIO26 (TX2) |
| GND             | GND          |

**Important:** Connect the grounds of both boards. The UART communication uses 115200 baud, 8 data bits, no parity, and 1 stop bit (115200, 8N1).

### B. STM32 Peripheral Connections

| Component          | STM32 Pin      |
| ------------------ | -------------- |
| Entrance IR Sensor | PB10           |
| Slot 1 IR Sensor   | PA0            |
| Slot 2 IR Sensor   | PA1            |
| Slot 3 IR Sensor   | PA4            |
| Slot 1 LED         | PB13           |
| Slot 2 LED         | PB14           |
| Slot 3 LED         | PB15           |
| Buzzer             | PB12           |
| Servo Signal       | PB4 (TIM3_CH1) |
| OLED SDA           | PB9            |
| OLED SCL           | PB8            |
| LDR Digital Output | PB0            |
| Parking Light 1    | PA8            |
| Parking Light 2    | PA9            |

### C. RC522 RFID Connections

| RC522 Pin | STM32 Pin |
| --------- | --------- |
| SDA / SS  | PB5       |
| SCK       | PA5       |
| MOSI      | PA7       |
| MISO      | PA6       |
| RST       | PB1       |
| GND       | GND       |
| 3.3V      | 3.3V      |

**Warning:** The RC522 operates at 3.3V. Do not connect its power pin to 5V.

### STM32 Device and Software Configuration

**Microcontroller:** STM32F401CCU6
**STM32 Family:** STM32F4 Series
**Target Device in Keil:** STM32F401CCUx
**IDE:** Keil MDK-ARM (Keil uVision)
**Firmware Framework:** STM32Cube HAL – Classic HAL
**Clock Source:** Internal HSI – 16 MHz

#### 5. How to run the stm32 firmware

1. Open Keil uVision.
2. Create a new project.
3. Select the device:
   `STMicroelectronics → STM32F4 Series → STM32F401CCUx`
4. Install the STM32F4 Device Family Pack (DFP) if prompted.
5. Confirm the device selection.


Open **Project → Manage → Run-Time Environment**.

Select the following components:

| Category            | Component   |
| ------------------- | ----------- |
| CMSIS               | CORE        |
| Device              | Startup     |
| STM32Cube Framework | Classic HAL |
| HAL                 | Common      |
| HAL                 | Cortex      |
| HAL                 | DMA         |
| HAL                 | GPIO        |
| HAL                 | PWR         |
| HAL                 | RCC         |
| HAL                 | ADC         |
| HAL                 | I2C         |
| HAL                 | TIM         |
| HAL                 | UART        |
| HAL                 | SPI         |

**Important:** Use the Classic STM32 HAL implementation, not the newer HAL2 APIs.

The firmware uses the following STM32 HAL modules:

* `stm32f4xx_hal_gpio.h` – GPIO configuration and control.
* `stm32f4xx_hal_rcc.h` – Clock configuration.
* `stm32f4xx_hal_uart.h` – USART2 communication with ESP32.
* `stm32f4xx_hal_spi.h` – SPI communication with RC522 RFID.
* `stm32f4xx_hal_i2c.h` – I2C communication with OLED.
* `stm32f4xx_hal_tim.h` – Timer and PWM control for servo.
* `stm32f4xx_hal_adc.h` – ADC HAL module dependency.
* `stm32f4xx_hal_pwr.h` – Power control support.

Ensure that the following are configured:

* Device: STM32F401CCUx
* CPU: Cortex-M4
* FPU: Single-precision floating point
* Compiler: ARM Compiler 6 (or a compatible Keil compiler)
* System clock: 16 MHz HSI
* HAL library: STM32CubeF4 Classic HAL

Add the STM32 HAL driver and CMSIS include directories to the compiler include paths.

The firmware uses the STM32 HAL API through:

```c
#include "stm32f4xx_hal.h"
```
After configuring the project, add `stmcode.c`, build the project, and flash the generated firmware to the STM32F401CCU6 board.

## 7. How to Run the ESP32 Firmware

### Step 1: Install Arduino IDE

Download and install Arduino IDE.

### Step 2: Install ESP32 Board Support

1. Open Arduino IDE.
2. Open Boards Manager.
3. Search for **ESP32**.
4. Install the package published by Espressif Systems.
5. Select **ESP32 Dev Module** from Tools → Board.

### Step 3: Configure Wi-Fi

Open `espcode.ino`.

Update the following settings with your own Wi-Fi credentials:

```cpp
const char *WIFI_SSID = "YOUR_WIFI_NAME";
const char *WIFI_PASS = "YOUR_WIFI_PASSWORD";
```

The ESP32 first attempts to connect to the configured Wi-Fi network.

If the connection fails within 15 seconds, it creates a fallback Wi-Fi hotspot:

```cpp
const char *AP_SSID = "SmartParking";
const char *AP_PASS = "12345678";
```

### Step 4: Upload the Code

1. Connect the ESP32 to your computer using USB.
2. Select the correct ESP32 board.
3. Select the appropriate COM port.
4. Open `espcode.ino`.
5. Click Verify to compile.
6. Click Upload to flash the firmware.

### Step 5: Access the Booking Webpage

1. Open Serial Monitor.
2. Set the baud rate to **115200**.
3. Wait for the ESP32 to connect to Wi-Fi.
4. Read the IP address displayed in Serial Monitor.
5. Connect your phone or laptop to the same Wi-Fi network.
6. Open a browser and enter:

```text
http://ESP32_IP_ADDRESS
```

If the ESP32 starts its fallback hotspot:

1. Connect to the `SmartParking` Wi-Fi network.
2. Enter the configured hotspot password.
3. Open the IP address displayed in Serial Monitor (normally `192.168.4.1`).

The Smart Parking webpage displays the three parking slots and allows users to book or cancel reservations using their vehicle registration number.

## 8. System Operation

1. The ESP32 hosts the online parking reservation webpage.
2. Users select an available slot and enter their vehicle number.
3. The ESP32 sends booking information to the STM32 through UART.
4. The STM32 continuously monitors the three parking slot IR sensors.
5. When a vehicle reaches the entrance, the entrance IR sensor detects it.
6. If an unbooked free slot is available, the system assigns it automatically.
7. If only a booked slot is available, the user is asked to tap an RFID card.
8. After successful authentication, the servo gate opens.
9. The vehicle enters, and the corresponding slot IR sensor detects occupancy.
10. The STM32 updates the LEDs and OLED display.
11. Occupancy information is sent back to the ESP32.
12. The ESP32 updates the webpage with the latest parking status.

## 9. UART Communication Protocol

The two microcontrollers exchange information using UART.

**ESP32 → STM32: Booking Information**

```text
B,b1,b2,b3
```

Indicates whether each parking slot is reserved.

**STM32 → ESP32: Occupancy Information**

```text
S,o1,o2,o3
```

Indicates whether each parking slot is occupied.

Here:

* `1` represents booked or occupied.
* `0` represents not booked or not occupied.

Communication settings: 115200 baud, 8N1.

## 10. Important Configuration Notes

* The current implementation supports three parking slots.
* One IR sensor is used for entrance detection.
* Three IR sensors monitor individual parking slots.
* One servo motor controls the entrance gate.
* RFID communication uses SPI.
* OLED communication uses I2C.
* ESP32 and STM32 communicate through USART2.
* Bookings expire after 30 minutes if the vehicle does not park.
* The STM32 firmware currently uses `ACCEPT_ANY_CARD = 1`, meaning any readable RFID card is accepted. Set it to `0` to enable UID-based authorization using the configured authorized UID list.
* The ESP32 fallback hotspot credentials can be changed in `espcode.ino`.

## 11. Troubleshooting

| Problem                            | Suggested Solution                                                                            |
| ---------------------------------- | --------------------------------------------------------------------------------------------- |
| ESP32 does not connect to Wi-Fi    | Verify the SSID and password. Ensure the network supports 2.4 GHz.                            |
| Booking webpage does not open      | Check the IP address in Serial Monitor and ensure the phone/laptop is on the correct network. |
| STM32 and ESP32 do not communicate | Check TX/RX cross-connections, common GND, and baud rate.                                     |
| RFID reader does not detect cards  | Check the 3.3V supply and SPI connections.                                                    |
| OLED does not display information  | Check I2C connections, power, and the OLED address (0x3C).                                    |
| Servo does not operate correctly   | Check the PWM signal, external power supply, and common ground.                               |
| Slot status is incorrect           | Check the IR sensor connections and active logic level.                                       |

## 12. Safety and Security

* Use a regulated power supply suitable for the connected components.
* Do not power the servo directly from an STM32 GPIO pin.
* Ensure the STM32 and ESP32 share a common ground.
* Use 3.3V logic for the RC522 module.
* Do not publish personal Wi-Fi credentials in the source code. Replace them with your own local configuration before running the project.

## 13. Expected Output

* Real-time parking slot availability displayed on the OLED.
* Online slot booking through a Wi-Fi webpage.
* Automatic vehicle detection at the entrance.
* RFID-based authentication for booked parking slots.
* Automatic gate operation using a servo motor.
* Visual and audio indications for parking status.
* Automatic parking lights based on ambient light.
* Live synchronization of slot occupancy between STM32 and ESP32.

---


