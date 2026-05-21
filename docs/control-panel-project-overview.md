# Stirling Single Electric Control Panel — Project Overview / Architecture Context

## 1. Project context

This project is part of the 5-inch gauge **Stirling Single** model locomotive build, based on A. J. Reeves plans. The locomotive is being considered as **electric powered rather than steam powered**, primarily because the small firebox may make reliable steaming difficult.

The current drive concept uses a **24 V scooter-style DC geared motor**, specifically a MY1018Z-style 250 W 24 V DC gear motor with an output speed around **380 rpm**. The locomotive drive is expected to require further mechanical reduction, with earlier discussions considering approximately **1.5:1 to 2:1** for usable running speed, though other mechanical arrangements such as bevel gears, worm gearboxes, chain drive, and right-angle drive components have also been explored.

The locomotive and passenger-car arrangement affects the control architecture:

- The **motor, gearbox, wheel/axle drive, and speed pickup** are expected to be on the locomotive or tender side.
- The **driver/operator will sit on a passenger car or rear driving position**, so the control panel may be physically separated from the gearbox and motor by around **1 m or more**.
- Wiring looms and detachable connectors will be needed so the locomotive/tender/passenger car can be separated.

## 2. Purpose of the control panel project

The control panel is intended to provide a period-sympathetic driver interface for the electric locomotive while keeping the traction motor control electrically simple and robust.

The control panel will include:

- Throttle control input for the dedicated PWM motor controller.
- Forward/reverse control for motor direction, likely handled by the PWM controller or associated power switching, not by ESP32 firmware.
- Speed indication using a Hall-effect sensor on the gearbox, wheel, axle, or other rotating component. 4 magnets will used for greater speed resolution and to trigger 4 audio steam 'chuffs' per wheel revolution
- Traction current indication using a Hall-effect current sensor such as an ACS758-style module.
- Gauge displays, using small x27 stepper-motor analogue gauges or digital display elements.
- The ESP32 will display odometer data. Data therefore needs to be written to the invite microSD card to enable this. Odometer will display on a small TFT display.
- Audio effects such as whistle, bell, or period-style sound cues. A 'chuff' will sound on each trigger of the speed hall effect sensor.
- Lighting/switch controls, such as headlights or panel illumination.
- Optional logging or diagnostic output during development.

A key architectural constraint is that the **ESP32 system will not directly control the traction PWM module**. The PWM controller remains dedicated and independent. The ESP32 control panel system is for sensing, display, audio, status, and auxiliary functions.

## 3. High-level architecture

```text
                        ┌────────────────────────────┐
                        │        Control Panel        │
                        │                            │
                        │  ┌──────────────────────┐  │
                        │  │        ESP32          │  │
                        │  │  sensing / display    │  │
                        │  │  audio / logic        │  │
                        │  └──────────┬───────────┘  │
                        │             │              │
                        │  Gauges     │    Audio     │
                        │  Display    │    Lights    │
                        │  Buttons    │    Switches  │
                        └─────────────┼──────────────┘
                                      │
                     low-voltage sensor / auxiliary loom
                                      │
        ┌─────────────────────────────┴─────────────────────────────┐
        │                                                           │
┌───────▼────────┐                                      ┌───────────▼──────────┐
│ Speed Hall     │                                      │ Current Sensor        │
│ sensor         │                                      │ ACS758-style module   │
│ gearbox/axle   │                                      │ in battery/motor feed │
└────────────────┘                                      └──────────────────────┘

Separate traction path:

Battery ─ fuse/isolation ─ dedicated PWM controller ─ motor ─ gearbox/drive
                         ▲
                         │
             throttle / direction controls
             wired to PWM controller only

No direct ESP32-to-PWM control connection.
```

## 4. Separation of responsibilities

### 4.1 Dedicated PWM motor controller

The PWM motor controller is responsible for:

- Motor speed control.
- Handling the motor current path.
- Any built-in throttle interpretation.
- Any built-in forward/reverse function, if supported.
- Motor protection features provided by the controller.

The PWM controller should be treated as a traction power component, not as a peripheral controlled by the ESP32.

### 4.2 ESP32 control panel system

The ESP32 is responsible for:

- Reading speed pulses from the Hall sensor.
- Calculating speed from pulse frequency.
- Reading traction current from a Hall-effect current sensor.
- Driving gauges or displays.
- Handling zero/calibration/setup buttons if needed.
- Driving audio effects for whistle/bell/steam chuffs.
- Driving auxiliary indicators and possibly lighting relays/MOSFETs.
- Serial/USB debug logging during development.

The ESP32 must not be required for basic motor operation. A failure of the ESP32 should ideally result in loss of displays/audio only, not a runaway or disabled traction system.

## 5. Proposed staged development approach

### Stage 1 — ESP32 development base

Establish a basic ESP32 project using VSCode and PlatformIO or Arduino framework.

Goals:

- Confirm USB programming from Mac/PC.
- Establish serial debug output.
- Add a simple project structure.
- Blink/test GPIO pins.
- Add basic configuration constants.

