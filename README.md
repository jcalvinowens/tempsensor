
ESP32-C3 Temperature Sensor
==========================

This repository contains the hardware design, firmware, and HTTP backend for a
small battery powered WiFi temperature and humidity sensor.

![](https://static.wbinvd.org/img/tempsensor/front.jpg)

The firmware is written in C, using ESP-IDF, which is based on FreeRTOS.

The backend is written in Python, using the native HTTPS and sqlite3 support. It
supports an unlimited number of sensors, and uses gnuplot to render graphs and
serve them over HTTPS:

![](https://static.wbinvd.org/img/tempsensor/graph.jpg)

The system supports over-the-air firmware updates, using two partitions with a
fallback to the old firmware if the first POST is unsuccessful.

TL;DR:

* [Gerber](prod/v020-GERBER.zip), [BOM](prod/v020-BOM.csv), and [CPL](prod/v020-CPL.csv)
* [Schematic](schematic.pdf), [PCB Layout](layout.pdf)
* [Firmware](main/main.c)
* [Backend](backend/daemon.py)

Design
------

The board is designed around the ESP32-C3, a low power single core RISCV SoC
with wifi and bluetooth support. The sensor is an unnecessarily fancy part with
0.01degC/0.01%RH resolution, just for fun.

The board has two 3.3V power supplies: an LDO and a switcher. The switcher is
much more efficient than the LDO, but has a high quiescent current draw (~1mA),
so it is enabled via a GPIO only when the wifi is turned on.

![](https://static.wbinvd.org/img/tempsensor/frontback.jpg)

I more or less followed the [ESP32-C3 design guidelines](https://www.espressif.com/sites/default/files/documentation/esp32-c3_hardware_design_guidelines_en.pdf)
when laying out the board, with the exception of RF impedence matching. The RF
trace and my antennas are all 50 Ohm, which, with the 35 Ohm LNA output, results
in a VSWR of ~1.4:1. The loss (versus a matched line) is less than an S-unit, so
it wasn't worth the money.

I used small laptop syle wifi antennas, which are easy to find in bulk for cheap
nowadays if they aren't compatible with 5GHz. Because they log signal strength,
the sensors will be useful for future experimentation with PCB antenna designs.

![](https://static.wbinvd.org/img/tempsensor/longfrontback.jpg)

To avoid the "thundering herd" problem, each sensor has a unique "send delay"
assigned by the backend, so the wifi connections and POSTs can be splayed while
still performing the actual measurement at the exact same time.

The sensors also support queueing measurments in non-volatile storage, which can
dramatically extend the battery lifetime (at the cost of delayed data).

The embedded flash is rated for 100K erase cycles. The default config allocates
four 4K sectors for NVS, so we get 400K 4K cycles total. Each 4K sector can hold
126 64-bit entries, so we can queue a total of 50,400,000 measurements before
worrying about wearing out the flash. At one measurement per minute, that would
take 95 years! I think we'll be okay...

![](https://static.wbinvd.org/img/tempsensor/prog.jpg)

The board is programmed via pogo pads. I spaced the pads to match a pogo clip I
ordered online, but it turned out the spacing was different than advertised...
so I had to kludge something together myself to match the boards I already had.

In retrospect, I could've made my life easier by avoiding routing in-between the
pads so they could be bigger. Even as is, holding the pogo pins on with my
thumb wasn't a problem at all.

Datasheets
----------

* MCU [ESP32-C3FH4](https://www.espressif.com/sites/default/files/documentation/esp32-c3_datasheet_en.pdf)
* Sensor [HDC1080MBR](https://www.ti.com/lit/ds/symlink/hdc1080.pdf)
* Switcher [MT2492](https://datasheet.lcsc.com/lcsc/1810262207_XI-AN-Aerosemi-Tech-MT2492_C89358.pdf)
* LDO [SPX3819M5-L-3-3/TR](https://datasheet.lcsc.com/lcsc/1810181735_MaxLinear-SPX3819M5-L-3-3-TR_C9055.pdf)
* 40MHz Crystal [Q22FA12800332](https://datasheet.lcsc.com/lcsc/1810171117_Seiko-Epson-Q22FA12800332_C255899.pdf)
* RTC Crystal [M332768PWNAC](https://datasheet.lcsc.com/lcsc/2202131930_JYJE-M332768PWNAC_C2838414.pdf)
* LED [HL-PC-3216S9AC](https://datasheet.lcsc.com/lcsc/2009091206_HONGLITRONIC-Hongli-Zhihui--HONGLITRONIC--HL-PC-3216S9AC_C499470.pdf)

Battery Life
------------

Battery testing is performed using a stockpile of "Energizer EN92 Industrial"
AAA batteries manufactured in 2022. No OTA updates were issued during the
testing periods.

1-minute measurements, connecting every single minute:

* 17-18 days at room temperature
* 12 days at 40F
* 6 days at -5F

1-minute measurements, connecting every six minutes:

* 68-70 days at room temperature
* 60 days at 40F
* 28 days at -5F

1-minute measurements, connecting once each hour:

* 140-157 days at room temperature
* 130 days at 40F
* 68 days at -5F

5-minute measurements, connecting every five hours:

* 11-12 months at room temperature
* 6-7 months at 40F
* 4-5 months at -5F

Multiple power outages occurred during the final year of testing. Since the wifi
retry behavior is extremely aggressive, these results very likely underestimate
the possible battery life under more ideal conditions.

TODO
----

Things I'm planning to experiment with, in no particular order:

* Report early if temp/humidity delta exceeds a threshold
* PCB wifi antennas
* Add hardware to measure battery usage
* Bluetooth support, especially mesh networking
* Log collection over the network
* Signed binaries ("secure boot")

Cost
----

I had 30 rev 2.0 PCBs manufactured by JLCPCB in November 2022: the total cost
including shipping was $13.80/each (4-layer, ENIG, two-sided assembly, DHL).

![](https://static.wbinvd.org/img/tempsensor/lot.jpg)

Using the pogo pads, I was able to flash all 30 in under half an hour!
