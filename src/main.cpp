#include <M5Cardputer.h>
#include <M5UnitScroll.h>
#include <SD.h>
#include <SPI.h>
#include <vector>
#include <algorithm>
#define EPUB_LOG(...) Serial.printf(__VA_ARGS__)
#include "epub_core.h"
#include "font_cp1251.h"

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

#define CHAP_SLACK   64
#define SKIP_PCT      2

enum State { MENU, INFO, READING };

struct Book { String title; String dir; uint32_t mtime; int pct; };
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
static bool              gPeripheral = false;

static String            gInfoDir;
static String            gInfoTitle;
static String            gInfoAuthor;
static long              gInfoTotal = 0;
static long              gInfoOff = 0;
static std::vector<long> gInfoChapters;

static M5Canvas          gCanvas(&M5Cardputer.Display);
static bool              gCanvasOk = false;

static SPIClass          gSdSpi(HSPI);
static M5UnitScroll      gEncoder;
static bool              gEncoderOk   = false;
static bool              gBtnPrev     = false;
static bool              gCtrlPrev    = false;
static bool              gBtn0Prev    = false;
static int32_t           gEncPrev     = 0;

void scanBooks();
int  bookPct(const String& dir);
bool dirHasEpub(const String& dir);
String findEpub(const String& dir);
long metaLong(const String& dir, const char* key);
long epubFileSize(const String& path);
bool extractEpub(const String& dir, const String& epubPath);
bool ensureExtracted(const String& dir, const String& epubPath);
bool canvasDrop();
void canvasRestore(bool had);
void drawConverting(const String& msg);
void drawMenu();
void enterInfo();
void drawInfo();
void loadMeta(const String& dir);
bool extractCover(const String& dir, const String& epubPath);
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
void nextChapter();
void prevChapter();
void jumpTo(long off);
int  chapterAt(long pos);
void drawText(lgfx::LGFXBase& d, const String& s, int x, int y, int scale, uint16_t col);

template <typename T> static auto ksLeft(const T& k, int) -> decltype(k.left, bool()) { return k.left; }
template <typename T> static bool ksLeft(const T&, long) { return false; }
template <typename T> static auto ksRight(const T& k, int) -> decltype(k.right, bool()) { return k.right; }
template <typename T> static bool ksRight(const T&, long) { return false; }
template <typename T> static auto ksBackspace(const T& k, int) -> decltype(k.backspace, bool()) { return k.backspace; }
template <typename T> static bool ksBackspace(const T&, long) { return false; }


