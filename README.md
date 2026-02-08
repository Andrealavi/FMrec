# FMrec

A lightweight C application that interfaces with an RTL-SDR dongle (specifically optimized for RTL-SDR Blog V4) to demodulate Wideband FM (WBFM) broadcast signals and record them directly to a WAV file.

## Overview

This project implements a Software Defined Radio (SDR) receiver pipeline in C. It captures raw IQ samples from the radio hardware, performs digital signal processing (DSP) to demodulate the FM signal, and downsamples the audio for standard playback.

The implementation focuses on understanding the core mathematics of DSP, including phase discrimination, de-emphasis filtering, and DC offset removal.

## Usage

To use the program you need to have librtlsdr installed on your device. Depending on the kind of dongle you may need specific versions (e.g. I used a version of the library for the RTL-SDR Blog V4 dongle which is different from the standard one).

If you are working with a Macbook with Apple Silicon CPU and you have a V4 dongle with the correct version of the library installed you can compile the program by using:
```bash
make
```
Otherwise you might need to modify the `Makefile` content.

After compiling the program can be used in the following way:
```bash
./fmrec center_frequency audio_duration
```

The center frequency represent the FM radio station frequency that we want to record while the audio duration argument represents the number of seconds that we want to record.

## Features

* **RTL-SDR Integration**: Direct interface with `librtlsdr` to capture IQ samples at 960 kS/s.
* **FM Demodulation**: Uses an `atan2` phase discriminator to recover audio from frequency modulation.
* **Signal Conditioning**:
    * **De-emphasis Filter**: Compensates for the pre-emphasis applied by FM broadcast transmitters (configured for 50µs/Europe).
    * **DC Blocking**: Removes DC offset to center the signal waveform.
    * **Boxcar Decimation**: High-quality downsampling from 960 kHz to 48 kHz using averaging to reduce aliasing.
* **WAV Export**: Writes standard 16-bit PCM WAV headers for universal compatibility.

## License

MIT