Suggested board family:

- ESP32 board is ESP32-S3 Development Boards with Expansion Adapter Kit 2.4G Wifi BT Module ESP32-S3-1 N8R2 N16R8 44Pin Type.

### Stage 2 — Speed sensing

Add the Hall-effect gearbox or axle speed sensor.

Design notes:

- The Hall sensor will be mounted at the gearbox drive component.
- 4 magnets will provide 4 pulses per revolution.
- Because the sensor may be around 1 m from the panel, use a robust wiring approach.
- Keep the sensor cable away from motor power wiring where possible.
- If it must share a loom, use twisted pairs, shielding, filtering, and proper grounding.
- Use a connector at the gearbox end so the loom can be disconnected.

Typical Hall sensor wiring:

- VCC
- GND
- Signal

Some Hall modules include onboard pull-up resistors and indicators. Bare Hall sensors may require an external pull-up resistor to 3.3 V or 5 V depending on the sensor type.

ESP32 input should be protected and debounced/filtered in software. Interrupt-based pulse counting is appropriate.

### Stage 3 — Speed calculation and display

Calculate locomotive speed from pulse frequency.

Inputs required:

- Wheel diameter, currently discussed around **8 inch driving wheels**.
- Pulses per wheel revolution, or pulses per gearbox shaft revolution plus gear ratio.
- Total ratio between measured shaft and wheel.

Speed calculation should be parameterised so the sensor can move from motor, gearbox, axle, or wheel without rewriting logic.

Conceptual formula:

```text
wheel_rpm = measured_pulse_frequency_hz * 60 / pulses_per_wheel_revolution
speed_m_per_s = wheel_rpm * wheel_circumference_m / 60
speed_km_h = speed_m_per_s * 3.6
```

Display options:

- Stepper-driven analogue speed gauge.
- OLED/TFT digital odometer display.
- Combination of analogue gauge plus small diagnostic screen.

### Stage 4 — Traction current sensing

Using an ACS758-style Hall current sensor module.

Design notes:

- The current sensor is placed around/in series with the traction current path, commonly in the battery positive feed or motor feed depending on what is to be measured.
- A bidirectional sensor is useful if current direction may reverse or if regenerative/braking current is ever relevant.
- If placed in the battery positive feed, the sensor measures battery current direction, not necessarily motor polarity unless the whole motor current path reverses through the sensor.
- If motor reversal is handled downstream of the sensor, interpret readings carefully.
- Ensure the sensor current rating is comfortably above expected stall/startup current.

The ESP32 reads the sensor analogue output using an ADC input, preferably with filtering and calibration.

Current display options:

- Analogue-style traction current gauge.
- Digital amps display on diagnostic TFT
.

### Stage 5 — Gauges

The control panel may include two gauges:

- Speed gauge.
- Traction current gauge.

These will be made using x27 stepper motors.

Design preference:

- Keep gauge control non-critical.
- On boot, home/calibrate gauges if required.
- Smooth readings to avoid jitter.
- Provide a diagnostic serial output so displayed value can be compared to calculated value.

### Stage 6 — Audio

Audio features may include:

- Whistle.
- Bell.
- Optional start/run motor or steam-style ambient effects.

Possible architecture:

- ESP32 triggers sounds from internal flash or a microSD/TF card module.
- AliExpress New High Quality MAX98357 MAX98357A I2S 3W Class D Amplifier Breakout Interface I2S DAC Decoder For Audio drives a speaker. I have 2 speakers if this is a stereo amp.
- Dedicated buttons or levers on the panel trigger whistle/bell.

Notes:

- MicroSD and TF modules refer to the same style of small flash card format in this context.
- Audio and motor power should be electrically quiet and decoupled to avoid noise through the speaker.
- Speaker/amplifier power should be fused and filtered separately from ESP32 logic where practical.

### Stage 7 — Physical controls and panel layout

The panel concept has previously included:

- A throttle control, preferably period-sympathetic.
- Forward/reverse switch or lever.
- Bell and whistle controls.
- Speed gauge.
- Traction current gauge.
- Headlight switches.
- Brushed brass or similar period-style finish.
- Dark or muted trim details.

Throttle options discussed:

- Rotary speed control styled as a period control.
- Lever-style throttle.
- Miniature Johnson-bar-style control in a gate, if it can be made mechanically practical.

Important architectural point:

- The throttle should feed the PWM controller directly, using the resistance/voltage input expected by the controller.
- The ESP32 may optionally read a copy of throttle position for display/logging, but this must not interfere with the PWM controller input.
- If copying the throttle signal, use buffering or an independent dual-gang potentiometer rather than loading the PWM controller signal.

### Stage 8 — Enclosure and wiring loom

Because the control panel will be used on a ride-on miniature railway locomotive, mechanical and wiring robustness matter.

Recommended practices:

- Use locking or keyed connectors for detachable looms.
- Separate traction wiring from sensor/audio/control wiring as much as possible.
- Use strain relief at all connectors.
- Use ferrules or crimp terminals rather than loose stranded wire in screw terminals.
- Fuse battery feeds appropriately.
- Add a master isolation switch and emergency stop outside the ESP32 system.
- Avoid routing Hall sensor signal wires alongside motor power wires unless shielded/twisted and filtered.

