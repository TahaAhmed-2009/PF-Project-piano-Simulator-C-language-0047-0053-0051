// Build (MinGW):  gcc win_piano.c -o win_piano.exe -lwinmm
// Build (MSVC):   cl win_piano.c
//
// Usage:  win_piano.exe [samples_folder] [root_note_for_unnamed_file, e.g. C4]
//
// SAMPLES
//   Put your .wav files in a "samples" folder next to the exe (or pass a folder
//   as the first argument). Note names are detected from the file names:
//     C4.wav, piano_C#4.wav, Db3.wav, Steinway-A2-mf.wav   (C4 = middle C)
//     sample_48.wav, 48.wav                                (48 = C4)
//   Notes you don't have a file for are made by pitch-shifting the nearest file,
//   so even ONE sample gives you every key. 8/16/24/32-bit and float WAVs work.
//   A single file with no note in its name is treated as C4 (or the note given
//   as the second argument).
//
// KEYS (tracker-style, two rows)
//   Lower row:  Z S X D C V G B H N J M , L . ; /     (Z = C of the current octave)
//   Upper row:  Q 2 W 3 E R 5 T 6 Y 7 U I 9 O 0 P     (Q = C one octave higher)
//   UP / DOWN arrows: change octave      ESC: quit

#define _CRT_SECURE_NO_WARNINGS
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
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
    int sample_rate;
    SampleType (*data)[CHANNELS];
} SoundData;

typedef int Note;   /* C0 = 0, C4 = 48 (middle C), A4 = 57 = 440 Hz */
static const char* note_to_string[] = { "C%i", "C#%i", "D%i", "D#%i", "E%i", "F%i",
                                        "F#%i", "G%i", "G#%i", "A%i", "A#%i", "B%i" };

static void noteToString(Note note, char* str) {
    sprintf(str, note_to_string[note % 12], note / 12);
}

/* ---------------------------------------------------------------- WAV ---- */

static int16_t convertSample(const uint8_t* p, int bits, int fmt) {
    if (fmt == 3 && bits == 32) {                       /* 32-bit float */
        float f; memcpy(&f, p, 4);
        if (f > 1.0f) f = 1.0f;
        if (f < -1.0f) f = -1.0f;
        return (int16_t)(f * 32767.0f);
    }
    switch (bits) {
        case 8:  return (int16_t)(((int)p[0] - 128) << 8);
        case 16: return (int16_t)(p[0] | (p[1] << 8));
        case 24: return (int16_t)(p[1] | (p[2] << 8));
        case 32: return (int16_t)(p[2] | (p[3] << 8));
    }
    return 0;
}

static bool loadWavFromFile(FILE* file, SoundData* out) {
    char riff[4], wave[4];
    uint32_t riff_size;
    if (fread(riff, 1, 4, file) != 4 || fread(&riff_size, 4, 1, file) != 1 ||
        fread(wave, 1, 4, file) != 4)
        return false;
    if (memcmp(riff, "RIFF", 4) != 0 || memcmp(wave, "WAVE", 4) != 0) return false;

    uint16_t fmt = 0, channels = 0, bits = 0;
    uint32_t rate = 0;
    bool have_fmt = false;
    char id[4];
    uint32_t size;

    while (fread(id, 1, 4, file) == 4 && fread(&size, 4, 1, file) == 1) {
        long start = ftell(file);
        if (memcmp(id, "fmt ", 4) == 0) {
            uint16_t block_align; uint32_t byte_rate;
            fread(&fmt, 2, 1, file);
            fread(&channels, 2, 1, file);
            fread(&rate, 4, 1, file);
            fread(&byte_rate, 4, 1, file);
            fread(&block_align, 2, 1, file);
            fread(&bits, 2, 1, file);
            if (fmt == 0xFFFE && size >= 40) {          /* WAVE_FORMAT_EXTENSIBLE */
                uint16_t cb, valid; uint32_t mask; uint16_t sub;
                fread(&cb, 2, 1, file);
                fread(&valid, 2, 1, file);
                fread(&mask, 4, 1, file);
                fread(&sub, 2, 1, file);
                fmt = sub;
            }
            if ((fmt != 1 && fmt != 3) || channels < 1 || channels > 8 ||
                (bits != 8 && bits != 16 && bits != 24 && bits != 32) || rate == 0)
                return false;
            have_fmt = true;
        } else if (memcmp(id, "data", 4) == 0) {
            if (!have_fmt) return false;
            fseek(file, 0, SEEK_END);
            long end = ftell(file);
            fseek(file, start, SEEK_SET);
            if (size > (uint32_t)(end - start)) size = (uint32_t)(end - start);

            int bps = bits / 8;
            int frame_bytes = bps * channels;
            int count = (int)(size / frame_bytes);
            if (count <= 1) return false;

            uint8_t* raw = malloc((size_t)count * frame_bytes);
            SampleType (*buf)[CHANNELS] = malloc(sizeof(SampleType) * CHANNELS * count);
            if (!raw || !buf) { free(raw); free(buf); return false; }
            size_t got = fread(raw, frame_bytes, count, file);
            for (size_t i = 0; i < got; i++) {
                const uint8_t* fr = raw + i * frame_bytes;
                int16_t l = convertSample(fr, bits, fmt);
                int16_t r = channels > 1 ? convertSample(fr + bps, bits, fmt) : l;
                buf[i][0] = l;
                buf[i][1] = r;
            }
            free(raw);
            if (got <= 1) { free(buf); return false; }
            out->data = buf;
            out->sample_count = (int)got;
            out->sample_rate = (int)rate;
            return true;
        }
        fseek(file, start + size + (size & 1), SEEK_SET);
    }
    return false;
}

