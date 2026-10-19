
#define _CRT_SECURE_NO_WARNINGS
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <limits.h>
#include <math.h>
#include <windows.h>
#include <mmsystem.h>

#pragma comment(lib, "winmm.lib")

#define SAMPLE_RATE 44100
#define CHANNELS 2
#define BIT_SIZE 16
#define SAMPLE_MAX SHRT_MAX
#define PI 3.14159265358979323846

typedef short SampleType;

typedef struct {
    int sample_count;
    int file_channels;
    SampleType (*data)[CHANNELS];
} SoundData;

typedef int Note;
static const char* note_to_string[] = { "C%i", "C#%i", "D%i", "D#%i", "E%i", "F%i",
                                        "F#%i", "G%i", "G#%i", "A%i", "A#%i", "B%i" };

static void noteToString(Note note, char* str) {
    sprintf(str, note_to_string[note % 12], note / 12);
}

/* ---------- WAV loading ---------- */

static bool loadWavFromFile(FILE* file, SoundData* out) {
    char chunk_id[4], format[4];
    uint32_t chunk_size;
    uint16_t bits_per_sample = 0, num_channels = 0;
    bool have_fmt = false;

    if (fread(chunk_id, 1, 4, file) != 4 || fread(&chunk_size, 4, 1, file) != 1 ||
        fread(format, 1, 4, file) != 4)
        return false;
    if (memcmp(chunk_id, "RIFF", 4) != 0 || memcmp(format, "WAVE", 4) != 0)
        return false;

    char sub_id[4];
    uint32_t sub_size;
    while (fread(sub_id, 1, 4, file) == 4 && fread(&sub_size, 4, 1, file) == 1) {
        if (memcmp(sub_id, "fmt ", 4) == 0) {
            uint16_t audio_format, block_align;
            uint32_t sample_rate, byte_rate;
            fread(&audio_format, 2, 1, file);
            fread(&num_channels, 2, 1, file);
            fread(&sample_rate, 4, 1, file);
            fread(&byte_rate, 4, 1, file);
            fread(&block_align, 2, 1, file);
            fread(&bits_per_sample, 2, 1, file);
            if (sub_size > 16) fseek(file, sub_size - 16, SEEK_CUR);
            if (audio_format != 1 || bits_per_sample != 16 ||
                (num_channels != 1 && num_channels != 2))
                return false;               // only 16-bit PCM mono/stereo
            have_fmt = true;
        } else if (memcmp(sub_id, "data", 4) == 0) {
            if (!have_fmt) return false;
            int count = sub_size / 2 / num_channels;
            if (count <= 0) return false;
            SampleType (*buf)[CHANNELS] = malloc(sizeof(SampleType) * CHANNELS * count);
            if (!buf) return false;
            int got = 0;
            for (; got < count; got++) {
                int16_t l = 0, r = 0;
                if (fread(&l, 2, 1, file) != 1) break;
                if (num_channels == 2) {
                    if (fread(&r, 2, 1, file) != 1) break;
                } else r = l;
                buf[got][0] = l;
                buf[got][1] = r;
            }
            if (got <= 0) { free(buf); return false; }
            out->data = buf;
            out->sample_count = got;
            out->file_channels = num_channels;
            return true;
        } else {
            fseek(file, sub_size + (sub_size & 1), SEEK_CUR);
        }
    }
    return false;
}

static FILE* openSampleFile(Note note) {
    char exe_dir[MAX_PATH] = "";
    GetModuleFileNameA(NULL, exe_dir, MAX_PATH);
    char* slash = strrchr(exe_dir, '\\');
    if (slash) slash[1] = '\0';

    char dirs[3][MAX_PATH];
    snprintf(dirs[0], MAX_PATH, "%ssamples\\", exe_dir);
    snprintf(dirs[1], MAX_PATH, "samples\\");
    snprintf(dirs[2], MAX_PATH, "C:\\piano_project\\samples\\");

    const char* patterns[] = { "%ssample_%d.wav", "%ssample_%d", "%s%d.wav", "%s%d" };
    char name[2048];
    for (int d = 0; d < 3; d++) {
        for (int p = 0; p < 4; p++) {
            snprintf(name, sizeof(name), patterns[p], dirs[d], note);
            FILE* f = fopen(name, "rb");
            if (f) return f;
        }
    }
    return NULL;
}

/* ---------- fallback synth (used when no sample file exists) ---------- */

static SoundData synthNote(Note note) {
    SoundData sd = { 0, 2, NULL };
    double freq = 16.3516 * pow(2.0, note / 12.0);   // note 57 == A4 == 440 Hz
    int n = SAMPLE_RATE * 3;
    sd.data = malloc(sizeof(SampleType) * CHANNELS * n);
    if (!sd.data) { sd.sample_count = 0; return sd; }
    sd.sample_count = n;
    for (int i = 0; i < n; i++) {
        double t = (double)i / SAMPLE_RATE;
        double attack = t < 0.005 ? t / 0.005 : 1.0;
        double env = exp(-t * 1.6) * attack;
        double v = sin(2 * PI * freq * t)
                 + 0.50 * sin(4 * PI * freq * t) * exp(-t * 2.0)
                 + 0.25 * sin(6 * PI * freq * t) * exp(-t * 3.0)
                 + 0.12 * sin(8 * PI * freq * t) * exp(-t * 4.0);
        SampleType s = (SampleType)(v / 1.9 * env * 16000.0);
        sd.data[i][0] = s;
        sd.data[i][1] = s;
    }
    return sd;
}

