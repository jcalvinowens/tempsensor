
ESP32C3 Temperature Sensor
==========================

This repo contains the hardware design, firmware, and HTTP backend for a small
battery powered wifi temperature/humidity sensor.

![](img/front.jpg)

The firmware is written in C, using ESP-IDF (which is based on FreeRTOS).

The backend is written in Python, using the native HTTP and sqlite3 support. It
has no arbitrary limit on the number of sensors it can support, and uses gnuplot
to render graphs and serve them over HTTPS:

![](img/graph.png)

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

I more or less followed the [ESP32-C3 design guidelines](https://www.espressif.com/sites/default/files/documentation/esp32-c3_hardware_design_guidelines_en.pdf)
when laying out the board, with the exception of RF impedence matching. The RF
trace and my antennas are all 50 Ohm, which, with the 35 Ohm LNA output, results
in a VSWR of ~1.4:1. The loss (versus a matched line) is less than an S-unit, so
it wasn't worth the money.

I used small laptop syle wifi antennas, which are easy to find in bulk for cheap
nowadays if they aren't compatible with 5GHz. Because they log signal strength,
the sensors will be useful for future experimentation with PCB antenna designs.

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

The board is programmed via pogo pads. I spaced the pads to match a pogo clip I
ordered online, but it turned out the spacing was different than advertised...
so I had to kludge something together myself to match the boards I already had.

![](img/pogo.jpg)

In retrospect, I could've made my life easier by avoiding routing in-between the
pads so they could be bigger. Even as is, holding the pogo pins on with my
thumb wasn't a problem at all.

![](img/prog.jpg)

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

I tested 22 sensors connecting every minute in my house: the room temperature
sensors lasted 17-18 days, a sensor placed outdoors which saw daily temps
30F - 55F lasted 10 days, a sensor in my 40F refrigerator lasted 12 days, and a
sensor in my -5F freezer lasted 6 days.

With the new code that stores readings in NVS and turns the wifi on much less
frequently, the battery life will be much longer: hopefully it will scale more
or less linearly, I'm currently testing 5-minute reporting intervals.

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

![](img/lot.jpg)

Using the pogo pads, I was able to flash all 30 in under half an hour!
