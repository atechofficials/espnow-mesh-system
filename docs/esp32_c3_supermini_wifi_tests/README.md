# ESP32-C3 Super Mini WiFi Tests And Hardware Mod Guide

This document summarizes the WiFi receive and WiFi transmit tests performed on three ESP32-C3 Super Mini development boards while investigating why some ESP32-C3 based nodes were not being detected reliably by the ESP32-S3 gateway.

Every RSSI value in this report is the average of three readings, which is why several values are fractional.

---

## Why These Tests Were Performed

The main trigger for these tests was a practical mesh-system issue:

- some ESP32-C3 Super Mini based nodes would enter pairing mode correctly
- their firmware appeared to send beacons correctly
- but the ESP32-S3 gateway would not reliably detect them

That made it necessary to separate:

- firmware and ESP-NOW logic issues
- power-delivery issues
- RF / antenna issues
- board-quality differences between individual ESP32-C3 Super Mini boards

---

## Test Setup

- Three ESP32-C3 Super Mini boards were tested
- Device-to-device distance was approximately `49 inches`
- `DDOS` (home router) was used as a stable reference network
- `ESP32_Test` was the SSID broadcast by an ESP32-WROOM-32E development board during receive tests
- `ESP32C3_Test` was the SSID broadcast by the ESP32-C3 Super Mini board under test during transmit tests
- An ESP32-S3-WROOM-1 DevKitC board was used as a reference receiver alongside the main receiver under test
- Tests `10` to `12` used `WiFi.setTxPower(WIFI_POWER_8_5dBm);`
- Tests `7` to `9` used no WiFi transmit power limit and no external `3.3V` supply

Important software note:
- the `8.5 dBm` transmit-power cap is only meaningful when it is applied after WiFi startup, not during early boot

---

## Board Configurations Used In The Tests

### Test Configuration A: Boards as they existed during Tests 1 to 6

These were not all stock boards.

| Board | State used in Tests `1` to `6` |
|------|---------------------------------|
| `Board-1` | Stock ceramic antenna, stock onboard LDO |
| `Board-2` | Stock antenna already replaced with a `31 mm` quarter-wave style steel-wire antenna (`0.82 mm` wire diameter), stock onboard LDO |
| `Board-3` | Stock antenna already replaced with a helix / spring-coil brass-wire antenna (`0.83 mm` wire diameter), stock onboard LDO |

### Test Configuration B: Boards after the later LDO and antenna rework used in Tests 7 to 12

| Board | State used in Tests `7` to `12` |
|------|----------------------------------|
| `Board-1` | Stock ceramic antenna removed and replaced with an external PCB antenna connected by shielded wire |
| `Board-2` | Onboard LDO replaced with `MIC5219-3.3YM5`; quarter-wave antenna removed and replaced with a helix / spring-coil brass-wire antenna |
| `Board-3` | Onboard LDO replaced with `MIC5219-3.3YM5`; helix / spring-coil brass-wire antenna retained |

---

## Test Configuration A Results: Tests 1 To 6

### Receive Tests (`Test-1` to `Test-3`)

| Test | Board under test | DUT RSSI `DDOS` | DUT RSSI `ESP32_Test` | DUT networks | Ref S3 RSSI `DDOS` | Ref S3 RSSI `ESP32_Test` | Ref S3 networks |
|------|------------------|-----------------|------------------------|--------------|--------------------|---------------------------|-----------------|
| `Test-1` | `Board-1` stock antenna | `-47.00 dBm` | `-60.67 dBm` | `5` | `-29.00 dBm` | `-48.33 dBm` | `6` |
| `Test-2` | `Board-2` quarter-wave steel antenna | `-31.33 dBm` | `-53.33 dBm` | `6` | `-29.00 dBm` | `-50.00 dBm` | `7` |
| `Test-3` | `Board-3` helix brass antenna | `-36.66 dBm` | `-55.00 dBm` | `7` | `-31.33 dBm` | `-50.66 dBm` | `7` |