void setup() {
    auto cfg = M5.config();
    M5Cardputer.begin(cfg, true);
    Serial.begin(115200);

    pinMode(0, INPUT_PULLUP);

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
        if (ks.enter) { enterInfo(); return; }
        if (changed) drawMenu();

    } else if (gState == INFO) {
        if (!M5Cardputer.Keyboard.isChange() || !M5Cardputer.Keyboard.isPressed()) return;
        auto ks = M5Cardputer.Keyboard.keysState();
        bool back = ks.del || ksBackspace(ks, 0);
        for (auto k : ks.word) if (k == '`') back = true;
        if (back) { gState = MENU; gBooks[gSel].pct = bookPct(gBooks[gSel].dir); drawMenu(); return; }
        if (ks.enter || ks.space) { openBook(); return; }

    } else {
        if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
            auto ks = M5Cardputer.Keyboard.keysState();

            bool wantsExit = ks.del || ksBackspace(ks, 0);
            for (auto k : ks.word) if (k == '`') wantsExit = true;
            if (wantsExit) {
                gPlaying = false;
                saveProgress();
                gFile.close();
                gState = MENU;
                gBooks[gSel].pct = bookPct(gBooks[gSel].dir);
                drawMenu();
                return;
            }

            if (ks.space) {
                gPlaying = !gPlaying;
                if (gPlaying) gLastMs = millis();
            }

            if (ksRight(ks, 0)) nextChapter();
            if (ksLeft(ks, 0)) prevChapter();

            for (auto k : ks.word) {
                if (k == '/') { if (ks.fn) nextChapter(); else advanceWord(); }
                else if (k == ',') { if (ks.fn) prevChapter(); else retreatWord(); }
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
                else if (k == 'p') {
                    gPeripheral = !gPeripheral;
                    saveSettings();
                    if (gWIdx > 0) drawWord(gWIdx - 1);
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

        bool ctrlNow = M5Cardputer.Keyboard.keysState().ctrl;
        if (ctrlNow && !gCtrlPrev) {
            gPlaying = !gPlaying;
            if (gPlaying) gLastMs = millis();
        }
        gCtrlPrev = ctrlNow;

        bool b0Now = (digitalRead(0) == LOW);
        if (b0Now && !gBtn0Prev) {
            gPlaying = !gPlaying;
            if (gPlaying) gLastMs = millis();
        }
        gBtn0Prev = b0Now;

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

int chapterAt(long pos) {
    int ch = -1;
    for (int i = 0; i < (int)gInfoChapters.size(); i++) if (pos >= gInfoChapters[i]) ch = i;
    return ch;
}

static long curWordPos() {
    if (gWIdx > 0 && gWIdx <= (int)gWords.size()) return gWords[gWIdx - 1].pos;
    return gCharOff;
}

void jumpTo(long off) {
    if (!gFile) return;
    if (off < 0) off = 0;
    if (off >= gFileSize) return;
    gFile.seek(off);
    seekToWordBoundary();
    gCharOff   = gFile.position();
    gResumeOff = gCharOff;
    gAnchorPos = -1;
    gSaveCount = 0;
    loadChunk();
    if (!gWords.empty()) { showCurrentWord(); gWIdx = 1; }
    saveProgress();
}

void nextChapter() {
    long p = curWordPos();
    if (gInfoChapters.empty()) { jumpTo(p + (long)gFileSize * SKIP_PCT / 100); return; }
    for (size_t i = 0; i < gInfoChapters.size(); i++) {
        if (gInfoChapters[i] > p) { jumpTo(gInfoChapters[i]); return; }
    }
}

void prevChapter() {
    long p = curWordPos();
    if (gInfoChapters.empty()) { jumpTo(p - (long)gFileSize * SKIP_PCT / 100); return; }
    int ch = chapterAt(p);
    if (ch < 0) { jumpTo(0); return; }
    long start = gInfoChapters[ch];
    if (ch == 0 || p - start > CHAP_SLACK) jumpTo(start);
    else jumpTo(gInfoChapters[ch - 1]);
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
            uint32_t mt = (uint32_t)entry.getLastWrite();
            if (SD.exists((dir + "/read.txt").c_str()) || dirHasEpub(dir))
                gBooks.push_back({title, dir, mt, bookPct(dir)});
        }
        entry.close();
    }
    root.close();
    std::reverse(gBooks.begin(), gBooks.end());
}


int bookPct(const String& dir) {
    long total = metaLong(dir, "total=");
    if (total <= 0) total = epubFileSize(dir + "/read.txt");
    if (total <= 0) return -1;
    long off = loadProgress(dir);
    if (off < 0) off = 0;
    if (off > total) off = total;
    return (int)((off * 100 + total / 2) / total);
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
        if (label.length() > 30) label = label.substring(0, 27) + "...";
        d.setCursor(14, y + 6);
        d.print(label);
        if (gBooks[i].pct >= 0) {
            String p = String(gBooks[i].pct) + "%";
            d.setTextColor(i == gSel ? COL_ORP : COL_HIST);
            d.setCursor(SCR_W - 10 - p.length() * 6, y + 6);
            d.print(p);
        }
        y += itemH;
    }
    d.setTextColor(COL_TEXT);
}


struct SdSrc : Src {
    File f; bool ok;
    SdSrc(const String& path) { f = SD.open(path.c_str(), FILE_READ); ok = (bool)f; }
    ~SdSrc() { if (f) f.close(); }
    uint64_t size() override { return f.size(); }
    size_t read(uint64_t off, void* buf, size_t n) override { f.seek(off); return f.read((uint8_t*)buf, n); }
};

struct OutBuf { File* f; uint8_t buf[512]; size_t len; };
static void obSink(const uint8_t* p, size_t n, void* user) {
    OutBuf* o = (OutBuf*)user;
    for (size_t i = 0; i < n; i++) {
        o->buf[o->len++] = p[i];
        if (o->len == sizeof(o->buf)) { o->f->write(o->buf, o->len); o->len = 0; }
    }
}

