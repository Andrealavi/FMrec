#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

#include "rtl-sdr.h"

// Sample rate is set to 960 kHz as it is a multiple of the WAV file sample rate
// and it is sufficiently high to work well with the SDR dongle and to correctly
// sample the radio signal (it is almost five time the FM signal bandwidth).
#define SAMPLE_RATE 960000
#define AUDIO_RATE 48000
#define DECIMATION_FACTOR (SAMPLE_RATE / AUDIO_RATE)

// Size of the buffer used to store samples. 16384 represents the number of
// bytes contained in a USB packet the one used to send data from the dongle to
// the CPU
#define BUFFER_SIZE (16 * 16384)
#define AUDIO_DURATION 5
#define SDR_INDEX 0

// Time coefficient used in the de-emphasis filter. It represents the speed to
// which the physical circuit reacts and it is used to convert the de-emphasis
// filter in software.
// The value of the constant depend on the continent in which the FM signals are
// being transmitted: 50 microseconds for Europe/Asia/Africa and 75 microseconds
// for Americas/Korea.
#define TAU 0.000050

// Standard WAV file header structure (44 bytes).
// This struct represents the standard RIFF/WAVE header for PCM audio.
typedef struct {
    // RIFF Chunk Descriptor 
    char     chunkId[4];        // "RIFF" (Big Endian)
    uint32_t chunkSize;         // File size - 8 bytes
    char     format[4];         // "WAVE" (Big Endian)
    // Format Sub-chunk
    char     fmtChunkId[4];     // "fmt " (includes trailing space)
    uint32_t fmtChunkSize;      // Size of the fmt chunk (usually 16 for PCM)
    uint16_t audioFormat;       // Audio format (1 = PCM, 3 = IEEE Float)
    uint16_t numChannels;       // Number of channels (1 = Mono, 2 = Stereo)
    uint32_t sampleRate;        // Sampling frequency in Hz (e.g., 44100)
    uint32_t byteRate;          // Bytes per second (SampleRate * BlockAlign)
    uint16_t blockAlign;        // Bytes per sample frame (NumChannels * BitsPerSample / 8)
    uint16_t bitsPerSample;     // Bits per sample (e.g., 16, 24, 32)
    // Data  Sub-chunk
    char     dataChunkId[4];    // "data" (Big Endian)
    uint32_t dataSize;          // Size of the raw audio data in bytes
} WavHeader;

// Convert value from unsigned int to float.
// This function is used for converting IQ samples to float values.
float convert_value(uint8_t value) {
    return (float)value - 127.5f;
}

// Compute the istantaneous frequency from a pair of IQ samples.
// This is the core of FM demodulation, as it converts the raw IQ samples into
// a frequency value that contains the actual audio data.
//
// The math behind is simple: we need to compute the derivative of the phase,
// which is the angle of the signal in the complex plane. To compute the phase
// we need to compute the arctan of Q/I, following simple trigonometry formalas.
// Then we compute the derivative by subtracting the two consecutive phases.
// It is important to handle the case where the shift in phase is small but
// the computed arctan value is the clipped to the opposite value (e.g. +pi and
// -pi), as it will cause a sudden jump in the final audio. 
// To solve this issue we can subtract or add 2pi depending on
// the frequency value.
float get_instant_freq(float i1, float q1, float i2, float q2) {
    float phase1 = atan2(q1, i1);
    float phase2 = atan2(q2, i2);

    float instant_freq = phase2 - phase1;
    
    if (instant_freq > M_PI) instant_freq -= 2 * M_PI;
    else if (instant_freq < -M_PI) instant_freq += 2 * M_PI;

    return instant_freq;
}

// Compute istantaneous frequency over all the IQ samples.
void get_freq_values(float *freq_samples, float *i_samples, float *q_samples, int len) {
    for (int i = 0; i < len - 1; i++) {
        freq_samples[i] = get_instant_freq(
                i_samples[i], q_samples[i], i_samples[i+1], q_samples[i+1]
        );
    }
}