/* Find a note in a file name. Returns -1 if none. */
static int parseNoteFromName(const char* path) {
    const char* base = path;
    const char* s;
    if ((s = strrchr(base, '\\'))) base = s + 1;
    if ((s = strrchr(base, '/')))  base = s + 1;
    char stem[260];
    strncpy(stem, base, sizeof(stem) - 1);
    stem[sizeof(stem) - 1] = '\0';
    char* dot = strrchr(stem, '.');
    if (dot) *dot = '\0';

    /* plain number, optionally "sample_" prefix */
    const char* q = stem;
    if (strncmp(q, "sample_", 7) == 0) q += 7;
    bool digits = (*q != '\0');
    for (const char* c = q; *c; c++) if (!isdigit((unsigned char)*c)) digits = false;
    if (digits) return atoi(q);

    static const int semis[7] = { 9, 11, 0, 2, 4, 5, 7 };   /* A B C D E F G */
    int result = -1;
    for (int i = 0; stem[i]; i++) {
        char c = stem[i];
        if (c < 'A' || c > 'G') continue;
        int j = i + 1, acc = 0;
        if (stem[j] == '#') { acc = 1; j++; }
        else if (stem[j] == 'b') { acc = -1; j++; }
        if (!isdigit((unsigned char)stem[j])) continue;
        int n = (stem[j] - '0') * 12 + semis[c - 'A'] + acc;
        if (n >= 0) result = n;                              /* keep the last match */
    }
    return result;
}

/* ------------------------------------------------- loading + pitch-shift -- */

#define BASE_NOTE 9
#define NUMBER_OF_NOTES 84
#define MAX_LOADED 512

typedef struct { int note; SoundData sd; bool used; } Loaded;
static Loaded loaded[MAX_LOADED];
static int loaded_count = 0;
static char loaded_dir[MAX_PATH] = "";

static bool loadFile(const char* path, int note) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    SoundData sd = { 0, SAMPLE_RATE, NULL };
    bool ok = loadWavFromFile(f, &sd);
    fclose(f);
    if (!ok) return false;
    for (int i = 0; i < loaded_count; i++)          /* duplicate note: keep first */
        if (loaded[i].note == note) { free(sd.data); return true; }
    if (loaded_count >= MAX_LOADED) { free(sd.data); return false; }
    loaded[loaded_count].note = note;
    loaded[loaded_count].sd = sd;
    loaded[loaded_count].used = false;
    loaded_count++;
    return true;
}

static int scanDirectory(const char* dir, const char* root_override) {
    char pattern[MAX_PATH + 8];
    snprintf(pattern, sizeof(pattern), "%s\\*.wav", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;

    char unnamed[16][MAX_PATH];
    int unnamed_count = 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        char full[MAX_PATH * 2];
        snprintf(full, sizeof(full), "%s\\%s", dir, fd.cFileName);
        int note = parseNoteFromName(fd.cFileName);
        if (note >= 0) loadFile(full, note);
        else if (unnamed_count < 16) strncpy(unnamed[unnamed_count++], full, MAX_PATH - 1);
    } while (FindNextFileA(h, &fd));
    FindClose(h);

    if (loaded_count == 0 && unnamed_count > 0) {
        int root = 48;                                   /* C4 */
        if (root_override) {
            int n = parseNoteFromName(root_override);
            if (n >= 0) root = n;
        }
        char nm[16]; noteToString(root, nm);
        for (int i = 0; i < unnamed_count; i++) {
            if (loadFile(unnamed[i], root)) {
                printf("File names have no note in them; using %s as %s.\n", unnamed[i], nm);
                break;
            }
        }
    }
    if (loaded_count > 0) strncpy(loaded_dir, dir, MAX_PATH - 1);
    return loaded_count;
}