bool canvasDrop() {
    bool had = gCanvasOk;
    if (gCanvasOk) { gCanvas.deleteSprite(); gCanvasOk = false; }
    return had;
}

void canvasRestore(bool had) {
    if (!had) return;
    gCanvas.setColorDepth(16);
    gCanvasOk = (gCanvas.createSprite(SCR_W, SCR_H) != nullptr);
    if (gCanvasOk) gCanvas.setTextWrap(false);
}

void drawConverting(const String& msg) {
    auto& d = M5Cardputer.Display;
    d.fillScreen(COL_BG);
    d.setTextWrap(false);
    d.setTextSize(1);
    d.setTextColor(COL_TEXT);
    d.setCursor(10, SCR_H / 2 - 12);
    d.print("Converting epub...");
    d.setTextColor(COL_DIM);
    d.setCursor(10, SCR_H / 2 + 4);
    d.print(msg);
}

static bool writeEntry(SdSrc& src, const ZipEntry& e, const String& path) {
    SD.remove(path.c_str());
    File cf = SD.open(path.c_str(), FILE_WRITE);
    if (!cf) return false;
    OutBuf ob; ob.f = &cf; ob.len = 0;
    bool ok = zipExtractToSink(src, e, obSink, &ob);
    if (ob.len) cf.write(ob.buf, ob.len);
    cf.close();
    if (!ok) SD.remove(path.c_str());
    return ok;
}

String findEpub(const String& dir) {
    File d = SD.open(dir.c_str());
    if (!d || !d.isDirectory()) { if (d) d.close(); return String(); }
    String result;
    File e;
    while ((e = d.openNextFile())) {
        if (!e.isDirectory()) {
            String nm = String(e.name());
            int sl = nm.lastIndexOf('/');
            if (sl >= 0) nm = nm.substring(sl + 1);
            String low = nm; low.toLowerCase();
            if (low.endsWith(".epub")) { result = dir + "/" + nm; e.close(); break; }
        }
        e.close();
    }
    d.close();
    return result;
}
bool dirHasEpub(const String& dir) { return findEpub(dir).length() > 0; }

long epubFileSize(const String& path) {
    File f = SD.open(path.c_str(), FILE_READ);
    if (!f) return -1;
    long s = (long)f.size(); f.close(); return s;
}

long metaLong(const String& dir, const char* key) {
    String path = dir + "/.rsvp_meta";
    if (!SD.exists(path.c_str())) return -1;
    File f = SD.open(path.c_str(), FILE_READ);
    if (!f) return -1;
    long v = -1;
    size_t kl = strlen(key);
    while (f.available()) {
        String line = f.readStringUntil('\n');
        line.trim();
        if (line.startsWith(key)) { v = line.substring(kl).toInt(); break; }
    }
    f.close();
    return v;
}

static void metaAppend(const String& dir, const char* key, long v) {
    String path = dir + "/.rsvp_meta";
    File f = SD.open(path.c_str(), FILE_APPEND);
    if (!f) return;
    f.print(key); f.println(v);
    f.close();
}