// De-emphasize filter is a low-pass filter that is used to reduce high
// frequency components in the signal.
// This is crucial because in FM transmissions, transmitters apply a boost
// to the high frequencies to make them less susceptible to noise,
// however at the receiver this will result in distorted audio.
// So it is crucial to apply such filter.
// To reproduce this filter in the digital domain an exponential moving average
// is used to replicate the same decay effect that is part of the analog circuit.
//
// The coefficient used in the average is computed using the sample rate and
// the time constant tau that depends on the continent where we are trying
// to demodulate the signal.
void deemphasize_filter(float *freq_samples, float last_sample, int len) {
    float alpha = 1.0 - exp(-(1.0/(TAU * SAMPLE_RATE)));

    freq_samples[0] = alpha * freq_samples[0] + (1.0f - alpha) * last_sample;
    for (int i = 1; i < len; i++) {
        freq_samples[i] = alpha * freq_samples[i] + (1.0f - alpha) * freq_samples[i - 1];
    }
}

// DC block filter is an high-pass filter used to reduce the impact of the DC
// frequencies. In the digital domain it works by centering the frequency around
// 0 using the two operations: first we compute the difference between the last
// two samples and then we add a fraction of the previous output. This formula
// translates the high-pass CR circuit.
void dc_block_filter(float *samples_buffer, int len) {
    const float R = 0.99;

    float last = samples_buffer[0];
    float tmp_res = 0.0f;
    for (int i = 1; i < len; i++) {
        tmp_res = samples_buffer[i] - last + R * samples_buffer[i-1];
        last = samples_buffer[i];
        samples_buffer[i] = tmp_res;
    }
}

// Perform FM signal demodulation. Specifically it performs the following
// operations:
// - Separate I and Q samples into two different arrays
// - Compute the frequency samples
// - Apply De-emphasize filter on frequency samples
// - Apply DC block filter on frequency samples
float demodulate(float *freq_samples, float last_sample, uint8_t *buffer, int len) {
    float *i_samples = malloc(sizeof(float) * (len / 2));
    float *q_samples = malloc(sizeof(float) * (len / 2));

    for (int i = 0, j = 0; j < len; i++, j += 2) {
        i_samples[i] = convert_value(buffer[j]);
        q_samples[i] = convert_value(buffer[j+1]);
    }

    get_freq_values(freq_samples, i_samples, q_samples, len/2);
    deemphasize_filter(freq_samples, last_sample, len/2);
    dc_block_filter(freq_samples, len/2);

    free(i_samples);
    free(q_samples);
    return freq_samples[len/2 - 1];
} 

// Decimate frequency samples to match the sample rate of the WAV audio file.
// This is a fundamental operation for converting the FM audio into the WAV
// file.
// Decimation, in this case, consists in accepting a sample every
// DECIMATION_FACTOR samples. The decimation factor is computed as the ratio
// between the SDR sample rate and the audio file sample rate.
int decimate(float *decimated_samples, float *freq_samples, int len) {
    for (int i = 0, j = 0; i < (len / DECIMATION_FACTOR); i++, j+=DECIMATION_FACTOR) {
        decimated_samples[i] = freq_samples[j];
    }

    return len / DECIMATION_FACTOR;
}

// Converts samples from float to int16_t for the WAV audio file.
// Since the WAV file will contain 16bit integers, it is important to convert
// them.
// This is done by clipping the sample value and converting it into an int16_t
// data type. The gain is used to take into account the value difference between
// the frequency sample and the audio file.
// Frequency samples go from -1 to 1, while WAV samples go from -32768 to 32767.
// It is fundamental to clip values to make them fit the 16 bit integer.
void convert_samples(int16_t *buffer, float *samples, int len) {
    const float GAIN = 32767.0f;

    for (int i = 0; i < len; i++) {
        float res = samples[i] * GAIN;

        if (res > 32767.0f) res = 32767.0f; 
        else if (res < -32768.0f) res = -32768.0f; 

        buffer[i] = (int16_t)res;
    }
}

