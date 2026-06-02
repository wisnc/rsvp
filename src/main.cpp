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
#define COL_HIST   0x8410
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

#define BAR_H  4

#define RD_TOP   16
#define RD_H     103

#define TXT_X       6
#define TXT_LH      10
#define TXT_MAXC    ((SCR_W - TXT_X * 2) / 6)
#define ANCHOR_Y    72
#define VIS_LINES   6

#define DIV_Y       84
#define MAIN_Y      90

#define WIN_FWD     300
#define WIN_BACK    520
#define ADV_LIMIT   2000
#define BUF_MAX     2700

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
static int               gAnchorPos  = -1;

static M5Canvas          gCanvas(&M5Cardputer.Display);
static bool              gCanvasOk = false;

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
void loadChunkBefore(int firstPos);
void parseWords(const char* buf, int n, int baseOffset, std::vector<Word>& out);
void drawFrame();
void drawWord(int idx);
void drawInlineTop();
void drawBottomBar();
int snapWordStart(int a);
void showCurrentWord();
void saveProgress();
int  loadProgress(const String& dir);
void loadSettings();
void saveSettings();
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

    gCanvas.setColorDepth(16);
    gCanvasOk = (gCanvas.createSprite(SCR_W, SCR_H) != nullptr);
    if (gCanvasOk) gCanvas.setTextWrap(false);

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

    loadSettings();
    M5Cardputer.Display.setBrightness(gBrightness);

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
                    if (gWpm < MAX_WPM) { gWpm += WPM_STEP; drawBottomBar(); saveSettings(); }
                }
                else if (k == '.') {
                    if (gWpm > MIN_WPM) { gWpm -= WPM_STEP; drawBottomBar(); saveSettings(); }
                }
                else if (k == '=' || k == '+') {
                    if (gBrightness < MAX_BRIGHT) {
                        gBrightness = min(gBrightness + BRIGHT_STEP, MAX_BRIGHT);
                        M5Cardputer.Display.setBrightness(gBrightness);
                        saveSettings();
                    }
                }
                else if (k == '-') {
                    if (gBrightness > MIN_BRIGHT) {
                        gBrightness = max(gBrightness - BRIGHT_STEP, MIN_BRIGHT);
                        M5Cardputer.Display.setBrightness(gBrightness);
                        saveSettings();
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
                lgfx::LGFXBase& d = *(gCanvasOk ? (lgfx::LGFXBase*)&gCanvas : (lgfx::LGFXBase*)&M5Cardputer.Display);
                d.fillRect(0, RD_TOP, SCR_W, RD_H, COL_BG);
                d.setTextSize(2);
                d.setTextColor(COL_DIM);
                d.setCursor(85, SCR_H / 2 - 8);
                d.print("- end -");
                if (gCanvasOk) gCanvas.pushSprite(0, 0);
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
        return;
    }

    if (gWords.empty()) return;
    int firstPos = gWords[0].pos;
    if (firstPos <= 0) return;

    loadChunkBefore(firstPos);
    if (gWords.empty()) return;

    gWIdx = (int)gWords.size() - 1;
    showCurrentWord();
    gWIdx++;
    gLastMs = millis();
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
    gPlaying = false;
    gAnchorPos = -1;
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

void loadChunkBefore(int firstPos) {
    gWords.clear();
    gWIdx = 0;
    if (!gFile || firstPos <= 0) return;

    int start = firstPos - CHUNK_BYTES;
    if (start < 0) start = 0;
    if (start > 0) start = snapWordStart(start);
    if (start >= firstPos) return;

    int len = firstPos - start;
    gFile.seek(start);
    char buf[CHUNK_BYTES + 1];
    int n = gFile.read((uint8_t*)buf, len);
    if (n <= 0) return;
    buf[n] = '\0';

    parseWords(buf, n, start, gWords);
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

void loadSettings() {
    if (!SD.exists("/.rsvp_config")) return;
    File f = SD.open("/.rsvp_config", FILE_READ);
    if (!f) return;
    while (f.available()) {
        String line = f.readStringUntil('\n');
        line.trim();
        int eq = line.indexOf('=');
        if (eq < 0) continue;
        String key = line.substring(0, eq);
        int val = line.substring(eq + 1).toInt();
        if (key == "wpm") {
            if (val >= MIN_WPM && val <= MAX_WPM) gWpm = val;
        } else if (key == "bright") {
            if (val >= MIN_BRIGHT && val <= MAX_BRIGHT) gBrightness = val;
        }
    }
    f.close();
}

void saveSettings() {
    SD.remove("/.rsvp_config");
    File f = SD.open("/.rsvp_config", FILE_WRITE);
    if (!f) return;
    f.print("wpm=");
    f.println(gWpm);
    f.print("bright=");
    f.println(gBrightness);
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
    lgfx::LGFXBase& d = *(gCanvasOk ? (lgfx::LGFXBase*)&gCanvas : (lgfx::LGFXBase*)&M5Cardputer.Display);
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

void drawBottomBar() {
    lgfx::LGFXBase& d = *(gCanvasOk ? (lgfx::LGFXBase*)&gCanvas : (lgfx::LGFXBase*)&M5Cardputer.Display);
    int by = BY + BH - BRD;

    char buf[16];
    snprintf(buf, sizeof(buf), "%d wpm", gWpm);
    String text = String(buf);
    int tw = text.length() * 6;
    int rightPad = 6;
    int gapPad = 6;
    int tx = SCR_W - rightPad - tw;
    int barW = tx - gapPad;

    d.fillRect(0, by - 3, SCR_W, BAR_H + 8, COL_BG);

    if (barW > 0) {
        d.fillRect(0, by, barW, BAR_H, COL_DIM);
        if (gFileSize > 0) {
            long filled = ((long)barW * gCharOff) / gFileSize;
            if (filled > barW) filled = barW;
            if (filled > 0) d.fillRect(0, by, (int)filled, BAR_H, COL_ORP);
        }
    }

    d.setTextSize(1);
    d.setTextColor(COL_ORP);
    d.setCursor(tx, by - 2);
    d.print(text);

    if (gCanvasOk) gCanvas.pushSprite(0, 0);
}

int snapWordStart(int a) {
    if (a <= 0) return 0;
    long saved = gFile.position();
    gFile.seek(a);
    char tmp[96];
    int m = gFile.read((uint8_t*)tmp, 96);
    gFile.seek(saved);
    int j = 0;
    while (j < m && tmp[j] != ' ' && tmp[j] != '\n' && tmp[j] != '\r' && tmp[j] != '\t') j++;
    while (j < m && (tmp[j] == ' ' || tmp[j] == '\n' || tmp[j] == '\r' || tmp[j] == '\t')) j++;
    return (j < m) ? a + j : a;
}

void drawFrame() {
    lgfx::LGFXBase& d = *(gCanvasOk ? (lgfx::LGFXBase*)&gCanvas : (lgfx::LGFXBase*)&M5Cardputer.Display);
    d.fillScreen(COL_BG);
    drawInlineTop();
    drawBottomBar();
}

void drawWord(int idx) {
    lgfx::LGFXBase& d = *(gCanvasOk ? (lgfx::LGFXBase*)&gCanvas : (lgfx::LGFXBase*)&M5Cardputer.Display);

    int curStart = gWords[idx].pos;
    int curEnd   = curStart + gWords[idx].text.length();
    (void)curEnd;

    if (gAnchorPos < 0 || curStart < gAnchorPos) {
        int a = curStart - WIN_BACK;
        if (a < 0) a = 0;
        gAnchorPos = snapWordStart(a);
    }

    int winEnd = curStart + WIN_FWD;
    if (winEnd > gFileSize) winEnd = gFileSize;
    int want = winEnd - gAnchorPos;
    if (want < 0) want = 0;
    if (want > BUF_MAX - 1) want = BUF_MAX - 1;

    static char buf[BUF_MAX];
    long saved = gFile.position();
    gFile.seek(gAnchorPos);
    int m = gFile.read((uint8_t*)buf, want);
    gFile.seek(saved);

    struct Tok { int off; int len; int pos; int nl; };
    std::vector<Tok> toks;
    {
        int i = 0, pendingNL = 0;
        while (i < m) {
            while (i < m) {
                char c = buf[i];
                if (c == '\n') { pendingNL++; i++; }
                else if (c == ' ' || c == '\r' || c == '\t') i++;
                else break;
            }
            if (i >= m) break;
            int ws = i;
            while (i < m) {
                char c = buf[i];
                if (c == ' ' || c == '\n' || c == '\r' || c == '\t') break;
                i++;
            }
            toks.push_back({ws, i - ws, gAnchorPos + ws, pendingNL});
            pendingNL = 0;
        }
    }

    struct Line { std::vector<int> idx; bool blank; };
    std::vector<Line> lines;
    {
        int maxc = TXT_MAXC;
        Line cur; cur.blank = false; int curLen = 0; bool empty = true;
        for (int k = 0; k < (int)toks.size(); k++) {
            int nl = toks[k].nl;
            if (!empty && nl >= 1) {
                lines.push_back(cur);
                cur = Line(); cur.blank = false; curLen = 0; empty = true;
            }
            if (nl >= 2) {
                Line b; b.blank = true;
                lines.push_back(b);
            }
            int tl = toks[k].len;
            if (empty) {
                cur.idx.push_back(k); curLen = tl; empty = false;
            } else if (curLen + 1 + tl <= maxc) {
                cur.idx.push_back(k); curLen += 1 + tl;
            } else {
                lines.push_back(cur);
                cur = Line(); cur.blank = false;
                cur.idx.push_back(k); curLen = tl; empty = false;
            }
        }
        if (!empty) lines.push_back(cur);
    }

    int cli = -1;
    for (int li = (int)lines.size() - 1; li >= 0 && cli < 0; li--) {
        if (lines[li].blank) continue;
        for (int k = 0; k < (int)lines[li].idx.size(); k++) {
            if (toks[lines[li].idx[k]].pos == curStart) { cli = li; break; }
        }
    }
    if (cli < 0) {
        for (int li = (int)lines.size() - 1; li >= 0; li--) {
            if (!lines[li].blank) { cli = li; break; }
        }
    }

    d.fillRect(0, RD_TOP, SCR_W, RD_H, COL_BG);
    d.setTextSize(1);
    d.setTextWrap(false);

    if (cli >= 0) {
        int firstLine = cli - (VIS_LINES - 1);
        if (firstLine < 0) firstLine = 0;
        for (int li = firstLine; li <= cli; li++) {
            int y = ANCHOR_Y - (cli - li) * TXT_LH;
            if (y < RD_TOP) continue;
            if (lines[li].blank) continue;
            int x = TXT_X;
            for (int k = 0; k < (int)lines[li].idx.size(); k++) {
                Tok& tk = toks[lines[li].idx[k]];
                String s;
                s.reserve(tk.len);
                for (int j = 0; j < tk.len; j++) s += buf[tk.off + j];
                d.setTextColor(tk.pos == curStart ? COL_ORP : COL_HIST);
                d.setCursor(x, y);
                d.print(s);
                x += tk.len * 6 + 6;
            }
        }
    }

    d.drawFastHLine(TXT_X, DIV_Y, SCR_W - TXT_X * 2, COL_DIM);

    {
        const String& fw = gWords[idx].text;
        int fl = fw.length();
        int fwW = fl * CHAR_W;
        int fx = (SCR_W - fwW) / 2;
        d.setTextSize(FONT_SCALE);
        d.setTextColor(COL_TEXT);
        for (int i = 0; i < fl; i++) {
            int x = fx + i * CHAR_W;
            if (x + CHAR_W <= 0 || x >= SCR_W) continue;
            d.setCursor(x, MAIN_Y);
            d.print(fw.charAt(i));
        }
    }

    drawBottomBar();

    if (cli >= 0 && (curStart - gAnchorPos) > ADV_LIMIT) {
        int firstLine = cli - (VIS_LINES - 1);
        if (firstLine < 0) firstLine = 0;
        int target = firstLine - 3;
        if (target < 0) target = 0;
        for (int li = target; li <= cli; li++) {
            if (!lines[li].blank && !lines[li].idx.empty()) {
                int np = toks[lines[li].idx[0]].pos;
                if (np > gAnchorPos) gAnchorPos = np;
                break;
            }
        }
    }
}