bool extractEpub(const String& dir, const String& epubPath) {
    drawConverting("reading...");
    Serial.printf("[epub] extract start: %s\n", epubPath.c_str());
    SdSrc src(epubPath);
    if (!src.ok) { Serial.println("[epub] FAIL open epub"); return false; }

    std::vector<ZipEntry> zdir;
    std::string opfPath, opf;
    if (!epubLoadOpf(src, zdir, opfPath, opf)) return false;

    if (zipFind(zdir, std::string("META-INF/encryption.xml"))) {
        drawConverting("DRM protected - skipped");
        delay(1500);
        return false;
    }

    std::string opfDir = dirOf(opfPath);
    std::string title = tagText(opf, "<dc:title"); if (title.empty()) title = tagText(opf, "<title");
    std::string author = tagText(opf, "<dc:creator"); if (author.empty()) author = tagText(opf, "<creator");
    title = normalizeAscii(title);
    author = normalizeAscii(author);

    std::vector<int> spineIdx;
    resolveSpine(opf, opfDir, zdir, spineIdx);
    int coverIdx = -1;
    { std::string ch = coverHref(opf); if (!ch.empty()) coverIdx = zipIndex(zdir, joinPath(opfDir, ch)); }
    std::string().swap(opf);
    std::string().swap(opfPath);

    {
        std::vector<ZipEntry> keep;
        keep.reserve(spineIdx.size() + 1);
        for (size_t k = 0; k < spineIdx.size(); k++) {
            if (spineIdx[k] < 0) continue;
            keep.push_back(zdir[spineIdx[k]]);
            spineIdx[k] = (int)keep.size() - 1;
        }
        if (coverIdx >= 0) { keep.push_back(zdir[coverIdx]); coverIdx = (int)keep.size() - 1; }
        zdir.swap(keep);
    }

    int total = 0;
    for (size_t k = 0; k < spineIdx.size(); k++) if (spineIdx[k] >= 0) total++;
    if (total == 0) { Serial.println("[epub] FAIL empty spine"); return false; }

    String readPath = dir + "/read.txt";
    SD.remove(readPath.c_str());
    File out = SD.open(readPath.c_str(), FILE_WRITE);
    if (!out) { Serial.println("[epub] FAIL open read.txt for write"); return false; }

    OutBuf ob; ob.f = &out; ob.len = 0;
    TextOut to; to.init(obSink, &ob);

    std::vector<uint64_t> chapOffs;
    chapOffs.reserve(total);
    int chapN = 0;
    for (size_t k = 0; k < spineIdx.size(); k++) {
        if (spineIdx[k] < 0) continue;
        const ZipEntry& xe = zdir[spineIdx[k]];
        Serial.printf("[epub] chapter %d/%d %s\n", chapN + 1, total, xe.name.c_str());
        { String m = "chapter "; m += (chapN + 1); m += "/"; m += total; drawConverting(m); }
        if (chapN > 0) to.reqBlank();
        chapOffs.push_back(to.count);
        StripSink ssink; ssink.out = &to;
        chapN++;
        if (!zipExtractToSink(src, xe, stripFeedSink, &ssink)) { Serial.printf("[epub] extract fail: %s\n", xe.name.c_str()); chapOffs.pop_back(); continue; }
        ssink.strip.finish(to);
    }
    to.flush();
    {
        std::vector<uint64_t> u;
        for (size_t i = 0; i < chapOffs.size(); i++) {
            if (chapOffs[i] >= to.count) break;
            if (!u.empty() && u.back() == chapOffs[i]) continue;
            u.push_back(chapOffs[i]);
        }
        chapOffs.swap(u);
    }
    if (ob.len) out.write(ob.buf, ob.len);
    out.close();
    Serial.printf("[epub] text done: %lu chars\n",(unsigned long)to.count);

    drawConverting("cover...");
    bool hasCover = false;
    if (coverIdx >= 0) hasCover = writeEntry(src, zdir[coverIdx], dir + "/.rsvp_cover");
    else SD.remove((dir + "/.rsvp_cover").c_str());

    String metaPath = dir + "/.rsvp_meta";
    SD.remove(metaPath.c_str());
    File mf = SD.open(metaPath.c_str(), FILE_WRITE);
    if (mf) {
        mf.print("epub_size="); mf.println((long)src.size());
        mf.print("title="); mf.println(title.c_str());
        mf.print("author="); mf.println(author.c_str());
        mf.print("total="); mf.println((unsigned long)to.count);
        mf.print("cover="); mf.println(hasCover ? 1 : 0);
        mf.print("chapters=");
        for (size_t i = 0; i < chapOffs.size(); i++) { if (i) mf.print(","); mf.print((unsigned long)chapOffs[i]); }
        mf.println();
        mf.close();
    }
    Serial.println("[epub] extract complete");
    return true;
}

bool extractCover(const String& dir, const String& epubPath) {
    SdSrc src(epubPath);
    if (!src.ok) return false;
    std::vector<ZipEntry> zdir;
    std::string opfPath, opf;
    if (!epubLoadOpf(src, zdir, opfPath, opf)) return false;
    int idx = -1;
    { std::string ch = coverHref(opf); if (!ch.empty()) idx = zipIndex(zdir, joinPath(dirOf(opfPath), ch)); }
    std::string().swap(opf);
    if (idx < 0) return false;
    return writeEntry(src, zdir[idx], dir + "/.rsvp_cover");
}