static SoundData resampleSound(const SoundData* src, int src_note, int dst_note) {
    SoundData out = { 0, SAMPLE_RATE, NULL };
    double r = ((double)src->sample_rate / SAMPLE_RATE) * pow(2.0, (dst_note - src_note) / 12.0);
    int n = (int)((src->sample_count - 1) / r);
    if (n < 2) n = 2;
    out.data = malloc(sizeof(SampleType) * CHANNELS * n);
    if (!out.data) return out;
    out.sample_count = n;
    for (int i = 0; i < n; i++) {
        double pos = i * r;
        int i0 = (int)pos;
        double fr = pos - i0;
        int i1 = i0 + 1;
        if (i0 > src->sample_count - 1) i0 = src->sample_count - 1;
        if (i1 > src->sample_count - 1) i1 = src->sample_count - 1;
        for (int ch = 0; ch < CHANNELS; ch++) {
            double a = src->data[i0][ch], b = src->data[i1][ch];
            out.data[i][ch] = (SampleType)(a + (b - a) * fr);
        }
    }
    return out;
}

/* fallback tone when no wav files are found at all */
static SoundData synthNote(Note note) {
    SoundData sd = { 0, SAMPLE_RATE, NULL };
    double freq = 16.3516 * pow(2.0, note / 12.0);
    int n = SAMPLE_RATE * 3;
    sd.data = malloc(sizeof(SampleType) * CHANNELS * n);
    if (!sd.data) return sd;
    sd.sample_count = n;
    for (int i = 0; i < n; i++) {
        double t = (double)i / SAMPLE_RATE;
        double env = exp(-t * 1.6) * (t < 0.005 ? t / 0.005 : 1.0);
        double v = sin(2 * PI * freq * t) + 0.50 * sin(4 * PI * freq * t) * exp(-t * 2.0)
                 + 0.25 * sin(6 * PI * freq * t) * exp(-t * 3.0);
        SampleType s = (SampleType)(v / 1.75 * env * 16000.0);
        sd.data[i][0] = s;
        sd.data[i][1] = s;
    }
    return sd;
}

/* ---------------------------------------------------------- piano state -- */

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

static PianoString piano[NUMBER_OF_NOTES];

static void initPiano(const char* folder_arg, const char* root_arg) {
    char exe_dir[MAX_PATH] = "";
    GetModuleFileNameA(NULL, exe_dir, MAX_PATH);
    char* slash = strrchr(exe_dir, '\\');
    if (slash) *slash = '\0';

    char candidates[6][MAX_PATH];
    int nc = 0;
    if (folder_arg) strncpy(candidates[nc++], folder_arg, MAX_PATH - 1);
    snprintf(candidates[nc++], MAX_PATH, "%s\\samples", exe_dir);
    strcpy(candidates[nc++], ".\\samples");
    strcpy(candidates[nc++], "C:\\piano_project\\samples");
    strncpy(candidates[nc++], exe_dir, MAX_PATH - 1);
    strcpy(candidates[nc++], ".");
    for (int i = 0; i < nc; i++) {
        size_t len = strlen(candidates[i]);
        if (len > 1 && (candidates[i][len - 1] == '\\' || candidates[i][len - 1] == '/'))
            candidates[i][len - 1] = '\0';
        if (scanDirectory(candidates[i], root_arg)) break;
    }

    if (loaded_count == 0) {
        printf("No usable .wav files found - using built-in synthesized tones.\n");
        for (int i = 0; i < NUMBER_OF_NOTES; i++)
            piano[i].sound_data = synthNote(BASE_NOTE + i);
        return;
    }

    int exact = 0, shifted = 0, lo = INT_MAX, hi = -1;
    for (int i = 0; i < loaded_count; i++) {
        if (loaded[i].note < lo) lo = loaded[i].note;
        if (loaded[i].note > hi) hi = loaded[i].note;
    }
    for (int i = 0; i < NUMBER_OF_NOTES; i++) {
        int note = BASE_NOTE + i;
        int best = 0, bestd = INT_MAX;
        for (int k = 0; k < loaded_count; k++) {
            int d = abs(loaded[k].note - note);
            if (d < bestd) { bestd = d; best = k; }
        }
        Loaded* L = &loaded[best];
        if (bestd == 0 && L->sd.sample_rate == SAMPLE_RATE) {
            piano[i].sound_data = L->sd;                 /* use as is */
            L->used = true;
            exact++;
        } else {
            piano[i].sound_data = resampleSound(&L->sd, L->note, note);
            if (bestd == 0) exact++; else shifted++;
        }
    }
    for (int k = 0; k < loaded_count; k++)
        if (!loaded[k].used) free(loaded[k].sd.data);

    char a[16], b[16];
    noteToString(lo, a); noteToString(hi, b);
    printf("Loaded %d sample file(s) from %s  (%s .. %s)\n", loaded_count, loaded_dir, a, b);
    printf("%d notes use your samples directly, %d are pitch-shifted from the nearest one.\n",
           exact, shifted);
}

