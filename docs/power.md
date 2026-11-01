# CPU frequency and voltage on the EVB10

Status: **implemented and run on the board** (on by default: `rk3588-dvfs=6` is in `board.toml`). The three CPU clusters and the DSU change rate and voltage
together under a governor; the mechanism is shared by every RK3588 board and lives in `UniXNU-rk3588-common` (`scripts/data/Rk3588Dvfs/`, its README has the
design), this repository supplies the rails (`package/Rk3588DvfsBoard.cpp`). What was observed: the voltages and the clocks the cores really run at follow
the tables, the scripted scroll runs at the frame rate it had with the clocks fixed, a 600 s run with touches finished without a panic. **Not measured**: power
and temperature (nothing on the board or on the host reads them), so how much the governor saves is known only as time spent at each point.
Not done: thermal limits, GPU and memory clocks, suspend (the board is mains powered, `sleep-disabled` stays set), a battery.

## Hardware

| Item | Value | Source |
| --- | --- | --- |
| Clock service | SCMI over SMC (function id `0x82000010`), shared memory at `0x10f000`; clocks CPUL 0, DSU 1, CPUB01 2, CPUB23 3, DDR 4, GPU 5, NPU 6 | stock device tree (`arm,scmi-smc`); the base and clock protocols answered on the board |
| Cortex-A55 rail | `vdd_cpu_lit_s0`: RK806 DCDC_REG2 on SPI2 (`0xfeb20000`, chip select 0), 550 to 950 mV, ramp 12.5 mV/us; also supplies the DSU | stock device tree; the firmware's platform library |
| Cortex-A76 rails | `vdd_cpu_big0_s0` RK8602 at 0x42 and `vdd_cpu_big1_s0` RK8603 at 0x43 on I2C0 (`0xfd880000`), 550 to 1050 mV, ramp 2.3 mV/us; big0 feeds cores 0 and 1 (clock CPUB01), big1 cores 2 and 3 (CPUB23) | same |
| Operating points | A55: 408, 600, 816, 1008, 1200, 1416, 1608, 1800 MHz at 675 / 675 / 675 / 675 / 712.5 / 762.5 / 850 / 950 mV; A76: 408, 816, 1008, 1200, 1416, 1608, 1800, 2016, 2208, 2304, 2400 MHz at 675 / 675 / 675 / 675 / 725 / 762.5 / 850 / 925 / 987.5 / 1000 / 1000 mV | edk2-rockchip `CpuPerformance.c` (Rockchip's tables for a typical die); the rates are the ones the clock service lists |
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
   traffic: the DSU follows the faster A76 cluster instead, at three quarters of its rate.
4. **The scripted scroll costs no frames.** The scripted swipes (gestures 3 to 11): 52.9 frames per second with the governor (run gov2), 52.8 without (base1); the GPU is
   the limit there (a command buffer takes 9.6 ms on it and the gap between frames sits at the 16.6 ms of the display). Over 63 swipes in 600 s (gov3): 52.1 frames per
   second while the finger was down. In the last 300 s of gov3 (a swipe of 0.8 s every 7 s) CPUB01 spent 23 % of the time at 408, 44 % at 1200 and 30 % at 2400 MHz,
   CPUB23 40 / 30 / 28 %, CPUL 91 % at 408 MHz, and the DSU averaged 1161 MHz (`evidence/dvfs-governor-20261101.txt`). A touch lifts the A76 clusters to 2.4 GHz for a
   second (64 boosts in gov3).

## How it works

* `Rk3588Domain`: a cluster's operating points (the table points the firmware's rate list also has) and `levelSet`: voltage first when the point is faster, clock first
  when it is slower; on the Cortex-A55 domain the voltage suits the faster of the cores and the DSU. The rates are read back (the service must answer with the rate asked for).
* `Rk3588Governor`: every 16 ms, per cluster, the busiest core's busy share from the CPUs' idle time (`rk3588_cpu_idle_mach`, XNU patch `rk3588-cpu-idle`): the lowest point at
  or above 1.25 x busy x the current rate, the top point from 90 % busy, up at once and down after four such windows; the A76 clusters do not go under half their top rate while
  there is work (unless the busiest core stayed under 10 % for 12 windows); a moving `rk3588_dvfs_activity()` (here: the touch driver's HID report counter) holds them at the top for
  `rk3588-dvfs-boost-ms` (default 1000). A cluster whose clock service, regulator or bus fails five times goes back to its top point and is left alone.
* The board file supplies, per rail: the SCMI clock, the regulator (RK806 buck or RK860x bus and address), limits and ramp. A voltage outside the limits is never written.

Boot arguments: `rk3588-dvfs=N` (0 off, 1 report, 2 to 5 probes, **6 governor**), `rk3588-dvfs-min=MHz`, `rk3588-dvfs-boost-ms=N`, `rk3588-dvfs-report=N`. The probes are for another board's
bring-up: 1 prints the clocks, 2 and 3 sweep the rates and read the regulators, 4 walks the operating points with voltages, 5 measures the DSU.

## Reproduce

```
cd UniXNU-evb10 && scripts/build-evb10-kernel.sh
cd ../uefi-evb10
# the governor is in the default arguments; for the comparison run without it add rk3588-dvfs=0
EXTRA_KERNEL_ARGS="evb10-touch-demo=120 mali-timeline=1" CAPTURE_SECS=235 EARLY_STOP=0 setsid nohup tools/xnu-probe/run-kernel.sh gov2 > runs/gov2.driver.log 2>&1 </dev/null &
python3 tools/xnu-probe/frame_phases.py gov2
rg 'rk3588-dvfs: gov' runs/gov2.kernel.log
```

## Open items

* Power and temperature: read the TSADC and, if the board has one, a power meter, to say what the governor saves. Most of the time at the top point is the touch boost (a swipe
  of 0.8 s plus the 1 s hold every 7 s is 26 %): a shorter hold or a boost to the point below the top would cut that, and the frame rate says whether it can.
* The Cortex-A55 cores 5 to 7 are off (`cpumask=31`): the governor reads only the cores that run.
* GPU frequency (SCMI clock 5 and the GPU rail BUCK1 at 0.75 V; the Mali driver sets its own clock today) and the DDR clock (clock 4).
* Operating points are Rockchip's typical-die tables; a die that needs more voltage at a point will run that point slower, not wrongly (finding 1), but nothing reports it.