bool ensureExtracted(const String& dir, const String& epubPath) {
    if (epubPath.length() == 0) return true;
    String readPath = dir + "/read.txt";
    bool need = false;
    if (!SD.exists(readPath.c_str())) need = true;
    else if (epubFileSize(readPath) <= 0) need = true;
    else {
        long ms = metaLong(dir, "epub_size=");
        if (ms < 0 || ms != epubFileSize(epubPath)) need = true;
    }
    if (!need) return true;
    bool had = canvasDrop();
    bool ok = extractEpub(dir, epubPath);
    canvasRestore(had);
    return ok;
}

void loadMeta(const String& dir) {
    gInfoTitle = ""; gInfoAuthor = ""; gInfoTotal = 0; gInfoChapters.clear();
    String metaPath = dir + "/.rsvp_meta";
    if (SD.exists(metaPath.c_str())) {
        File f = SD.open(metaPath.c_str(), FILE_READ);
        if (f) {
            while (f.available()) {
                String line = f.readStringUntil('\n');
                line.trim();
                if (line.startsWith("title=")) gInfoTitle = line.substring(6);
                else if (line.startsWith("author=")) gInfoAuthor = line.substring(7);
                else if (line.startsWith("total=")) gInfoTotal = line.substring(6).toInt();
                else if (line.startsWith("chapters=")) {
                    String cs = line.substring(9);
                    int start = 0;
                    while (start < (int)cs.length()) {
                        int comma = cs.indexOf(',', start);
                        String tok = (comma < 0) ? cs.substring(start) : cs.substring(start, comma);
                        tok.trim();
                        if (tok.length()) gInfoChapters.push_back(tok.toInt());
                        if (comma < 0) break;
                        start = comma + 1;
                    }
                }
            }
            f.close();
        }
    }
    if (gInfoTitle.length() == 0) gInfoTitle = gBooks[gSel].title;
    if (gInfoTotal <= 0) {
        File rf = SD.open((dir + "/read.txt").c_str(), FILE_READ);
        if (rf) { gInfoTotal = rf.size(); rf.close(); }
    }
}

static bool jpegSize(const char* path, int& w, int& h) {
    File f = SD.open(path, FILE_READ);
    if (!f) return false;
    if (f.read() != 0xFF || f.read() != 0xD8) { f.close(); return false; }
    bool ok = false;
    while (f.available()) {
        int b = f.read();
        if (b < 0) break;
        if (b != 0xFF) continue;
        int m = f.read();
        while (m == 0xFF) m = f.read();
        if (m < 0) break;
        if (m == 0x01 || (m >= 0xD0 && m <= 0xD9)) continue;
        int hi = f.read(), lo = f.read();
        if (hi < 0 || lo < 0) break;
        int len = (hi << 8) | lo;
        if (len < 2) break;
        if ((m >= 0xC0 && m <= 0xCF) && m != 0xC4 && m != 0xC8 && m != 0xCC) {
            f.read();
            int h1 = f.read(), h2 = f.read(), w1 = f.read(), w2 = f.read();
            h = (h1 << 8) | h2; w = (w1 << 8) | w2; ok = true; break;
        }
        f.seek(f.position() + (len - 2));
    }
    f.close();
    return ok;
}

struct SdFileWrapper : public lgfx::v1::DataWrapper {
    File f;
    bool ok;
    SdFileWrapper(const char* path) : lgfx::v1::DataWrapper() {
        need_transaction = true;
        f = SD.open(path, FILE_READ);
        ok = (bool)f;
    }
    ~SdFileWrapper() { if (f) f.close(); }
    int read(uint8_t* buf, uint32_t len) override { return f.read(buf, len); }
    void skip(int32_t offset) override { f.seek(f.position() + offset); }
    bool seek(uint32_t offset) override { return f.seek(offset); }
    void close(void) override { if (f) f.close(); }
    int32_t tell(void) override { return (int32_t)f.position(); }
};