### Transmit Tests On Onboard Supply (`Test-1` to `Test-3`)

| Test | AP board under test | WROOM-32E RSSI `DDOS` | WROOM-32E RSSI `ESP32C3_Test` | WROOM-32E networks | Ref S3 RSSI `DDOS` | Ref S3 RSSI `ESP32C3_Test` | Ref S3 networks |
|------|---------------------|-----------------------|--------------------------------|--------------------|--------------------|-----------------------------|-----------------|
| `Test-1` | `Board-1` stock antenna | `-29.33 dBm` | `-69.00 dBm` | `7` | `-29.67 dBm` | `-63.67 dBm` | `7` |
| `Test-2` | `Board-2` quarter-wave steel antenna | `-36.33 dBm` | `Not detected` | `7` | `-29.33 dBm` | `Not detected` | `7` |
| `Test-3` | `Board-3` helix brass antenna | `-32.33 dBm` | `Not detected` | `6` | `-32.33 dBm` | `Not detected` | `6` |

### Transmit Tests On External `3.3V` Supply (`Test-4` to `Test-6`)

| Test | AP board under test | WROOM-32E RSSI `DDOS` | WROOM-32E RSSI `ESP32C3_Test` | WROOM-32E networks | Ref S3 RSSI `DDOS` | Ref S3 RSSI `ESP32C3_Test` | Ref S3 networks |
|------|---------------------|-----------------------|--------------------------------|--------------------|--------------------|-----------------------------|-----------------|
| `Test-4` | `Board-1` stock antenna + external `3.3V` | `-35.33 dBm` | `-72.33 dBm` | `8` | `-30.00 dBm` | `-64.00 dBm` | `7` |
| `Test-5` | `Board-2` quarter-wave steel antenna + external `3.3V` | `-35.67 dBm` | `Not detected` | `7` | `-27.33 dBm` | `Not detected` | `7` |
| `Test-6` | `Board-3` helix brass antenna + external `3.3V` | `-32.33 dBm` | `-55.67 dBm` | `7` | `-35.00 dBm` | `-54.00 dBm` | `8` |

### What Tests 1 To 6 Showed

- `Board-1` was weak but still functional in both receive and transmit tests
- `Board-2` showed strong receive performance but effectively failed transmit tests even with external `3.3V`
- `Board-3` only became a stable transmitter when powered from a stronger external `3.3V` source

Those results pointed to two different issues:

- `Board-2` looked like an RF / transmit-path problem
- `Board-3` looked like a power-delivery / onboard-LDO problem

---

## Test Configuration B Results: Tests 7 To 12

### Receive Tests After The Rework (`Test-7` to `Test-9`)

| Test | Board under test | DUT RSSI `DDOS` | DUT RSSI `ESP32_Test` | DUT networks | Ref S3 RSSI `DDOS` | Ref S3 RSSI `ESP32_Test` | Ref S3 networks |
|------|------------------|-----------------|------------------------|--------------|--------------------|---------------------------|-----------------|
| `Test-7` | `Board-1` external PCB antenna | `-29.33 dBm` | `-44.00 dBm` | `7` | `-31.00 dBm` | `-45.33 dBm` | `8` |
| `Test-8` | `Board-2` helix brass antenna + `MIC5219` | `-38.00 dBm` | `-53.00 dBm` | `5` | `-32.00 dBm` | `-46.33 dBm` | `7` |
| `Test-9` | `Board-3` helix brass antenna + `MIC5219` | `-38.00 dBm` | `-50.67 dBm` | `5` | `-32.00 dBm` | `-46.00 dBm` | `8` |

### Transmit Tests After The Rework At Default Power (`Test-7` to `Test-9`)