static int synthesized_count = 0;

static SoundData loadSoundForNote(Note note) {
    SoundData sd = { 0, 2, NULL };
    FILE* f = openSampleFile(note);
    if (f) {
        bool ok = loadWavFromFile(f, &sd);
        fclose(f);
        if (ok) return sd;
    }
    synthesized_count++;
    return synthNote(note);
}

/* ---------- piano state ---------- */

typedef struct {
    volatile bool pressed;
    volatile bool fading;
    volatile int sample_position;
    volatile int fading_count;
} PlayingState;

typedef struct {
    SoundData sound_data;
    PlayingState playing_state;
} PianoString;

#define BASE_NOTE 9
#define NUMBER_OF_NOTES 84
static PianoString piano[NUMBER_OF_NOTES];

static void initPiano(void) {
    for (int i = 0; i < NUMBER_OF_NOTES; i++)
        piano[i].sound_data = loadSoundForNote(BASE_NOTE + i);
}

static void deinitPiano(void) {
    for (int i = 0; i < NUMBER_OF_NOTES; i++)
        free(piano[i].sound_data.data);
}

static void pressPianoKey(Note note) {
    if (note >= BASE_NOTE && note < NUMBER_OF_NOTES + BASE_NOTE) {
        PianoString* p = &piano[note - BASE_NOTE];
        p->playing_state.sample_position = 0;
        p->playing_state.fading = false;
        p->playing_state.pressed = true;
    }
}

static void depressPianoKey(Note note) {
    if (note >= BASE_NOTE && note < NUMBER_OF_NOTES + BASE_NOTE) {
        PianoString* p = &piano[note - BASE_NOTE];
        if (p->playing_state.pressed) {
            p->playing_state.fading_count = 0;
            p->playing_state.fading = true;
            p->playing_state.pressed = false;
        }
    }
}

static void printPianoState(void) {
    printf("\r\x1b[KActive Notes: ");
    char buf[16];
    for (int i = 0; i < NUMBER_OF_NOTES; i++) {
        if (piano[i].playing_state.pressed) {
            noteToString(i + BASE_NOTE, buf);
            printf("%s ", buf);
        }
    }
    fflush(stdout);
}

/* ---------- audio ---------- */

#define BUFFER_SIZE 1024      /* frames per buffer (~23 ms) */
#define NUM_BUFFERS 4
static HWAVEOUT hWaveOut;
static HANDLE hAudioThread;
static WAVEHDR waveHeaders[NUM_BUFFERS];
static short bufferData[NUM_BUFFERS][BUFFER_SIZE * CHANNELS];
static volatile bool audioRunning = true;

static void mixAudio(short* out, int frame_count) {
    for (int s = 0; s < frame_count; s++) {
        float outL = 0.0f, outR = 0.0f;
        for (int ps = 0; ps < NUMBER_OF_NOTES; ps++) {
            PianoString* p = &piano[ps];
            if (p->sound_data.sample_count <= 1) continue;
            int pos = p->playing_state.sample_position;

            if (p->playing_state.pressed) {
                if (pos < p->sound_data.sample_count) {
                    outL += p->sound_data.data[pos][0] / 2.0f;
                    outR += p->sound_data.data[pos][1] / 2.0f;
                    p->playing_state.sample_position = pos + 1;
                } else {
                    p->playing_state.pressed = false;
                }
            } else if (p->playing_state.fading) {
                int fc = p->playing_state.fading_count;
                if (pos < p->sound_data.sample_count && fc < 44100) {
                    fc++;
                    float mult = 1.0f / (1.0f + (float)fc / 3000.0f);
                    outL += p->sound_data.data[pos][0] / 2.0f * mult;
                    outR += p->sound_data.data[pos][1] / 2.0f * mult;
                    p->playing_state.fading_count = fc;
                    p->playing_state.sample_position = pos + 1;
                } else {
                    p->playing_state.fading = false;
                }
            }
        }
        if (outL > SAMPLE_MAX) outL = SAMPLE_MAX;
        if (outL < -SAMPLE_MAX) outL = -SAMPLE_MAX;
        if (outR > SAMPLE_MAX) outR = SAMPLE_MAX;
        if (outR < -SAMPLE_MAX) outR = -SAMPLE_MAX;
        out[s * 2]     = (short)outL;
        out[s * 2 + 1] = (short)outR;
    }
}