int drawCover(const String& path) {
    if (!SD.exists(path.c_str())) return -1;
    auto& disp = M5Cardputer.Display;
    uint8_t sig[4] = {0,0,0,0};
    { File hf = SD.open(path.c_str(), FILE_READ); if (!hf) return -1; hf.read(sig, 4); hf.close(); }
    if (sig[0] == 0x89 && sig[1] == 0x50) {
        SdFileWrapper w(path.c_str());
        return (w.ok && disp.drawPng(&w, 0, 0, 96, 135, 0, 0, 0.0f, 0.0f)) ? 1 : 0;
    }
    if (!(sig[0] == 0xFF && sig[1] == 0xD8)) return 0;
    int nw = 0, nh = 0;
    if (!jpegSize(path.c_str(), nw, nh) || nw <= 0 || nh <= 0) {
        SdFileWrapper w(path.c_str());
        return (w.ok && disp.drawJpg(&w, 0, 0, 96, 135, 0, 0, 0.0f, 0.0f)) ? 1 : 0;
    }
    if (nw >= 768 || nh >= 1080) return 0;
    SdFileWrapper w(path.c_str());
    return (w.ok && disp.drawJpg(&w, 0, 0, 96, 135, 0, 0, 0.0f, 0.0f)) ? 1 : 0;
}

void drawInfo() {
    auto& d = M5Cardputer.Display;
    d.fillScreen(COL_BG);
    d.setTextWrap(false);
    d.setTextSize(1);

    String coverPath = gInfoDir + "/.rsvp_cover";
    int cover = drawCover(coverPath);
    d.clearClipRect();
    if (cover != 1) {
        d.drawRect(0, 0, 96, 135, COL_DIM);
        d.setTextColor(COL_DIM);
        if (cover == -1) {
            d.setCursor(20, 58); d.print("No cover");
            d.setCursor(32, 70); d.print("art");
        } else {
            d.setCursor(14, 58); d.print("Can't render");
            d.setCursor(20, 70); d.print("cover art");
        }
    }

    int tx = 102;
    int tw = 236 - tx;
    int cpl = tw / 6; if (cpl < 1) cpl = 1;
    int y = 8;

    d.setTextColor(COL_TEXT);
    {
        String t = gInfoTitle;
        int lines = 0;
        while (t.length() > 0 && lines < 3) {
            String seg;
            if ((int)t.length() <= cpl) { seg = t; t = ""; }
            else {
                int cut = cpl;
                int sp = t.lastIndexOf(' ', cpl);
                if (sp > 0) cut = sp;
                seg = t.substring(0, cut);
                t = t.substring(cut); t.trim();
                if (lines == 2 && t.length() > 0) {
                    if (seg.length() > 2) seg = seg.substring(0, seg.length() - 2) + "..";
                    t = "";
                }
            }
            drawText(d, seg, tx, y, 1, COL_TEXT);
            y += 11; lines++;
        }
    }
    y += 5;

    if (gInfoAuthor.length() > 0) {
        String a = "by " + gInfoAuthor;
        if ((int)a.length() > cpl) a = a.substring(0, cpl);
        drawText(d, a, tx, y, 1, COL_HIST);
        y += 15;
    }

    long total = gInfoTotal > 0 ? gInfoTotal : 1;

    if (gInfoChapters.size() > 0) {
        int ch = 0;
        for (int i = 0; i < (int)gInfoChapters.size(); i++) if (gInfoOff >= gInfoChapters[i]) ch = i;
        d.setTextColor(COL_TEXT);
        d.setCursor(tx, y); d.print("Chapter " + String(ch + 1) + " of " + String((int)gInfoChapters.size()));
        y += 13;
    }

    int pct = (int)((float)gInfoOff / (float)total * 100.0f + 0.5f);
    if (pct > 100) pct = 100;
    d.setTextColor(COL_TEXT);
    d.setCursor(tx, y); d.print(String(pct) + "% read");
    y += 12;

    d.setTextColor(COL_DIM);
    d.setCursor(tx, y); d.print(String(gInfoOff) + " / " + String(total));

    int barY = 120, barH = 8, barX = tx, barW = tw;
    d.drawRect(barX, barY, barW, barH, COL_DIM);
    int fillw = (int)((float)gInfoOff / (float)total * (float)(barW - 2));
    if (fillw < 0) fillw = 0;
    if (fillw > barW - 2) fillw = barW - 2;
    d.fillRect(barX + 1, barY + 1, fillw, barH - 2, COL_ORP);
}