Connector ideas:

- JST-style connectors for internal low-current modules.
- GX aviation connectors, M8/M12 sensor connectors, or automotive sealed connectors for external/disconnectable loom sections.
- Anderson-style connectors or appropriately rated DC connectors for traction power, if needed.

## 6. Electrical domains

The design should keep these domains conceptually separate:

| Domain | Purpose | Notes |
|---|---|---|
| Traction power | Battery, fuse, PWM controller, motor | High current, noisy, safety-critical |
| PWM control wiring | Throttle, direction, enable, brake if present | Goes directly to PWM module |
| ESP32 logic | Sensors, display, buttons, audio triggers | 3.3 V logic; not safety-critical |
| Auxiliary power | 5 V regulator, display power, audio amp | Needs filtering and adequate current |
| Sensor wiring | Hall speed, current sensor output | Protect from motor noise |
| Audio | Sound module/ESP32 audio, amplifier, speaker | Keep supply clean to avoid noise |

## 7. Safety and failure-mode assumptions

The ESP32 must be designed as an instrumentation and auxiliary controller, not as the traction safety controller.

Preferred failure behaviour:

- If ESP32 crashes: motor control still behaves normally through the PWM controller.
- If speed sensor fails: speed display reads zero/error but motor control remains independent.
- If current sensor fails: current display reads zero/error but motor control remains independent.
- If audio fails: no effect on traction.
- If display fails: no effect on traction.

Safety-critical controls should be hard-wired:

- Battery isolation.
- Fuse/circuit breaker.
- Emergency stop.
- PWM controller enable/disable if used.
- Direction control if required by the PWM controller.

## 8. Initial firmware modules

A clean initial firmware structure could be:

```text
/src
  main.cpp
  config.h
  speed_sensor.h / speed_sensor.cpp
  current_sensor.h / current_sensor.cpp
  gauges.h / gauges.cpp
  display.h / display.cpp
  audio.h / audio.cpp
  controls.h / controls.cpp
  diagnostics.h / diagnostics.cpp
```

Suggested responsibilities:

| Module | Responsibility |
|---|---|
| `speed_sensor` | Interrupt pulse counting, frequency calculation, speed conversion |
| `current_sensor` | ADC reading, calibration, filtering, amps conversion |
| `gauges` | Stepper gauge positioning and smoothing |
| `display` | TFT/OLED/7-segment output |
| `audio` | Bell/whistle triggering |
| `controls` | Buttons and auxiliary switches |
| `diagnostics` | Serial output and debug status |
| `config` | Wheel diameter, pulses/rev, calibration constants, pin assignments |

## 9. Key configuration values to capture

These should be explicit constants or settings:

```text
wheel_diameter_mm
speed_sensor_location
pulses_per_sensor_rev
sensor_revs_per_wheel_rev
speed_display_units
current_sensor_model
current_sensor_zero_adc
current_sensor_mv_per_amp
traction_current_warning_amps
traction_current_max_display_amps
gauge_speed_full_scale_kmh
gauge_current_full_scale_amps
```

## 10. Development test plan

### Bench tests

- Confirm ESP32 programming via USB.
- Confirm serial monitor output.
- Simulate speed pulses with a signal generator or manual magnet movement.
- Confirm Hall sensor detection at low and high speed.
- Confirm current sensor ADC readings with no current.
- Confirm current sensor readings using a known DC load.
- Confirm display/gauge outputs.
- Confirm audio triggers.

### Rolling chassis tests

- Test speed sensor on lifted wheels or test stand.
- Compare calculated speed against measured wheel rpm.
- Confirm current reading under light load.
- Check for signal noise when motor is running.
- Confirm the ESP32 does not reset under motor load.
- Confirm audio/display remain stable during acceleration.

### Ride-on tests

- Validate visibility of display in daylight.
- Validate control reach and ergonomics.
- Confirm emergency stop and isolation are accessible.
- Confirm detachable loom works reliably.
- Confirm no cable snagging between locomotive, tender, and passenger car.

## 11. Open design decisions

- Exact ESP32 board.
- Display type: analogue gauges, digital display, or both.
- Exact audio method: ESP32 DAC/I2S, DFPlayer-style module, or other sound board.
- Current sensor current range and bidirectional/unidirectional choice.
- Final location of speed Hall sensor.
- Connector family for removable gearbox/control loom.
- Whether throttle position should be monitored by ESP32 without affecting PWM controller.
- Whether panel lighting/headlights are switched directly or via ESP32-controlled MOSFETs/relays.
- Final enclosure size and period styling.

## 12. Design principle summary

The control panel should be designed as a **robust, modular instrumentation and auxiliary control system** around an ESP32, while the traction motor remains controlled by a **separate dedicated PWM controller**.

The guiding principle is:

> The ESP32 may observe, display, sound, and assist — but it should not be required for safe basic traction control.