| Test | AP board under test | WROOM-32E RSSI `DDOS` | WROOM-32E RSSI `ESP32C3_Test` | WROOM-32E networks | Ref S3 RSSI `DDOS` | Ref S3 RSSI `ESP32C3_Test` | Ref S3 networks |
|------|---------------------|-----------------------|--------------------------------|--------------------|--------------------|-----------------------------|-----------------|
| `Test-7` | `Board-1` external PCB antenna | `-32.00 dBm` | `Not detected` | `6` | `-40.00 dBm` | `Not detected` | `6` |
| `Test-8` | `Board-2` helix brass antenna + `MIC5219` | `-32.67 dBm` | `-49.67 dBm` | `7` | `-35.67 dBm` | `-47.33 dBm` | `7` |
| `Test-9` | `Board-3` helix brass antenna + `MIC5219` | `-35.00 dBm` | `Not detected` | `6` | `-36.33 dBm` | `Not detected` | `6` |

### Transmit Tests After The Rework With `8.5 dBm` Cap (`Test-10` to `Test-12`)

| Test | AP board under test | WROOM-32E RSSI `DDOS` | WROOM-32E RSSI `ESP32C3_Test` | WROOM-32E networks | Ref S3 RSSI `DDOS` | Ref S3 RSSI `ESP32C3_Test` | Ref S3 networks |
|------|---------------------|-----------------------|--------------------------------|--------------------|--------------------|-----------------------------|-----------------|
| `Test-10` | `Board-1` external PCB antenna + `8.5 dBm` cap | `-34.00 dBm` | `-52.00 dBm` | `7` | `-31.33 dBm` | `-47.33 dBm` | `8` |
| `Test-11` | `Board-2` helix brass antenna + `MIC5219` + `8.5 dBm` cap | `-38.67 dBm` | `-69.33 dBm` | `7` | `-34.33 dBm` | `-51.33 dBm` | `7` |
| `Test-12` | `Board-3` helix brass antenna + `MIC5219` + `8.5 dBm` cap | `-37.33 dBm` | `-58.33 dBm` | `7` | `-31.67 dBm` | `-50.33 dBm` | `7` |

### What Tests 7 To 12 Showed

- `Board-2` improved dramatically after the LDO replacement and antenna change
- `Board-3` improved after the LDO replacement but still benefited from the `8.5 dBm` cap
- `Board-1` gained excellent receive performance with the external PCB antenna, but its transmit side was still better behaved once the `8.5 dBm` cap was used

---

## Board Mod Guide

These modifications are meant for prototype and experimental use. They are not recommended for users who are not comfortable with fine-pitch SMD rework.

### Before You Start

1. Test the board first in stock form so you have a baseline.
2. Photograph the board before removing any parts.
3. Use magnification while working around the RF section and the LDO.
4. Keep airflow moderate so you do not blow away tiny RF parts.
5. After every rework stage, check for shorts before powering the board.

### LDO Replacement Guide

This was the rework used on `Board-2` and `Board-3`.

1. Identify the stock `3.3V` LDO on the ESP32-C3 Super Mini board.
2. Remove the stock LDO carefully with hot air and flux.
3. Clean the pads fully and inspect for lifted pads or solder bridges.
4. Install `MIC5219-3.3YM5` in the correct orientation.
5. Recheck continuity between:
   - `VIN` and `GND`
   - `VOUT` and `GND`
   - the LDO output rail and the ESP32-C3 `3.3V` rail
6. Power the board and confirm the output rail is stable at `3.3V` before running WiFi tests.
7. Re-test WiFi transmit behavior after the LDO swap before making further RF changes if you want to isolate the effect of the regulator alone.

### Antenna Modification Guide

These are the antenna approaches used during the tests.

#### Option A: External PCB antenna via shielded wire

This was the approach used on `Board-1` for the later tests.

1. Remove the stock ceramic antenna cleanly.
2. Expose and inspect the antenna feed area carefully.
3. Use a short shielded feed wire.
4. Connect the feed conductor to the RF feed point.
5. Connect the shield side according to the board's ground side around the antenna feed area.
6. Keep the cable as short and mechanically stable as possible.
7. Mount the external PCB antenna away from the crystal oscillator, regulator, and the rest of the dense board area.
8. Re-test both WiFi receive and WiFi transmit behavior after the antenna change.