void enterInfo() {
    String dir = gBooks[gSel].dir;
    String epubPath = findEpub(dir);

    if (!ensureExtracted(dir, epubPath)) { gState = MENU; drawMenu(); return; }
    if (epubPath.length() > 0 && !SD.exists((dir + "/.rsvp_cover").c_str()) && metaLong(dir, "cover=") != 0) {
        bool had = canvasDrop();
        bool ok = extractCover(dir, epubPath);
        canvasRestore(had);
        metaAppend(dir, "cover=", ok ? 1 : 0);
    }

    gInfoDir = dir;
    gInfoOff = loadProgress(dir);
    loadMeta(dir);

    gState = INFO;
    drawInfo();
}

void openBook() {
    String dir = gBooks[gSel].dir;
    String readPath = dir + "/read.txt";
    String epubPath = findEpub(dir);

    if (!ensureExtracted(dir, epubPath)) { gState = MENU; drawMenu(); return; }

    gFile = SD.open(readPath.c_str(), FILE_READ);
    if (!gFile) { gState = MENU; drawMenu(); return; }

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
        String vs = line.substring(eq + 1);
        vs.trim();
        int val = vs.toInt();
        if (key == "wpm") {
            if (val >= MIN_WPM && val <= MAX_WPM) gWpm = val;
        } else if (key == "bright") {
            if (val >= MIN_BRIGHT && val <= MAX_BRIGHT) gBrightness = val;
        } else if (key == "peripheral_words") {
            gPeripheral = (vs == "true");
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
    f.print("peripheral_words=");
    f.println(gPeripheral ? "true" : "false");
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

static void drawGlyph5x7(lgfx::LGFXBase& d, const uint8_t* g, int x, int y, int s, uint16_t col) {
    for (int c = 0; c < 5; c++) {
        uint8_t bits = g[c];
        for (int r = 0; r < 7; r++) {
            if (!(bits & (1 << r))) continue;
            if (s == 1) d.drawPixel(x + c, y + r, col);
            else d.fillRect(x + c * s, y + r * s, s, s, col);
        }
    }
}

void drawText(lgfx::LGFXBase& d, const String& s, int x, int y, int scale, uint16_t col) {
    int n = s.length();
    int cw = 6 * scale;
    d.setTextSize(scale);
    d.setTextColor(col);
    for (int i = 0; i < n; i++) {
        int cx = x + i * cw;
        if (cx + cw <= 0 || cx >= SCR_W) continue;
        uint8_t c = (uint8_t)s.charAt(i);
        const uint8_t* g = cp1251Glyph(c);
        if (g) drawGlyph5x7(d, g, cx, y, scale, col);
        else { d.setCursor(cx, y); d.print((char)c); }
    }
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
                drawText(d, s, x, y, 1, tk.pos == curStart ? COL_ORP : COL_HIST);
                x += tk.len * 6 + 6;
            }
        }
    }

    d.drawFastHLine(TXT_X, DIV_Y, SCR_W - TXT_X * 2, COL_DIM);

    {
        d.setTextSize(FONT_SCALE);

        const String& fw = gWords[idx].text;
        int fwW = fw.length() * CHAR_W;
        int fx  = (SCR_W - fwW) / 2;

        if (gPeripheral) {
            int curTok = -1;
            for (int k = 0; k < (int)toks.size(); k++) {
                if (toks[k].pos == curStart) { curTok = k; break; }
            }

            if (curTok > 0) {
                int pl = toks[curTok - 1].len;
                String pv;
                pv.reserve(pl);
                for (int j = 0; j < pl; j++) pv += buf[toks[curTok - 1].off + j];
                drawText(d, pv, fx - CHAR_W - pl * CHAR_W, MAIN_Y, FONT_SCALE, COL_HIST);
            }
            if (curTok >= 0 && curTok + 1 < (int)toks.size()) {
                int nl2 = toks[curTok + 1].len;
                String nx;
                nx.reserve(nl2);
                for (int j = 0; j < nl2; j++) nx += buf[toks[curTok + 1].off + j];
                drawText(d, nx, fx + fwW + CHAR_W, MAIN_Y, FONT_SCALE, COL_HIST);
            }
        }

        drawText(d, fw, fx, MAIN_Y, FONT_SCALE, COL_TEXT);
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
