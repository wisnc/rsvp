#include <M5Cardputer.h>
#include <M5UnitScroll.h>
#include <SD.h>
#include <SPI.h>
#include <vector>

#define SD_SCK  40
#define SD_MISO 39
#define SD_MOSI 14
#define SD_CS   12

#define GROVE_SDA  2
#define GROVE_SCL  1

#define SCR_W 240
#define SCR_H 135

#define COL_BG     0x0000
#define COL_TEXT   0xFFFF
#define COL_ORP    0xFD20
#define COL_DIM    0x4208
#define COL_PREV   0x2104
#define COL_SELBG  0x1082

#define BRD        2
#define BY        10
#define BH        (SCR_H - BY * 2)

#define BASE_WPM      175
#define MIN_WPM        50
#define MAX_WPM       800
#define WPM_STEP       25
#define CHUNK_BYTES  4096
#define SAVE_INTERVAL  30
#define RECAP_WORDS    20
#define RECAP_BYTES   512

#define FONT_SCALE  3
#define CHAR_W      (6 * FONT_SCALE)
#define CHAR_H      (8 * FONT_SCALE)

#define PB_H  4
#define PB_X  6
#define PB_W  (SCR_W - PB_X * 2)
#define PB_Y  (BY + BH - BRD - 8 - PB_H)

#define WY    (BY + BRD + 6)
#define WH    (PB_Y - WY - 2)

#define BRIGHT_STEP  25
#define MIN_BRIGHT   10
#define MAX_BRIGHT   255

enum State { MENU, READING };

struct Book { String title; String dir; };
struct Word { String text; int pos; };

static State             gState = MENU;
static std::vector<Book> gBooks;
static int               gSel = 0, gScroll = 0;

static File              gFile;
static int               gFileSize = 0;
static int               gCharOff  = 0;
static int               gResumeOff = 0;
static std::vector<Word> gWords;
static int               gWIdx     = 0;
static int               gWpm      = BASE_WPM;
static unsigned long     gLastMs   = 0;
static unsigned long     gDelay    = 0;
static bool              gPlaying  = false;
static int               gSaveCount = 0;
static int               gBrightness = 128;

static SPIClass          gSdSpi(HSPI);
static M5UnitScroll      gEncoder;
static bool              gEncoderOk   = false;
static bool              gBtnPrev     = false;
static int32_t           gEncPrev     = 0;

void scanBooks();
void drawMenu();
void drawTitle();
void openBook();
void loadChunk();
void parseWords(const char* buf, int n, int baseOffset, std::vector<Word>& out);
void drawFrame();
void drawWord(int idx);
void drawInlineTop();
void drawInlineBot();
void drawProgressBar();
void showCurrentWord();
void saveProgress();
int  loadProgress(const String& dir);
unsigned long wordDelay(const String& w);
void seekToWordBoundary();
void advanceWord();
void retreatWord();


void setup() {
    auto cfg = M5.config();
    M5Cardputer.begin(cfg, true);

    M5Cardputer.Display.setRotation(1);
    M5Cardputer.Display.setBrightness(gBrightness);
    M5Cardputer.Display.fillScreen(COL_BG);
    M5Cardputer.Display.setTextColor(COL_TEXT);
    M5Cardputer.Display.setTextSize(1);
    M5Cardputer.Display.setTextWrap(false);

    Wire.begin(GROVE_SDA, GROVE_SCL);
    gEncoderOk = gEncoder.begin(&Wire, SCROLL_ADDR, GROVE_SDA, GROVE_SCL, 400000U);
    if (gEncoderOk) {
        gEncPrev = gEncoder.getEncoderValue();
        gEncoder.setLEDColor(0x000000);
    }

    gSdSpi.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
    if (!SD.begin(SD_CS, gSdSpi, 25000000)) {
        M5Cardputer.Display.setCursor(10, 50);
        M5Cardputer.Display.print("SD card not found");
        while (true) delay(1000);
    }

    scanBooks();

    if (gBooks.empty()) {
        M5Cardputer.Display.setCursor(10, 40);
        M5Cardputer.Display.print("No books in /ebooks/");
        M5Cardputer.Display.setCursor(10, 60);
        M5Cardputer.Display.print("/ebooks/<title>/read.txt");
        while (true) delay(1000);
    }

    drawMenu();
}