int main(int argc, char **argv) {
    // Configuration of the SDR device.
    rtlsdr_dev_t *sdr = NULL;
    float center_freq = 0.0;
    int audio_duration = 0;

    if (argc > 1) {
        center_freq = atof(argv[1]);
        audio_duration = atoi(argv[2]);
    } else {
        fprintf(stderr, "Missing center frequency argument or audio duration\n");
        exit(1);
    }

    if (rtlsdr_open(&sdr, SDR_INDEX) < 0) {
        fprintf(stderr, "Failed to open SDR sdrice.\n");
    }

    rtlsdr_set_center_freq(sdr, center_freq * 1000000.0);
    rtlsdr_set_sample_rate(sdr, SAMPLE_RATE);
    rtlsdr_set_tuner_gain_mode(sdr, 0);
    rtlsdr_reset_buffer(sdr);

    // Main FM demodulation and audio recording logic.
    FILE *audio_file = fopen("audio.wav", "wb");

    // Fill WAV header with known values.
    WavHeader header;
    memcpy(header.chunkId, "RIFF", 4);
    header.chunkSize = 0; // Placeholder, will fix later
    memcpy(header.format, "WAVE", 4);
    memcpy(header.fmtChunkId, "fmt ", 4);
    header.fmtChunkSize = 16;
    header.audioFormat = 1; // PCM
    header.numChannels = 1; // Mono
    header.sampleRate = AUDIO_RATE; // 48000
    header.bitsPerSample = 16;
    header.byteRate = AUDIO_RATE * 1 * 16 / 8;
    header.blockAlign = 1 * 16 / 8;
    memcpy(header.dataChunkId, "data", 4);
    header.dataSize = 0; // Placeholder

    // Write the header to the start of the file.
    fwrite(&header, sizeof(WavHeader), 1, audio_file);

    // Main buffers for data handling.
    uint8_t buffer[BUFFER_SIZE];
    float freq_samples[BUFFER_SIZE / 2];
    float freq_samples_decimated[BUFFER_SIZE / (2 * DECIMATION_FACTOR)];

    int read_bytes = 0;
    float last_sample = 0.0f;
    int bytes_count = 0;
    int total_bytes = SAMPLE_RATE * audio_duration * 2;
    long total_audio_bytes = 0;
    while (bytes_count < total_bytes) {
        // Read BUFFER_SIZE IQ samples from SDR into buffer.
        int result = rtlsdr_read_sync(sdr, buffer, BUFFER_SIZE, &read_bytes); 
        if (result < 0) {
            fprintf(stderr, "An error occurred while reading IQ samples.\n");
            exit(1);
        }

        // FM signal handling.
        last_sample = demodulate(freq_samples, last_sample, buffer, BUFFER_SIZE);    
        int samples_to_write = decimate(freq_samples_decimated, freq_samples, BUFFER_SIZE / 2);
        
        // Frequency conversion into WAV data and actual write.
        int16_t *int_samples = malloc(sizeof(int16_t) * samples_to_write);        
        convert_samples(int_samples, freq_samples_decimated, samples_to_write);
        fwrite(int_samples, sizeof(int16_t), samples_to_write, audio_file);

        total_audio_bytes += samples_to_write * sizeof(int16_t);
        bytes_count += read_bytes;

        // Free the integer samples buffer at the end.
        free(int_samples);
    }
    
    // Move the pointer at the start of the audio file, as we have to update
    // the header to match the size of the audio file.
    fseek(audio_file, 0, SEEK_SET);

    header.chunkSize = 36 + total_audio_bytes; // Total file size - 8
    header.dataSize = total_audio_bytes;  // Just the audio data size

    fwrite(&header, sizeof(WavHeader), 1, audio_file);

    fclose(audio_file);
    rtlsdr_close(sdr);
    return 0; 
}
