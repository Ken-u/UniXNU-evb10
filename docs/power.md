# CPU and GPU frequency, voltage and temperature on the EVB10

Status: **implemented and run on the board** (on by default: `rk3588-dvfs=6` is in `board.toml`). The three CPU clusters and the DSU change rate and voltage
together under one governor, the GPU under another; the temperature sensors are read and an over-temperature limit takes points off the top of the CPU clusters
and the GPU. The mechanism is shared by every RK3588 board and lives in `UniXNU-rk3588-common` (`scripts/data/Rk3588Dvfs/`, its README has the design), this
repository supplies the rails (`package/Rk3588DvfsBoard.cpp`).

Observed on the board: the voltages and the clocks the cores and the GPU really run at follow the tables, the scripted scroll runs at the frame rate it had with
the clocks fixed (the GPU's command buffers are shorter: 8.7 ms against 9.8 ms), the limit lowers the clocks when the sensors pass it and lifts them again, two
600 s runs with touches finished without a panic. **Not measured**: power. The board has no power meter and the temperature does not resolve what the governors
save (the heat sink keeps the idle temperature at 30 C, one sensor step is 0.93 C), so the saving is known as time spent at each point and as a **model** of the
cores' dynamic power (finding 9), not as watts. Not done: the DDR clock (finding 10), suspend (the board is mains powered, `sleep-disabled` stays set), a battery.

## Hardware

| Item | Value | Source |
| --- | --- | --- |
| Clock service | SCMI over SMC (function id `0x82000010`), shared memory at `0x10f000`; clocks CPUL 0, DSU 1, CPUB01 2, CPUB23 3, DDR 4, GPU 5, NPU 6 | stock device tree (`arm,scmi-smc`); the base and clock protocols answered on the board |
| Cortex-A55 rail | `vdd_cpu_lit_s0`: RK806 DCDC_REG2 on SPI2 (`0xfeb20000`, chip select 0), 550 to 950 mV, ramp 12.5 mV/us; also supplies the DSU | stock device tree; the firmware's platform library |
| Cortex-A76 rails | `vdd_cpu_big0_s0` RK8602 at 0x42 and `vdd_cpu_big1_s0` RK8603 at 0x43 on I2C0 (`0xfd880000`), 550 to 1050 mV, ramp 2.3 mV/us; big0 feeds cores 0 and 1 (clock CPUB01), big1 cores 2 and 3 (CPUB23) | same |
| GPU rail | `vdd_gpu_s0`: RK806 DCDC_REG1 (BUCK1) on SPI2, 550 to 950 mV, ramp 12.5 mV/us; the firmware leaves 750 mV | same; read back on the board |
| Temperature | TSADC at `0xfec00000`, seven channels (SoC, the two Cortex-A76 pairs, the Cortex-A55 cores, the SoC centre, GPU, NPU), 0.93 C per code step; the firmware leaves it clocked but not sampling | Rockchip's `rk_tsadcv8` sequence; the sensor reads 30 C on the idle board |
| CPU operating points | A55: 408, 600, 816, 1008, 1200, 1416, 1608, 1800 MHz at 675 / 675 / 675 / 675 / 712.5 / 762.5 / 850 / 950 mV; A76: 408, 816, 1008, 1200, 1416, 1608, 1800, 2016, 2208, 2304, 2400 MHz at 675 / 675 / 675 / 675 / 725 / 762.5 / 850 / 925 / 987.5 / 1000 / 1000 mV | edk2-rockchip `CpuPerformance.c` (Rockchip's tables for a typical die); the rates are the ones the clock service lists |
| GPU operating points | settings 200 (GPLL / 6), 300, 400, 500, 600 at 675 mV, 700 at 700 mV, 800 at 750 mV, 900 at 800 mV; measured 197, 310, 396, 502, 598, 719, 852, 957 MHz (finding 5) | the vendor `gpu_opp_table` voltages (typical die) and the GPU's cycle counter |
| Logical CPUs | 0 and 1 CPUB01, 2 and 3 CPUB23, 4 to 7 CPUL (`cpumask=31` starts 0 to 4 only) | SoC device tree order |

## What was found

1. **The nominal rate needs the table's voltage.** The firmware clocks the cores from a PVTPLL, a ring oscillator whose speed follows its supply: the rate the
   service reports is only a setting of that oscillator. Without touching the rails the rates are meaningless: with the Cortex-A76 rails at the 1.0 V the firmware
   leaves, every setting from 1608 MHz up measures 2230 MHz and 816 MHz measures 1220 MHz (run dvfs3). With the voltage of the table written first, the measured clock is
   within 5 % of the rate on all three clusters, from 400 MHz to 2.4 GHz (`evidence/dvfs-operating-points-20261101.txt`). This also explains the 2.23 GHz that the
   Cortex-A76 cores ran at under the 2.4 GHz request. The oscillator makes too low a voltage slow, not unsafe.
2. **The regulators are reachable from the kernel** without a driver of the firmware's: the RK806 over SPI2 (header bytes, then the register; buck n's voltage is
   register 0x1a + n - 1, 500 mV + 6.25 mV x code) and the RK8602/03 over I2C0 (register 0x06 holds the code, 500 mV + 6.25 mV per step, bit 7 of register 0x00 enables;
   the die id in register 0x03 tells a RK8602/03 from a RK8600/01). Both polled, with the firmware's own bus setup kept (I2C CLKDIV `0x007b007b`, 100 kHz).
3. **The DSU is on the Cortex-A55 clock and rail, and every cluster pays for it.** The service sets the DSU to the Cortex-A55 rate whenever that is set; the DSU also takes
   a rate of its own afterwards. A Cortex-A76 core's load from the L3 takes 18.1 ns with the DSU at 1800 MHz, 26.1 ns at 1008 MHz, 51.9 ns at 396 MHz; from memory 120, 134
   and 181 ns (`evidence/dvfs-dsu-20261101.txt`). A governor that drops the Cortex-A55 cluster because the A55 cores are idle would therefore slow the A76 cores' memory
   traffic: the DSU follows the faster A76 cluster instead, at three quarters of its rate. (The probe once printed 0 ns: the compiler had deleted the pointer chase
   whose result nothing read. The sink is volatile now; the run after the fix, gd6, gives the numbers above again.)
4. **The scripted scroll costs no frames.** The scripted swipes (gestures 3 to 11): 52.9 frames per second with the CPU governor (run gov2), 52.8 without (base1); the GPU is
   the limit there (a command buffer takes 9.6 ms on it and the gap between frames sits at the 16.6 ms of the display). Over 63 swipes in 600 s (gov3): 52.1 frames per
   second while the finger was down. In the last 300 s of gov3 (a swipe of 0.8 s every 7 s) CPUB01 spent 23 % of the time at 408, 44 % at 1200 and 30 % at 2400 MHz,
   CPUB23 40 / 30 / 28 %, CPUL 91 % at 408 MHz, and the DSU averaged 1161 MHz (`evidence/dvfs-governor-20261101.txt`).
5. **The GPU's clock is a PVTPLL too, and the Mali driver does not use the service.** The settings 300 to 900 MHz of the clock service put the GPU on its PVTPLL
   (bit 14 of CRU `CLKSEL_CON(158)`); 200 MHz is GPLL / 6 and does not depend on the voltage; 1000 MHz is ignored (the clock stays at 750 MHz); the 800 and 900 settings
   are one ring that differs only in the voltage the table gives it. The Mali driver sets CPLL / 2 = 750 MHz itself at the firmware's 750 mV. The ring follows the supply at
   about 2.1 to 2.3 MHz per millivolt (run gv1, the GPU's cycle counter against the architected counter): at the vendor voltage of each setting the GPU measures 310 MHz at
   300 (675 mV), 396 at 400, 502 at 500, 598 at 600 (675 mV), 719 at 700 (700 mV), 852 at 800 (750 mV) and 957 at 900 (800 mV); at 850 mV the 900 setting reaches 1053 MHz
   (run gs1) and at 675 mV it reaches 679 MHz (`evidence/gpu-clock-20261101.txt`). So the vendor table's voltages reproduce its rates on this die, and the 800 setting gives
   852 MHz at the voltage the GPU has today.
6. **The GPU's busy share can be read.** `GPU_STATUS` bit 0 is set while the GPU works; sampled every 7 us for 50 ms windows (run gw1) it reads 0 % on the idle display and 45 to
   84 % during the scripted swipes (107 of 321 busy windows at 60 % and over), the shaders stay powered through the busy windows and go off about 10 ms after the last work.
7. **The GPU governor changes nothing in the frame rate and shortens the command buffer.** Scripted swipes 3 to 11 with the CPU and the GPU governor (run gg1): 53.8 frames per
   second, median command buffer 8.7 ms; the final code over 600 s (gf1): 52.8 on those swipes, 53.2 over all 61 moving swipes, 8.7 ms; before it, 52.8 and 9.8 ms (base1). The
   GPU spent 65 to 100 % of each ten seconds at 200 MHz and 675 mV and was at the top while a swipe ran (mean 327 MHz at 8.5 % busy over the 480 s of gf1 after the boot). Letting it use the 900
   setting (957 MHz at 800 mV, run gm900) gives 7.8 ms command buffers and 53.3 frames per second against 53.8, so the ceiling stays at the 800 setting
   (`evidence/gpu-governor-20261101.txt`).
8. **The temperature limit works.** With the limit set 3 C above the idle temperature (run gt33) the governor took up to five points off the top of every cluster while the
   system was busy (boot, then the swipes; 93 % of one ten-second window throttled) and gave them back as the sensors fell (steps back to 0 at 140 to 180 s); the hottest
   sensor reading seen was 34.2 C. At the default of 85 C it did not act in tb1 and tg1, whose hottest reading was 36.0 C (`evidence/dvfs-thermal-20261101.txt`).
9. **A model of the saving, and a shorter touch boost.** Two idle runs of 600 s, with the clocks as the firmware leaves them (tb1) and with the governor (tg1), gave mean
   temperatures of 30.04 and 29.64 C after 200 s: a difference of 0.4 C, which is less than the difference between their starting temperatures (31.4 and 30.5 C), so the
   sensor does not say what the governor saves. The log carries a model instead: P = C x f x V^2 of the busy time of each cluster, with the dynamic-power coefficients of the
   vendor device tree (A55 100, A76 300, GPU 2982 uW per MHz per V^2), against the same work at the top point. On the scripted swipes (gestures and idle together) the cores'
   model power is 12 % lower with the old 1000 ms touch boost (gg1), 15 % lower with 300 ms (gb300) and 23 % lower with no boost (gb0); the frame rate on gestures 3 to 11 is
   53.8 / 53.3 / 51.1 and the gaps above 25 ms are 4 / 6 / 16 (single runs: a difference of half a frame per second is noise). The default is now 300 ms. The final 600 s run (gf1)
   models 271 mW against 310 mW at the top points (13 % less). The GPU model does not move (its top point has the voltage it had); its saving is the 200 MHz / 675 mV idle
   state, which is not in the model and was not measured.
10. **The DDR clock is left alone.** The service lists no rates for clock 4 (a rate query answers error 102), so there is nothing to select. The vendor kernel sets the DDR
   rate with a SIP call into BL31 (`ROCKCHIP_SIP_CONFIG_DRAM_SET_RATE`) and waits for the display's vertical blank first (`rockchip_dmcfreq_wait_complete`); doing that here
   means coordinating with the VOP scanout, which is the part the board has working, so it was not attempted.

## How it works

* `Rk3588Domain`: a cluster's operating points (the table points the firmware's rate list also has) and `levelSet`: voltage first when the point is faster, clock first
  when it is slower; on the Cortex-A55 domain the voltage suits the faster of the cores and the DSU. The rates are read back (the service must answer with the rate asked for).
* `Rk3588Governor`: every 16 ms, per cluster, the busiest core's busy share from the CPUs' idle time (`rk3588_cpu_idle_mach`, XNU patch `rk3588-cpu-idle`): the lowest point at
  or above 1.25 x busy x the current rate, the top point from 90 % busy, up at once and down after four such windows; the A76 clusters do not go under half their top rate while
  there is work (unless the busiest core stayed under 10 % for 12 windows); a moving `rk3588_dvfs_activity()` (here: the touch driver's HID report counter) holds them at the top for
  `rk3588-dvfs-boost-ms` (default 300). Every eighth window it reads the hottest CPU, SoC-centre and GPU sensor (`Rk3588Thermal`): at or above `rk3588-dvfs-tmax` (default 85 C)
  it takes one more point off the top of every cluster (up to eight), below tmax minus 2 C it gives one back. A cluster whose clock service, regulator or bus fails five times goes
  back to its top point and is left alone.
* `Rk3588Gpu`: a thread of its own samples `GPU_STATUS` about every millisecond (every 8 ms after a second without work); every 25 ms it takes the top point at 40 % busy or more,
  otherwise the lowest point that would keep the GPU about 30 % busy at the clock it has (the vendor GPU's devfreq asks for 30 % up and 10 % down), up at once and down after three
  windows; a touch holds the top for `rk3588-gpu-boost-ms` (200). Rate and voltage are ordered like the CPU's and the service must read back the setting asked for; five failures put the
  GPU back at the top and leave it alone. The CPU governor's temperature limit lowers the GPU's ceiling by the same steps. Points: 200, 300 to 800 (the 800 setting is the default
  ceiling, `rk3588-gpu-max`). `rk3588-gpu=0` leaves the GPU as the Mali driver set it.
* The board file supplies, per rail: the SCMI clock, the regulator (RK806 buck or RK860x bus and address), limits and ramp. A voltage outside the limits is never written.

Boot arguments: `rk3588-dvfs=N` (0 off, 1 report, 2 to 5 probes, **6 governors**, 7 governors that only watch the temperature, 8 to 10 GPU probes), `rk3588-dvfs-min=MHz`,
`rk3588-dvfs-boost-ms=N`, `rk3588-dvfs-tmax=C`, `rk3588-dvfs-report=N`, `rk3588-gpu=0|1`, `rk3588-gpu-min=MHz`, `rk3588-gpu-max=MHz`, `rk3588-gpu-boost-ms=N`. The probes are for
another board's bring-up: 1 prints the clocks, 2 and 3 sweep the rates and read the regulators, 4 walks the operating points with voltages, 5 measures the DSU, 8 sets each rate of
the GPU's clock service and measures it, 9 prints the GPU's busy share, 10 walks the GPU voltage down for each setting.

## Reproduce

```
cd UniXNU-evb10 && scripts/build-evb10-kernel.sh
cd ../uefi-evb10
# the governors are in the default arguments; for the comparison run without them add rk3588-dvfs=0
EXTRA_KERNEL_ARGS="evb10-touch-demo=120 mali-timeline=1" CAPTURE_SECS=600 EARLY_STOP=0 setsid nohup tools/xnu-probe/run-kernel.sh gf1 > runs/gf1.driver.log 2>&1 </dev/null &
python3 tools/xnu-probe/frame_phases.py gf1
rg 'rk3588-dvfs: gov' runs/gf1.kernel.log
# the GPU's clock and voltage: rk3588-dvfs=8 (rates), 10 (voltage walk), 9 with the swipes (busy share); the temperature: rk3588-dvfs=7 for 600 s
```

## Open items

* Power: the board has no meter. With one (or a USB power meter on the supply), compare `rk3588-dvfs=0` with the governors on the same scripted swipes; the model above is not a
  measurement, and the GPU's idle saving (200 MHz and 675 mV for most of the time) is not in it.
* The DDR clock (clock 4): needs the SIP call and the display's vertical blank (finding 10).
* The Cortex-A55 cores 5 to 7 are off (`cpumask=31`): the governor reads only the cores that run.
* Operating points are Rockchip's typical-die tables; a die that needs more voltage at a point will run that point slower, not wrongly (findings 1 and 5), but nothing reports it:
  the vendor's variants for other dies (PVTM based voltage selection) are not read.
* Frame rates are single runs of 9 swipes (or 61 in 600 s); the run-to-run spread was not measured (no identical run was repeated), so differences under about one frame per second mean nothing.