static void deinitPiano(void) {
    for (int i = 0; i < NUMBER_OF_NOTES; i++) free(piano[i].sound_data.data);
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

static int octave = 3;

static void printPianoState(void) {
    printf("\r\x1b[KOctave %d | Active notes: ", octave);
    char buf[16];
    for (int i = 0; i < NUMBER_OF_NOTES; i++) {
        if (piano[i].playing_state.pressed) {
            noteToString(i + BASE_NOTE, buf);
            printf("%s ", buf);
        }
    }
    fflush(stdout);
}

/* ---------------------------------------------------------------- audio -- */

#define BUFFER_SIZE 1024
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

/* ------------------------------------------------------------- keyboard -- */

typedef struct { int vk; int semi; } KeyMap;   /* semitones above C of current octave */
static const KeyMap keys[] = {
    /* lower row */
    { 'Z', 0 }, { 'S', 1 }, { 'X', 2 }, { 'D', 3 }, { 'C', 4 }, { 'V', 5 }, { 'G', 6 },
    { 'B', 7 }, { 'H', 8 }, { 'N', 9 }, { 'J', 10 }, { 'M', 11 }, { VK_OEM_COMMA, 12 },
    { 'L', 13 }, { VK_OEM_PERIOD, 14 }, { VK_OEM_1, 15 }, { VK_OEM_2, 16 },
    /* upper row */
    { 'Q', 12 }, { '2', 13 }, { 'W', 14 }, { '3', 15 }, { 'E', 16 }, { 'R', 17 }, { '5', 18 },
    { 'T', 19 }, { '6', 20 }, { 'Y', 21 }, { '7', 22 }, { 'U', 23 }, { 'I', 24 },
    { '9', 25 }, { 'O', 26 }, { '0', 27 }, { 'P', 28 }
};
#define NUM_KEYS ((int)(sizeof(keys) / sizeof(keys[0])))
static bool key_states[NUM_KEYS];
static int key_notes[NUM_KEYS];     /* note each held key started, so release matches */

static void reactToKeyEvents(void) {
    bool up_prev = false, down_prev = false;
    printf("\nPlaying. UP/DOWN = octave, ESC = quit.\n");
    printPianoState();
    while (!(GetAsyncKeyState(VK_ESCAPE) & 0x8000)) {
        bool changed = false;

        bool up = (GetAsyncKeyState(VK_UP) & 0x8000) != 0;
        bool down = (GetAsyncKeyState(VK_DOWN) & 0x8000) != 0;
        if (up && !up_prev && octave < 6) { octave++; changed = true; }
        if (down && !down_prev && octave > 0) { octave--; changed = true; }
        up_prev = up; down_prev = down;

        for (int i = 0; i < NUM_KEYS; i++) {
            bool is_down = (GetAsyncKeyState(keys[i].vk) & 0x8000) != 0;
            if (is_down && !key_states[i]) {
                key_states[i] = true;
                key_notes[i] = octave * 12 + keys[i].semi;
                pressPianoKey(key_notes[i]);
                changed = true;
            } else if (!is_down && key_states[i]) {
                key_states[i] = false;
                depressPianoKey(key_notes[i]);
                changed = true;
            }
        }
        if (changed) printPianoState();
        Sleep(5);
    }
    printf("\n");
}

int main(int argc, char** argv) {
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (GetConsoleMode(hOut, &mode))
        SetConsoleMode(hOut, mode | 0x0004 /* ENABLE_VIRTUAL_TERMINAL_PROCESSING */);

    printf("Loading samples...\n");
    initPiano(argc > 1 ? argv[1] : NULL, argc > 2 ? argv[2] : NULL);

    if (!initPianoAudio()) { deinitPiano(); return EXIT_FAILURE; }
    reactToKeyEvents();
    deinitPianoAudio();
    deinitPiano();
    return EXIT_SUCCESS;
}