static DWORD WINAPI AudioThread(LPVOID param) {
    (void)param;
    int cur = 0;
    while (audioRunning) {
        WAVEHDR* hdr = &waveHeaders[cur];
        if ((hdr->dwFlags & WHDR_DONE) || !(hdr->dwFlags & WHDR_PREPARED)) {
            if (hdr->dwFlags & WHDR_PREPARED)
                waveOutUnprepareHeader(hWaveOut, hdr, sizeof(WAVEHDR));
            mixAudio(bufferData[cur], BUFFER_SIZE);
            memset(hdr, 0, sizeof(WAVEHDR));
            hdr->lpData = (LPSTR)bufferData[cur];
            hdr->dwBufferLength = BUFFER_SIZE * CHANNELS * sizeof(short);
            waveOutPrepareHeader(hWaveOut, hdr, sizeof(WAVEHDR));
            waveOutWrite(hWaveOut, hdr, sizeof(WAVEHDR));
            cur = (cur + 1) % NUM_BUFFERS;
        } else {
            Sleep(2);
        }
    }
    return 0;
}

static bool initPianoAudio(void) {
    WAVEFORMATEX wfx = { 0 };
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = CHANNELS;
    wfx.nSamplesPerSec = SAMPLE_RATE;
    wfx.wBitsPerSample = BIT_SIZE;
    wfx.nBlockAlign = CHANNELS * BIT_SIZE / 8;
    wfx.nAvgBytesPerSec = SAMPLE_RATE * wfx.nBlockAlign;

    if (waveOutOpen(&hWaveOut, WAVE_MAPPER, &wfx, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
        fprintf(stderr, "Could not open audio output device.\n");
        return false;
    }
    memset(waveHeaders, 0, sizeof(waveHeaders));
    hAudioThread = CreateThread(NULL, 0, AudioThread, NULL, 0, NULL);
    return hAudioThread != NULL;
}

static void deinitPianoAudio(void) {
    audioRunning = false;
    if (hAudioThread) {
        WaitForSingleObject(hAudioThread, 1000);
        CloseHandle(hAudioThread);
    }
    waveOutReset(hWaveOut);
    for (int i = 0; i < NUM_BUFFERS; i++)
        if (waveHeaders[i].dwFlags & WHDR_PREPARED)
            waveOutUnprepareHeader(hWaveOut, &waveHeaders[i], sizeof(WAVEHDR));
    waveOutClose(hWaveOut);
}

/* ---------- keyboard ---------- */

#define MIDDLE_C 48   /* C4 */

typedef struct { int vk; int note; } KeyMap;
static const KeyMap keys[] = {
    { 'A', MIDDLE_C + 0 },  { 'W', MIDDLE_C + 1 },  { 'S', MIDDLE_C + 2 },
    { 'E', MIDDLE_C + 3 },  { 'D', MIDDLE_C + 4 },  { 'F', MIDDLE_C + 5 },
    { 'T', MIDDLE_C + 6 },  { 'G', MIDDLE_C + 7 },  { 'Y', MIDDLE_C + 8 },
    { 'H', MIDDLE_C + 9 },  { 'U', MIDDLE_C + 10 }, { 'J', MIDDLE_C + 11 },
    { 'K', MIDDLE_C + 12 }, { 'O', MIDDLE_C + 13 }, { 'L', MIDDLE_C + 14 },
    { 'P', MIDDLE_C + 15 }, { VK_OEM_1, MIDDLE_C + 16 }   /* ; */
};
#define NUM_KEYS ((int)(sizeof(keys) / sizeof(keys[0])))
static bool key_states[NUM_KEYS];

static void reactToKeyEvents(int output) {
    printf("\nPlaying. Press ESC to quit.\n");
    while (!(GetAsyncKeyState(VK_ESCAPE) & 0x8000)) {
        bool changed = false;
        for (int i = 0; i < NUM_KEYS; i++) {
            bool down = (GetAsyncKeyState(keys[i].vk) & 0x8000) != 0;
            if (down && !key_states[i]) {
                key_states[i] = true;
                pressPianoKey(keys[i].note);
                changed = true;
            } else if (!down && key_states[i]) {
                key_states[i] = false;
                depressPianoKey(keys[i].note);
                changed = true;
            }
        }
        if (changed && output == 1) printPianoState();
        Sleep(5);
    }
}

int main(void) {
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (GetConsoleMode(hOut, &mode))
        SetConsoleMode(hOut, mode | 0x0004 /* ENABLE_VIRTUAL_TERMINAL_PROCESSING */);

    printf("Loading notes...\n");
    initPiano();
    if (synthesized_count > 0)
        printf("%d of %d notes had no sample file and use a synthesized tone.\n",
               synthesized_count, NUMBER_OF_NOTES);

    printf("\n   W E   T Y U   O P\n  A S D F G H J K L ;\n  (A = middle C)\n");

    if (!initPianoAudio()) { deinitPiano(); return EXIT_FAILURE; }
    reactToKeyEvents(1);
    deinitPianoAudio();
    deinitPiano();
    return EXIT_SUCCESS;
}
