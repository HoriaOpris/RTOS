# RTOS practice

## msp430-freertos

FreeRTOS V11.3.3 on the MSP-EXP430FR6989 LaunchPad, as a Code Composer Studio project.

- `main.c`: hardware setup, UART receive interrupt, blink and console tasks, a queue and a mutex.
- `lcd.h`, `lcd.c`: driver for the board's 6-character LCD (`lcd_print`, `lcd_put_char`, `lcd_print_uint`, `lcd_clear`).
- `FreeRTOSConfig.h`: kernel settings (tick rate, heap size, priorities).
- `FreeRTOS/`: unmodified kernel sources and the CCS MSP430X port.

The 6 KB FreeRTOS heap is placed in FRAM because the chip has only 2 KB of RAM.

### Open in CCS

File > Import Project(s), then choose the `msp430-freertos` folder. Build and debug as usual.

### Build and flash from the terminal

```bash
CCS=/Applications/ti/ccs2101/ccs
"$CCS/ccs-server.app/Contents/MacOS/ccs-server-cli.sh" -noSplash -workspace /tmp/ccs-ws \
  -application com.ti.ccs.apps.importProject -ccs.location msp430-freertos
"$CCS/ccs-server.app/Contents/MacOS/ccs-server-cli.sh" -noSplash -workspace /tmp/ccs-ws \
  -application com.ti.ccs.apps.buildProject -ccs.projects msp430-freertos -ccs.configuration Debug
"$CCS/ccs_base/DebugServer/bin/DSLite" flash -c msp430-freertos/targetConfigs/MSP430FR6989.ccxml \
  -e -f -v -u msp430-freertos/Debug/msp430-freertos.out
```

### Talk to the board

115200 baud, 8N1, on the LaunchPad's application UART (the second `usbmodem` port):

```bash
screen /dev/cu.usbmodem3103 115200
```

The board draws a status screen that refreshes twice a second: red LED flash count, up time,
green LED state, the last command's result and the command list.
Commands, each ended with Enter: `on`, `off` (green LED), `stats`, `lcd TEXT` (show TEXT on the LCD). Anything else is echoed.
Quit `screen` with Ctrl-A then K.
