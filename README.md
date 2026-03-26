# DOS Graphics Demos

Retro DOS graphics demos targeting real hardware: VGA plasma effects, water simulations, and ATI Radeon X1300 Pro (RV515) 2D GPU acceleration — all running in 32-bit protected mode.

## Projects

| Directory | Description |
|-----------|-------------|
| `PLASMA/` | VGA/VESA plasma, water ripple, and fire effects with RDTSC timing and write-combining |
| `RADEON/` | ATI Radeon RV515 2D GPU acceleration: parallax scrolling, dune chase demo, GPU benchmarks |
| `VESADEMO/` | VESA VBE mode enumeration and basic framebuffer demo |
| `DUCKHUNT/` | Duck Hunt game (DJGPP/GCC build) |
| `BC31/` | Borland C 3.1 installer utilities |

## Hardware Targets

- **CPU**: Pentium+ (uses RDTSC for microsecond timing)
- **GPU**: ATI Radeon X1300 Pro (RV515) for RADEON demos, any VESA 2.0+ card for PLASMA/VESADEMO
- **Mode**: 800×600 8bpp (VESA mode 0x103) for most demos
- **DOS extender**: PMODE/W (OpenWatcom) or CWSDPMI (DJGPP)

## Cross-Compilation Setup

All projects use **OpenWatcom 2.0** targeting 32-bit DOS with the PMODE/W extender. The toolchain runs on Windows, Linux, or macOS — the output is a standard DOS `.exe`.

### Windows Setup

#### 1. Install OpenWatcom 2.0

Download from https://github.com/open-watcom/open-watcom-v2/releases

Run the installer, or extract the ZIP to e.g. `C:\WATCOM`. The installer sets environment variables automatically; for ZIP extraction, set them manually.

#### 2. Set Environment Variables

Add to your **System Environment Variables** (or run in each CMD session):

```cmd
set WATCOM=C:\WATCOM
set PATH=%WATCOM%\binnt64;%WATCOM%\binnt;%PATH%
set INCLUDE=%WATCOM%\h
```

For permanent setup, add these to **System Properties → Environment Variables**:
- `WATCOM` = `C:\WATCOM`
- Prepend `%WATCOM%\binnt64;%WATCOM%\binnt` to `PATH`
- `INCLUDE` = `%WATCOM%\h`

#### 3. Build from Command Prompt

```cmd
cd RADEON

rem Build RADEON.exe (main demo)
wcc386 -bt=dos -5r -ox -s -zq RADEON.C
wlink system pmodew name RADEON file RADEON option quiet

rem Build RBLIT.exe (GPU diagnostic tests)
wcc386 -bt=dos -3r -ox -s -zq RBLIT.C
wlink system pmodew name RBLIT file RBLIT option quiet
```

```cmd
cd PLASMA

rem Build PLASMA.exe
wcc386 -bt=dos -6r -ox -oh -on -zq PLASMA.C
wcc386 -bt=dos -6r -ox -oh -on -zq VGA.C
wcc386 -bt=dos -6r -ox -oh -on -zq WAVE.C
wlink system pmodew name PLASMA file PLASMA,VGA,WAVE option quiet
```

#### 4. Windows Batch Files (Optional)

Create `RADEON\build.bat`:
```bat
@echo off
set WATCOM=C:\WATCOM
set PATH=%WATCOM%\binnt64;%WATCOM%\binnt;%PATH%
set INCLUDE=%WATCOM%\h

echo Building RADEON...
wcc386 -bt=dos -5r -ox -s -zq RADEON.C
if errorlevel 1 goto :fail
wlink system pmodew name RADEON file RADEON option quiet
if errorlevel 1 goto :fail
echo Done: RADEON.exe
goto :eof
:fail
echo BUILD FAILED
```

Create `RADEON\build_blit.bat`:
```bat
@echo off
set WATCOM=C:\WATCOM
set PATH=%WATCOM%\binnt64;%WATCOM%\binnt;%PATH%
set INCLUDE=%WATCOM%\h

echo Building RBLIT...
wcc386 -bt=dos -3r -ox -s -zq RBLIT.C
if errorlevel 1 goto :fail
wlink system pmodew name RBLIT file RBLIT option quiet
if errorlevel 1 goto :fail
echo Done: RBLIT.exe
goto :eof
:fail
echo BUILD FAILED
```

### Linux Setup (current dev environment)

```bash
# Install OpenWatcom 2.0 to /opt/watcom
# Download from https://github.com/open-watcom/open-watcom-v2/releases
# Use the Linux installer or extract the tarball

export WATCOM=/opt/watcom
export PATH=$WATCOM/binl64:$WATCOM/binl:$PATH
export INCLUDE=$WATCOM/h

cd RADEON && bash build.sh
cd PLASMA && bash build.sh
```

### Compiler Flags Reference

| Flag | Meaning |
|------|---------|
| `-bt=dos` | Target DOS |
| `-3r` | 386 register calling convention (RBLIT — no RDTSC) |
| `-5r` | Pentium register calling convention (RADEON — uses RDTSC) |
| `-6r` | Pentium Pro register convention (PLASMA — max optimization) |
| `-ox` | Maximum optimization (`-obmiler -s`) |
| `-oh` | Expensive optimizations (better register allocation) |
| `-on` | Relaxed floating-point (`-ffast-math` equivalent) |
| `-s` | Remove stack overflow checks |
| `-zq` | Quiet mode |
| `system pmodew` | Link with PMODE/W DOS extender (built into .exe) |

### Running on Real Hardware

1. Copy the `.exe` file to a DOS-bootable USB drive or hard disk
2. Boot into DOS (FreeDOS or MS-DOS 6.22+)
3. For RADEON demos: requires ATI Radeon X1300 Pro (RV515) or similar R300-R500 GPU
4. For PLASMA/VESADEMO: any VESA 2.0 compatible video card
5. Optional: run `WCINIT.EXE` before demos to enable write-combining for ~30× faster CPU framebuffer access

```
C:\> WCINIT          (enable write-combining, run once after boot)
C:\> RADEON          (GPU demos with menu)
C:\> RBLIT           (GPU diagnostic test suite, logs to BLITLOG.TXT)
C:\> PLASMA          (VGA plasma/water effects)
```

## RADEON Demo Features

- **Pattern Demo**: GPU-drawn geometric patterns (fill + line primitives)
- **Bouncing Blit**: Color-keyed transparent sprite bouncing with GPU blits
- **CPU vs GPU Benchmark**: Full-screen fill comparison with RDTSC timing
- **Rectangle Flood**: Maximum GPU throughput test (batched FIFO, XORshift PRNG)
- **Dune Chase**: 16-layer fractal mountain parallax + 128-segment rainbow worm
- **Diagnostic Parallax**: Colored grid layers for debugging GPU blit issues
- **Scenic Parallax**: Multi-layer landscape scrolling with CPU utilization HUD

## RBLIT Diagnostic Suite

75+ tests validating every aspect of the RV515 2D engine:
- Basic fills, blits, color-keyed transparency
- Scissor clipping, source/destination overlap
- PITCH_OFFSET encoding (1KB alignment, combined registers, GMC control bits)
- Multi-surface addressing, page isolation
- VGA CRTC state, AVIVO display controller

Known expected failures (5 tests) are labeled `[EXPECTED]` with explanations — these are hardware characteristics, not bugs.