void loop() {
    M5Cardputer.update();

    if (gState == MENU) {
        if (!M5Cardputer.Keyboard.isChange() || !M5Cardputer.Keyboard.isPressed()) return;
        auto ks = M5Cardputer.Keyboard.keysState();

        bool changed = false;
        for (auto k : ks.word) {
            if (k == ';' || k == ',') {
                if (gSel > 0) { gSel--; changed = true; }
                if (gSel < gScroll) gScroll = gSel;
            } else if (k == '.' || k == '/') {
                if (gSel < (int)gBooks.size() - 1) { gSel++; changed = true; }
                if (gSel >= gScroll + 5) gScroll++;
            }
        }
        if (ks.enter) { openBook(); return; }
        if (changed) drawMenu();

    } else {
        if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
            auto ks = M5Cardputer.Keyboard.keysState();

            bool wantsExit = ks.del;
            for (auto k : ks.word) if (k == '`') wantsExit = true;
            if (wantsExit) {
                gPlaying = false;
                saveProgress();
                gFile.close();
                gState = MENU;
                drawMenu();
                return;
            }

            if (ks.space) {
                gPlaying = !gPlaying;
                if (gPlaying) gLastMs = millis();
            }

            for (auto k : ks.word) {
                if (k == '/') advanceWord();
                else if (k == ',') retreatWord();
                else if (k == ';') {
                    if (gWpm < MAX_WPM) { gWpm += WPM_STEP; drawInlineBot(); }
                }
                else if (k == '.') {
                    if (gWpm > MIN_WPM) { gWpm -= WPM_STEP; drawInlineBot(); }
                }
                else if (k == '=' || k == '+') {
                    if (gBrightness < MAX_BRIGHT) {
                        gBrightness = min(gBrightness + BRIGHT_STEP, MAX_BRIGHT);
                        M5Cardputer.Display.setBrightness(gBrightness);
                    }
                }
                else if (k == '-') {
                    if (gBrightness > MIN_BRIGHT) {
                        gBrightness = max(gBrightness - BRIGHT_STEP, MIN_BRIGHT);
                        M5Cardputer.Display.setBrightness(gBrightness);
                    }
                }
            }
        }

        if (gEncoderOk) {
            int32_t encNow = gEncoder.getEncoderValue();
            int32_t delta  = encNow - gEncPrev;
            if (delta != 0) {
                gEncPrev = encNow;
                if (delta > 0) advanceWord();
                else           retreatWord();
            }

            bool btnNow = gEncoder.getButtonStatus();
            if (btnNow && !gBtnPrev) {
                gPlaying = !gPlaying;
                if (gPlaying) gLastMs = millis();
            }
            gBtnPrev = btnNow;
        }

        if (!gPlaying) return;
        if (millis() - gLastMs < gDelay) return;

        if (gWIdx >= (int)gWords.size()) {
            loadChunk();
            if (gWords.empty()) {
                gPlaying = false;
                auto& d = M5Cardputer.Display;
                d.fillRect(0, WY, SCR_W, WH, COL_BG);
                d.setTextSize(2);
                d.setTextColor(COL_DIM);
                d.setCursor(85, SCR_H / 2 - 8);
                d.print("- end -");
                saveProgress();
                return;
            }
        }

        showCurrentWord();
        gWIdx++;
        gSaveCount++;
        if (gSaveCount >= SAVE_INTERVAL) { saveProgress(); gSaveCount = 0; }
    }
}


void advanceWord() {
    if (gWIdx >= (int)gWords.size()) {
        loadChunk();
        if (gWords.empty()) return;
    }
    showCurrentWord();
    gWIdx++;
    gLastMs = millis();
}

void retreatWord() {
    if (gWIdx > 1) {
        gWIdx -= 2;
        showCurrentWord();
        gWIdx++;
        gLastMs = millis();
    }
}

void showCurrentWord() {
    Word& cw = gWords[gWIdx];
    gCharOff = cw.pos + cw.text.length();
    drawWord(gWIdx);
    gDelay   = wordDelay(cw.text);
    gLastMs  = millis();
}


void scanBooks() {
    File root = SD.open("/ebooks");
    if (!root || !root.isDirectory()) return;
    File entry;
    while ((entry = root.openNextFile())) {
        if (entry.isDirectory()) {
            String fullPath = String(entry.name());
            int lastSlash = fullPath.lastIndexOf('/');
            String title = (lastSlash >= 0) ? fullPath.substring(lastSlash + 1) : fullPath;
            String dir = "/ebooks/" + title;
            if (SD.exists((dir + "/read.txt").c_str()))
                gBooks.push_back({title, dir});
        }
        entry.close();
    }
    root.close();
}


void drawTitle() {
    auto& d = M5Cardputer.Display;
    const char* title = "Rapid Serial Visual Presentation";
    int x = 6;
    int baseY = 4;

    for (int i = 0; title[i] != '\0'; i++) {
        char c = title[i];
        if (c >= 'A' && c <= 'Z') {
            d.setTextSize(2);
            d.setTextColor(COL_ORP);
            d.setCursor(x, baseY);
            d.print(c);
            x += 12;
        } else {
            d.setTextSize(1);
            d.setTextColor(COL_PREV);
            d.setCursor(x, baseY + 8);
            d.print(c);
            x += 6;
        }
    }
}