#### Option B: Helix / spring-coil wire antenna

This was the approach used on `Board-3`, and later also on `Board-2`.

1. Remove the stock ceramic antenna.
2. Prepare the helix / spring-coil antenna from brass wire.
3. Solder the helix antenna neatly at the antenna feed point.
4. Keep the antenna clear of nearby metal, bench fixtures, and USB cable strain while testing.
5. Re-test receive and transmit behavior after the swap.

#### Option C: Quarter-wave straight wire antenna

This was the earlier approach used on `Board-2`.

1. Remove the stock ceramic antenna.
2. Prepare a straight quarter-wave wire antenna.
3. Solder it carefully to the antenna feed point.
4. Re-test receive and transmit behavior.

In these tests, the quarter-wave steel-wire implementation improved receive performance but did not solve the transmit problem on `Board-2`, so it was later replaced by the helix antenna.

### Recommended Rework Sequence

If someone still wants to use ESP32-C3 Super Mini boards for prototype mesh nodes, this order is more sensible than changing everything at once:

1. Test the board in stock form.
2. If WiFi transmit looks unstable, replace the onboard LDO first.
3. Re-test the board.
4. If RF performance is still weak, replace the stock ceramic antenna with a more suitable external solution.
5. Re-test again.
6. If the board is still marginal, try the `8.5 dBm` TX cap in firmware, but only after WiFi startup has completed.

---

## Findings

### 1. Some ESP32-C3 Super Mini boards had real power-delivery problems

- `Board-3` clearly improved once its onboard LDO was replaced
- `Board-2` also improved after the same regulator upgrade

That supports the conclusion that some of these boards ship with weak or unstable onboard LDO implementations.

### 2. The stock ceramic antenna arrangement is a weak point

- `Board-1` improved dramatically on receive after moving to an external PCB antenna
- the stock antenna layout appears compromised by the very small board and by nearby parts

### 3. The `8.5 dBm` cap helps some boards, but not all in the same way

- `Board-1` became a more reliable transmitter with the cap
- `Board-3` also benefited from the cap
- `Board-2` performed best after hardware rework even without the cap

So the TX-power cap is best treated as a stabilization tool for marginal boards, not as the primary fix.

---

## Practical Ranking After The Rework

| Board | Receive result | Transmit result | Practical verdict |
|------|----------------|-----------------|-------------------|
| `Board-1` | Excellent receive with the external PCB antenna | Needs `8.5 dBm` cap to transmit reliably | Usable for experiments, but the antenna choice is still more experimental than the helix boards |
| `Board-2` | Good receive | Best transmit performance after `MIC5219` + helix changes | Best all-round board after rework |
| `Board-3` | Good receive | Still marginal at full power, but stable with `8.5 dBm` cap | Usable after rework, but still not as strong as `Board-2` |

---

## Final Conclusion

These tests support the following conclusions:

- some ESP32-C3 Super Mini boards ship with weak or unstable onboard LDOs
- the stock ceramic antenna arrangement is a weak point of the design
- moving to a larger antenna and, in some cases, lowering WiFi transmit power can make the boards much more usable
- the boards can be made serviceable for prototyping after rework, but they are still not ideal candidates for a reliable consumer product

For production-oriented hardware, a certified ESP32 module remains the safer choice. For prototype work, modified Super Mini boards can still be usable when:

- the board has a stable LDO
- the antenna implementation has been corrected
- the WiFi transmit-power cap is applied only where the hardware actually needs it

---

## Learning Resources / References

- [ESP32-C3 SuperMini Antenna Modification Guide](https://peterneufeld.wordpress.com/2025/03/04/esp32-c3-supermini-antenna-modification/)