void drawMenu() {
    auto& d = M5Cardputer.Display;
    d.fillScreen(COL_BG);

    drawTitle();
    d.drawFastHLine(6, 24, SCR_W - 12, COL_DIM);

    int maxVis = 5, itemH = 20, y = 30;
    d.setTextSize(1);
    for (int i = gScroll; i < min((int)gBooks.size(), gScroll + maxVis); i++) {
        if (i == gSel) {
            d.fillRect(6, y, SCR_W - 12, itemH, COL_SELBG);
            d.setTextColor(COL_ORP);
        } else {
            d.setTextColor(COL_TEXT);
        }
        String label = gBooks[i].title;
        if (label.length() > 34) label = label.substring(0, 31) + "...";
        d.setCursor(14, y + 6);
        d.print(label);
        y += itemH;
    }
    d.setTextColor(COL_TEXT);
}


void openBook() {
    String path = gBooks[gSel].dir + "/read.txt";
    gFile = SD.open(path.c_str(), FILE_READ);
    if (!gFile) return;

    gFileSize  = gFile.size();
    gCharOff   = loadProgress(gBooks[gSel].dir);
    gResumeOff = gCharOff;
    gSaveCount = 0;

    gState   = READING;
    gPlaying = true;
    if (gEncoderOk) gEncPrev = gEncoder.getEncoderValue();

    drawFrame();

    if (gCharOff > 0 && gCharOff < gFileSize) {
        std::vector<Word> recapWords;
        int recapStart = (gCharOff > RECAP_BYTES) ? gCharOff - RECAP_BYTES : 0;
        gFile.seek(recapStart);

        if (recapStart > 0) seekToWordBoundary();

        int alignedStart = gFile.position();
        int recapLen = gCharOff - alignedStart;
        if (recapLen > 0 && recapLen <= RECAP_BYTES) {
            char rbuf[RECAP_BYTES + 1];
            int rn = gFile.read((uint8_t*)rbuf, recapLen);
            if (rn > 0) {
                rbuf[rn] = '\0';
                parseWords(rbuf, rn, alignedStart, recapWords);
            }
        }

        int trimStart = ((int)recapWords.size() > RECAP_WORDS)
                        ? (int)recapWords.size() - RECAP_WORDS : 0;

        gFile.seek(gCharOff);
        seekToWordBoundary();
        gCharOff = gFile.position();
        loadChunk();

        if (trimStart < (int)recapWords.size()) {
            gWords.insert(gWords.begin(),
                          recapWords.begin() + trimStart,
                          recapWords.end());
            gWIdx = 0;
        }
    } else {
        if (gCharOff >= gFileSize) {
            gCharOff = 0;
            gFile.seek(0);
        }
        loadChunk();
    }

    if (!gWords.empty()) {
        showCurrentWord();
        gWIdx = 1;
    }
}

void seekToWordBoundary() {
    if (!gFile.available()) return;
    char c = gFile.peek();
    if (c != ' ' && c != '\n' && c != '\r' && c != '\t') return;
    while (gFile.available()) {
        c = gFile.peek();
        if (c != ' ' && c != '\n' && c != '\r' && c != '\t') break;
        gFile.read();
    }
}


void parseWords(const char* buf, int n, int baseOffset, std::vector<Word>& out) {
    int wStart = -1;
    for (int i = 0; i <= n; i++) {
        char c = (i < n) ? buf[i] : ' ';
        bool ws = (c == ' ' || c == '\n' || c == '\r' || c == '\t' || c == '\0');
        if (!ws && wStart < 0) {
            wStart = i;
        } else if (ws && wStart >= 0) {
            String w;
            for (int j = wStart; j < i; j++) w += buf[j];
            out.push_back({w, baseOffset + wStart});
            wStart = -1;
        }
    }
}

void loadChunk() {
    gWords.clear();
    gWIdx = 0;
    if (!gFile || !gFile.available()) return;

    int chunkStart = gFile.position();
    char buf[CHUNK_BYTES + 1];
    int n = gFile.read((uint8_t*)buf, CHUNK_BYTES);
    if (n <= 0) return;
    buf[n] = '\0';

    if (gFile.available() && n == CHUNK_BYTES) {
        int back = n - 1;
        while (back > 0 && buf[back] != ' ' && buf[back] != '\n' && buf[back] != '\r') back--;
        if (back > 0) {
            gFile.seek(chunkStart + back + 1);
            buf[back + 1] = '\0';
            n = back + 1;
        }
    }

    parseWords(buf, n, chunkStart, gWords);
}


int loadProgress(const String& dir) {
    String path = dir + "/prog.txt";
    if (!SD.exists(path.c_str())) return 0;
    File f = SD.open(path.c_str(), FILE_READ);
    if (!f) return 0;
    String v = f.readStringUntil('\n');
    f.close();
    int val = v.toInt();
    return (val > 0) ? val : 0;
}

void saveProgress() {
    if (gCharOff < gResumeOff) return;
    String path = gBooks[gSel].dir + "/prog.txt";
    SD.remove(path.c_str());
    File f = SD.open(path.c_str(), FILE_WRITE);
    if (!f) return;
    f.println(gCharOff);
    f.close();
}


static const int kDurPct[] = { 50, 50, 57, 67, 83, 100, 111, 125, 143, 167, 200 };

unsigned long wordDelay(const String& w) {
    unsigned long base = 60000UL / gWpm;
    int len = w.length();

    int idx = (len > 10) ? 10 : len;
    if (idx < 1) idx = 1;
    base = (base * kDurPct[idx]) / 100;

    char last = w.charAt(w.length() - 1);
    if (last == '.' || last == '!' || last == '?')
        base = (base * 15) / 10;
    else if (last == ',' || last == ';' || last == ':')
        base = (base * 125) / 100;

    return base;
}


void drawInlineLabel(int borderY, const String& text, uint16_t textCol) {
    auto& d = M5Cardputer.Display;
    int tw = text.length() * 6;
    int tx = (SCR_W - tw) / 2;
    int gapPad = 5;

    d.fillRect(0, borderY - 3, SCR_W, 8, COL_BG);

    int leftEnd = tx - gapPad;
    if (leftEnd > 0)
        d.fillRect(0, borderY, leftEnd, BRD, COL_DIM);

    int rightStart = tx + tw + gapPad;
    if (rightStart < SCR_W)
        d.fillRect(rightStart, borderY, SCR_W - rightStart, BRD, COL_DIM);

    d.setTextSize(1);
    d.setTextColor(textCol);
    d.setCursor(tx, borderY - 3);
    d.print(text);
}

void drawInlineTop() {
    String t = gBooks[gSel].title;
    if (t.length() > 28) t = t.substring(0, 25) + "...";
    drawInlineLabel(BY, t, COL_ORP);
}

void drawInlineBot() {
    char buf[16];
    snprintf(buf, sizeof(buf), "%d wpm", gWpm);
    drawInlineLabel(BY + BH - BRD, String(buf), COL_ORP);
}

void drawFrame() {
    auto& d = M5Cardputer.Display;
    d.fillScreen(COL_BG);
    drawInlineTop();
    drawInlineBot();
}

void drawProgressBar() {
    auto& d = M5Cardputer.Display;
    d.fillRect(PB_X, PB_Y, PB_W, PB_H, COL_BG);
    if (gFileSize <= 0) return;
    d.fillRect(PB_X, PB_Y + 1, PB_W, PB_H - 2, COL_DIM);
    long filled = ((long)PB_W * gCharOff) / gFileSize;
    if (filled > PB_W) filled = PB_W;
    if (filled > 0)
        d.fillRect(PB_X, PB_Y, (int)filled, PB_H, COL_ORP);
}

void drawWord(int idx) {
    auto& d = M5Cardputer.Display;
    const String& txt = gWords[idx].text;
    int len = txt.length();

    d.fillRect(0, WY, SCR_W, WH, COL_BG);

    int wordY = WY + (WH / 2) - (CHAR_H / 2);
    int prevY = wordY - 14;
    int nextY = wordY + CHAR_H + 6;

    if (idx > 0) {
        const String& prev = gWords[idx - 1].text;
        int pw = prev.length() * 6;
        int px = (SCR_W - pw) / 2;
        d.setTextSize(1);
        d.setTextWrap(false);
        d.setTextColor(COL_PREV);
        d.setCursor(px, prevY);
        d.print(prev);
    }

    int textW  = len * CHAR_W;
    int startX = (SCR_W - textW) / 2;

    d.setTextSize(FONT_SCALE);
    d.setTextWrap(false);
    d.setTextColor(COL_TEXT);
    for (int i = 0; i < len; i++) {
        int x = startX + (i * CHAR_W);
        if (x + CHAR_W <= 0 || x >= SCR_W) continue;
        d.setCursor(x, wordY);
        d.print(txt.charAt(i));
    }

    if (idx + 1 < (int)gWords.size()) {
        const String& nxt = gWords[idx + 1].text;
        int nw = nxt.length() * 6;
        int nx = (SCR_W - nw) / 2;
        d.setTextSize(1);
        d.setTextWrap(false);
        d.setTextColor(COL_PREV);
        d.setCursor(nx, nextY);
        d.print(nxt);
    }

    drawProgressBar();
}
