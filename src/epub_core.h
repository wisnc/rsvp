#pragma once
#include <stdint.h>
#include <string.h>
#include <ctype.h>
#include <string>
#include <vector>
#include "miniz_tinfl.h"

#ifndef EPUB_LOG
#define EPUB_LOG(...) ((void)0)
#endif

struct Src {
    virtual ~Src() {}
    virtual uint64_t size() = 0;
    virtual size_t read(uint64_t off, void* buf, size_t n) = 0;
};

typedef void (*ByteSink)(const uint8_t* p, size_t n, void* user);

static bool inflateStream(Src& s, uint64_t dataOff, uint64_t compSize, ByteSink sink, void* user) {
    static uint8_t dict[TINFL_LZ_DICT_SIZE];
    uint8_t inbuf[2048];
    static tinfl_decompressor d; tinfl_init(&d);
    uint64_t pos = dataOff, inLeft = compSize;
    size_t dictOfs = 0;
    tinfl_status st = TINFL_STATUS_NEEDS_MORE_INPUT;
    for (;;) {
        size_t want = inLeft < sizeof(inbuf) ? (size_t)inLeft : sizeof(inbuf);
        size_t got = want ? s.read(pos, inbuf, want) : 0;
        if (want && got == 0) { EPUB_LOG("[epub] inflate: SD read 0 (pos=%lu left=%lu)\n",(unsigned long)pos,(unsigned long)inLeft); return false; }
        pos += got; inLeft -= got;
        const uint8_t* inNext = inbuf; size_t inAvail = got;
        int moreInput = (inLeft > 0);
        for (;;) {
            size_t inThis = inAvail;
            size_t outThis = TINFL_LZ_DICT_SIZE - dictOfs;
            int flags = moreInput ? TINFL_FLAG_HAS_MORE_INPUT : 0;
            st = tinfl_decompress(&d, inNext, &inThis, dict, dict + dictOfs, &outThis, flags);
            inNext += inThis; inAvail -= inThis;
            if (outThis) sink(dict + dictOfs, outThis, user);
            dictOfs = (dictOfs + outThis) & (TINFL_LZ_DICT_SIZE - 1);
            if (st == TINFL_STATUS_DONE) return true;
            if (st < 0) return false;
            if (st == TINFL_STATUS_NEEDS_MORE_INPUT) break;
            if (inAvail == 0 && st != TINFL_STATUS_HAS_MORE_OUTPUT) break;
        }
        if (inLeft == 0 && got == 0 && st == TINFL_STATUS_NEEDS_MORE_INPUT) return false;
    }
}

struct ZipEntry { std::string name; uint16_t method; uint64_t comp, uncomp, lho; };

static uint32_t rd32(const uint8_t* p){ return p[0]|(p[1]<<8)|(p[2]<<16)|((uint32_t)p[3]<<24); }
static uint16_t rd16(const uint8_t* p){ return p[0]|(p[1]<<8); }

static bool zipReadDir(Src& s, std::vector<ZipEntry>& out) {
    uint64_t fsize = s.size();
    EPUB_LOG("[epub] fsize=%lu\n",(unsigned long)fsize);
    if (fsize < 22) return false;
    uint64_t scan = fsize > 4096 ? 4096 : fsize;
    std::vector<uint8_t> tail(scan);
    s.read(fsize - scan, tail.data(), scan);
    long eocd = -1;
    for (long i = (long)scan - 22; i >= 0; i--) {
        if (tail[i]==0x50 && tail[i+1]==0x4b && tail[i+2]==0x05 && tail[i+3]==0x06) { eocd = i; break; }
    }
    if (eocd < 0) { EPUB_LOG("[epub] no EOCD in tail\n"); return false; }
    const uint8_t* e = &tail[eocd];
    uint16_t total = rd16(e+10);
    uint32_t cdSize = rd32(e+12), cdOff = rd32(e+16);
    EPUB_LOG("[epub] eocd@%ld cdSize=%u cdOff=%u n=%u\n",(long)eocd,cdSize,cdOff,total);
    if (cdSize==0 || cdSize==0xFFFFFFFFu || cdOff==0xFFFFFFFFu || total==0xFFFF || (uint64_t)cdOff + cdSize > fsize) {
        EPUB_LOG("[epub] bad or ZIP64 central dir, unsupported\n"); return false;
    }
    std::vector<uint8_t>().swap(tail);
    out.clear();
    out.reserve(total);
    uint64_t pos = cdOff, end = (uint64_t)cdOff + cdSize;
    uint8_t hdr[46];
    char name[512];
    while (pos + 46 <= end) {
        if (s.read(pos, hdr, 46) != 46) { EPUB_LOG("[epub] CD read short\n"); return false; }
        if (!(hdr[0]==0x50 && hdr[1]==0x4b && hdr[2]==0x01 && hdr[3]==0x02)) break;
        uint16_t nlen = rd16(hdr+28), elen = rd16(hdr+30), clen = rd16(hdr+32);
        ZipEntry ze;
        ze.method = rd16(hdr+10); ze.comp = rd32(hdr+20); ze.uncomp = rd32(hdr+24); ze.lho = rd32(hdr+42);
        size_t take = nlen < sizeof(name) ? nlen : sizeof(name);
        if (s.read(pos + 46, name, take) != take) { EPUB_LOG("[epub] CD name short\n"); return false; }
        ze.name.assign(name, take);
        out.push_back(ze);
        pos += 46 + nlen + elen + clen;
    }
    EPUB_LOG("[epub] entries=%u\n",(unsigned)out.size());
    return true;
}

static const ZipEntry* zipFind(const std::vector<ZipEntry>& v, const std::string& name) {
    for (auto& e : v) if (e.name == name) return &e;
    return 0;
}

static uint64_t zipDataOffset(Src& s, const ZipEntry& e) {
    uint8_t hdr[30];
    if (s.read(e.lho, hdr, 30) != 30) return 0;
    if (!(hdr[0]==0x50 && hdr[1]==0x4b && hdr[2]==0x03 && hdr[3]==0x04)) return 0;
    uint16_t nlen = rd16(hdr+26), elen = rd16(hdr+28);
    return e.lho + 30 + nlen + elen;
}

static void strSink(const uint8_t* p, size_t n, void* user) {
    ((std::string*)user)->append((const char*)p, n);
}

static bool zipExtract(Src& s, const ZipEntry& e, std::string& out) {
    uint64_t off = zipDataOffset(s, e);
    if (!off) return false;
    if (e.method == 0) {
        out.resize(e.comp);
        return s.read(off, &out[0], e.comp) == e.comp;
    }
    if (e.method == 8) {
        if (e.uncomp > 0 && e.uncomp < (1u << 20)) out.reserve((size_t)e.uncomp);
        return inflateStream(s, off, e.comp, strSink, &out);
    }
    return false;
}

static const char* cpAscii(uint32_t cp) {
    switch (cp) {
        case 0xA0: case 0x2000: case 0x2001: case 0x2002: case 0x2003:
        case 0x2004: case 0x2005: case 0x2006: case 0x2007: case 0x2008:
        case 0x2009: case 0x200A: case 0x202F: case 0x205F: case 0x3000: return " ";
        case 0x200B: case 0x200C: case 0x200D: case 0x2060: case 0xFEFF: case 0xAD: return "";
        case 0x2018: case 0x2019: case 0x201A: case 0x201B: case 0x2032:
        case 0xB4: case 0x2039: case 0x203A: return "'";
        case 0x201C: case 0x201D: case 0x201E: case 0x201F: case 0xAB: case 0xBB: case 0x2033: return "\"";
        case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2212: return "-";
        case 0x2014: case 0x2015: return "--";
        case 0x2026: return "...";
        case 0xB7: case 0x2022: case 0x2023: case 0x25AA: case 0x25CF: case 0x25E6: return "-";
        case 0x2044: case 0xF7: return "/";
        case 0xD7: return "x";
        case 0xBC: return "1/4"; case 0xBD: return "1/2"; case 0xBE: return "3/4";
        case 0xA9: return "(c)"; case 0xAE: return "(r)"; case 0x2122: return "(tm)";
        case 0xB0: return "";
        case 0xE0: case 0xE1: case 0xE2: case 0xE3: case 0xE4: case 0xE5: case 0x101: return "a";
        case 0xC0: case 0xC1: case 0xC2: case 0xC3: case 0xC4: case 0xC5: return "A";
        case 0xE7: case 0x107: case 0x10D: return "c"; case 0xC7: return "C";
        case 0xE8: case 0xE9: case 0xEA: case 0xEB: case 0x113: return "e";
        case 0xC8: case 0xC9: case 0xCA: case 0xCB: return "E";
        case 0xEC: case 0xED: case 0xEE: case 0xEF: case 0x12B: return "i";
        case 0xCC: case 0xCD: case 0xCE: case 0xCF: return "I";
        case 0xF1: case 0x144: return "n"; case 0xD1: return "N";
        case 0xF2: case 0xF3: case 0xF4: case 0xF5: case 0xF6: case 0xF8: case 0x14D: return "o";
        case 0xD2: case 0xD3: case 0xD4: case 0xD5: case 0xD6: case 0xD8: return "O";
        case 0xF9: case 0xFA: case 0xFB: case 0xFC: case 0x16B: return "u";
        case 0xD9: case 0xDA: case 0xDB: case 0xDC: return "U";
        case 0xFD: case 0xFF: case 0x177: return "y"; case 0xDD: return "Y";
        case 0xDF: return "ss"; case 0xE6: return "ae"; case 0xC6: return "AE";
        case 0x153: return "oe"; case 0x152: return "OE";
        case 0xFE: return "th"; case 0xF0: return "d"; case 0x161: return "s"; case 0x17E: return "z";
        default: return 0;
    }
}

static uint32_t namedEntity(const std::string& n) {
    if (n=="amp") return '&';
    if (n=="lt") return '<';
    if (n=="gt") return '>';
    if (n=="quot") return '"';
    if (n=="apos") return '\'';
    if (n=="nbsp") return 0xA0;
    if (n=="mdash") return 0x2014;
    if (n=="ndash") return 0x2013;
    if (n=="hellip") return 0x2026;
    if (n=="lsquo") return 0x2018;
    if (n=="rsquo") return 0x2019;
    if (n=="ldquo") return 0x201C;
    if (n=="rdquo") return 0x201D;
    if (n=="copy") return 0xA9;
    if (n=="reg") return 0xAE;
    if (n=="trade") return 0x2122;
    if (n=="deg") return 0xB0;
    if (n=="middot") return 0xB7;
    if (n=="bull") return 0x2022;
    if (n=="laquo") return 0xAB;
    if (n=="raquo") return 0xBB;
    if (n=="sbquo") return 0x201A;
    if (n=="bdquo") return 0x201E;
    if (n=="emsp") return 0x2003;
    if (n=="ensp") return 0x2002;
    if (n=="thinsp") return 0x2009;
    if (n=="shy") return 0xAD;
    if (n=="times") return 0xD7;
    if (n=="frac12") return 0xBD;
    return 0xFFFFFFFF;
}

struct TextOut {
    ByteSink sink; void* user; uint64_t count; int pending; bool started;
    void init(ByteSink s, void* u){ sink=s; user=u; count=0; pending=0; started=false; }
    void put(char c){ uint8_t b=(uint8_t)c; sink(&b,1,user); count++; }
    void flush(){ if(!started) return; if(pending==1) put(' '); else if(pending==2) put('\n'); else if(pending==3){ put('\n'); put('\n'); } pending=0; }
    void reqSpace(){ if(started && pending<1) pending=1; }
    void reqNL(){ if(started && pending<2) pending=2; }
    void reqBlank(){ if(started && pending<3) pending=3; }
    void emitCh(char c){ flush(); put(c); started=true; }
    void emitCp(uint32_t cp){
        if (cp < 0x80) {
            if (cp=='\t'||cp=='\n'||cp=='\r'||cp==' ') { reqSpace(); return; }
            if (cp < 0x20) return;
            emitCh((char)cp); return;
        }
        if (cp >= 0x410 && cp <= 0x44F) { emitCh((char)(0xC0 + (cp - 0x410))); return; }
        if (cp == 0x401) { emitCh((char)0xA8); return; }
        if (cp == 0x451) { emitCh((char)0xB8); return; }
        const char* r = cpAscii(cp);
        if (!r) { emitCh('?'); return; }
        while (*r) emitCh(*r++);
    }
};

static std::string normalizeAscii(const std::string& in) {
    std::string o;
    TextOut t;
    t.init([](const uint8_t* p, size_t n, void* u){ ((std::string*)u)->append((const char*)p, n); }, &o);
    size_t i = 0;
    while (i < in.size()) {
        unsigned char c = in[i]; uint32_t cp; int a;
        if (c < 0x80) { cp = c; a = 1; }
        else if ((c&0xE0)==0xC0 && i+1<in.size()) { cp=((c&0x1F)<<6)|(in[i+1]&0x3F); a=2; }
        else if ((c&0xF0)==0xE0 && i+2<in.size()) { cp=((c&0x0F)<<12)|((in[i+1]&0x3F)<<6)|(in[i+2]&0x3F); a=3; }
        else if ((c&0xF8)==0xF0 && i+3<in.size()) { cp=((uint32_t)(c&0x07)<<18)|((in[i+1]&0x3F)<<12)|((in[i+2]&0x3F)<<6)|(in[i+3]&0x3F); a=4; }
        else { cp=c; a=1; }
        t.emitCp(cp); i += a;
    }
    return o;
}

static bool isBlockTag(const std::string& t) {
    return t=="p"||t=="div"||t=="br"||t=="li"||t=="tr"||t=="blockquote"||
           t=="section"||t=="article"||t=="ul"||t=="ol"||t=="table"||t=="hr"||
           t=="figure"||t=="figcaption"||t=="pre";
}
static bool isHeaderTag(const std::string& t){ return t.size()==2 && t[0]=='h' && t[1]>='1' && t[1]<='6'; }

static void stripHtml(const std::string& html, TextOut& out) {
    size_t i = 0, n = html.size();
    while (i < n) {
        unsigned char c = html[i];
        if (c == '<') {
            size_t j = i + 1;
            bool close = false;
            if (j < n && html[j] == '/') { close = true; j++; }
            if (j < n && html[j] == '!') { size_t k = html.find('>', i); i = (k==std::string::npos)? n : k+1; continue; }
            std::string name;
            while (j < n) { char t = html[j]; if (t=='>'||t==' '||t=='\t'||t=='\n'||t=='\r'||t=='/') break; name += (char)tolower(t); j++; }
            size_t endTag = html.find('>', i);
            size_t next = (endTag==std::string::npos)? n : endTag+1;
            bool selfClose = (next>=2 && html[next-2]=='/');
            if (!close && !selfClose && (name=="script" || name=="style" || name=="head")) {
                std::string closeTag = "</" + name;
                size_t found = std::string::npos;
                for (size_t z = next; z + closeTag.size() <= n; z++) {
                    bool m = true;
                    for (size_t q=0;q<closeTag.size();q++){ char a=tolower(html[z+q]); if(a!=closeTag[q]){m=false;break;} }
                    if (m){ found=z; break; }
                }
                if (found==std::string::npos) { i = n; continue; }
                size_t gt = html.find('>', found);
                i = (gt==std::string::npos)? n : gt+1;
                continue;
            }
            if (isHeaderTag(name)) out.reqBlank();
            else if (isBlockTag(name)) { if (name=="br"||name=="hr") out.reqNL(); else out.reqBlank(); }
            i = next;
            continue;
        }
        if (c == '&') {
            size_t sc = html.find(';', i);
            if (sc != std::string::npos && sc - i <= 12) {
                std::string ent = html.substr(i+1, sc-i-1);
                uint32_t cp = 0xFFFFFFFF;
                if (!ent.empty() && ent[0]=='#') {
                    if (ent.size()>1 && (ent[1]=='x'||ent[1]=='X')) cp = strtoul(ent.c_str()+2, 0, 16);
                    else cp = strtoul(ent.c_str()+1, 0, 10);
                } else cp = namedEntity(ent);
                if (cp != 0xFFFFFFFF) { out.emitCp(cp); i = sc+1; continue; }
            }
            out.emitCh('&'); i++; continue;
        }
        uint32_t cp; int adv;
        if (c < 0x80) { cp = c; adv = 1; }
        else if ((c&0xE0)==0xC0 && i+1<n) { cp = ((c&0x1F)<<6)|(html[i+1]&0x3F); adv=2; }
        else if ((c&0xF0)==0xE0 && i+2<n) { cp = ((c&0x0F)<<12)|((html[i+1]&0x3F)<<6)|(html[i+2]&0x3F); adv=3; }
        else if ((c&0xF8)==0xF0 && i+3<n) { cp = ((uint32_t)(c&0x07)<<18)|((html[i+1]&0x3F)<<12)|((html[i+2]&0x3F)<<6)|(html[i+3]&0x3F); adv=4; }
        else { cp = c; adv = 1; }
        out.emitCp(cp);
        i += adv;
    }
}

struct HtmlStripper {
    int st;
    std::string tagbuf;
    std::string entbuf;
    std::string skipClose;
    size_t skipPos;
    bool skipToGt;
    uint8_t u8[4]; int u8need, u8have;
    HtmlStripper(): st(0), skipPos(0), skipToGt(false), u8need(0), u8have(0) {}

    uint32_t decodeU8() {
        if (u8need==2) return ((u8[0]&0x1F)<<6)|(u8[1]&0x3F);
        if (u8need==3) return ((u8[0]&0x0F)<<12)|((u8[1]&0x3F)<<6)|(u8[2]&0x3F);
        return ((uint32_t)(u8[0]&0x07)<<18)|((u8[1]&0x3F)<<12)|((u8[2]&0x3F)<<6)|(u8[3]&0x3F);
    }
    void flushU8(TextOut& out){ if (u8need){ out.emitCh('?'); u8need=0; u8have=0; } }
    void emitByteAsText(uint8_t c, TextOut& out) {
        if (u8need) {
            if ((c&0xC0)==0x80) { u8[u8have++]=c; if (u8have==u8need){ out.emitCp(decodeU8()); u8need=0; u8have=0; } return; }
            out.emitCh('?'); u8need=0; u8have=0;
        }
        if (c<0x80) out.emitCp(c);
        else if ((c&0xE0)==0xC0){ u8[0]=c; u8have=1; u8need=2; }
        else if ((c&0xF0)==0xE0){ u8[0]=c; u8have=1; u8need=3; }
        else if ((c&0xF8)==0xF0){ u8[0]=c; u8have=1; u8need=4; }
        else out.emitCp('?');
    }
    void decodeEntity(TextOut& out) {
        uint32_t cp = 0xFFFFFFFF;
        if (!entbuf.empty() && entbuf[0]=='#') {
            if (entbuf.size()>1 && (entbuf[1]=='x'||entbuf[1]=='X')) cp = strtoul(entbuf.c_str()+2, 0, 16);
            else cp = strtoul(entbuf.c_str()+1, 0, 10);
        } else cp = namedEntity(entbuf);
        if (cp != 0xFFFFFFFF) out.emitCp(cp);
        else { out.emitCh('&'); for (size_t z=0;z<entbuf.size();z++) emitByteAsText((uint8_t)entbuf[z], out); out.emitCh(';'); }
    }
    void handleTag(TextOut& out) {
        const std::string& t = tagbuf;
        size_t i=0; bool close=false;
        if (i<t.size() && t[i]=='/'){ close=true; i++; }
        if (i<t.size() && t[i]=='!') return;
        std::string name;
        while (i<t.size()){ char c=t[i]; if (c==' '||c=='\t'||c=='\n'||c=='\r'||c=='/') break; name+=(char)tolower((unsigned char)c); i++; }
        bool selfClose = (!t.empty() && t[t.size()-1]=='/');
        if (!close && !selfClose && (name=="script"||name=="style"||name=="head")) { st=2; skipClose="</"+name; skipPos=0; skipToGt=false; return; }
        if (isHeaderTag(name)) out.reqBlank();
        else if (isBlockTag(name)) { if (name=="br"||name=="hr") out.reqNL(); else out.reqBlank(); }
    }
    void feed(const uint8_t* d, size_t n, TextOut& out) {
        for (size_t k=0;k<n;k++){
            uint8_t c=d[k];
            if (st==2) {
                if (skipToGt) { if (c=='>'){ st=0; skipToGt=false; } continue; }
                char lc=(char)tolower(c);
                if (lc==skipClose[skipPos]) { skipPos++; if (skipPos==skipClose.size()) skipToGt=true; }
                else skipPos = (lc==skipClose[0])?1:0;
                continue;
            }
            if (st==1) {
                if (c=='>') { handleTag(out); if (st==1) st=0; tagbuf.clear(); }
                else if (tagbuf.size()<4096) tagbuf+=(char)c;
                continue;
            }
            if (st==3) {
                if (c==';') { decodeEntity(out); st=0; entbuf.clear(); continue; }
                if ((c=='#'||(c>='0'&&c<='9')||(c>='a'&&c<='z')||(c>='A'&&c<='Z')) && entbuf.size()<12) { entbuf+=(char)c; continue; }
                out.emitCh('&');
                for (size_t z=0;z<entbuf.size();z++) emitByteAsText((uint8_t)entbuf[z], out);
                entbuf.clear(); st=0;
            }
            if (c=='<') { flushU8(out); st=1; tagbuf.clear(); }
            else if (c=='&') { flushU8(out); st=3; entbuf.clear(); }
            else if (c==' '||c=='\t'||c=='\n'||c=='\r') { flushU8(out); out.reqSpace(); }
            else emitByteAsText(c, out);
        }
    }
    void finish(TextOut& out) {
        if (st==3) { out.emitCh('&'); for (size_t z=0;z<entbuf.size();z++) emitByteAsText((uint8_t)entbuf[z], out); entbuf.clear(); }
        flushU8(out);
        st=0;
    }
};

static bool zipExtractToSink(Src& s, const ZipEntry& e, ByteSink sink, void* user) {
    uint64_t off = zipDataOffset(s, e);
    if (!off) return false;
    if (e.method == 0) {
        uint8_t buf[2048]; uint64_t left = e.comp, pos = off;
        while (left) { size_t want = left<sizeof(buf)?(size_t)left:sizeof(buf); size_t got=s.read(pos,buf,want); if(!got) return false; sink(buf,got,user); pos+=got; left-=got; }
        return true;
    }
    if (e.method == 8) return inflateStream(s, off, e.comp, sink, user);
    return false;
}

struct StripSink { HtmlStripper strip; TextOut* out; };
static void stripFeedSink(const uint8_t* p, size_t n, void* user) {
    StripSink* s = (StripSink*)user;
    s->strip.feed(p, n, *s->out);
}

static std::string attrVal(const std::string& s, size_t tagStart, const char* attr) {
    size_t gt = s.find('>', tagStart);
    size_t lim = (gt==std::string::npos)? s.size() : gt;
    std::string key = std::string(attr) + "=";
    size_t p = s.find(key, tagStart);
    if (p==std::string::npos || p>lim) return "";
    p += key.size();
    if (p>=s.size()) return "";
    char q = s[p];
    if (q!='"' && q!='\'') return "";
    size_t e = s.find(q, p+1);
    if (e==std::string::npos) return "";
    return s.substr(p+1, e-p-1);
}
static std::string tagText(const std::string& s, const char* open) {
    size_t p = s.find(open);
    if (p==std::string::npos) return "";
    size_t gt = s.find('>', p);
    if (gt==std::string::npos) return "";
    size_t e = s.find('<', gt+1);
    if (e==std::string::npos) return "";
    std::string t = s.substr(gt+1, e-gt-1);
    size_t a=t.find_first_not_of(" \t\r\n"); size_t b=t.find_last_not_of(" \t\r\n");
    if (a==std::string::npos) return "";
    return t.substr(a, b-a+1);
}
static std::string dirOf(const std::string& path){ size_t s=path.rfind('/'); return s==std::string::npos? "" : path.substr(0,s+1); }
static std::string urlDecode(const std::string& s){
    std::string o; for(size_t i=0;i<s.size();i++){ if(s[i]=='%'&&i+2<s.size()){ int h=strtol(s.substr(i+1,2).c_str(),0,16); o+=(char)h; i+=2;} else if(s[i]=='#'){break;} else o+=s[i]; } return o;
}
static std::string joinPath(const std::string& base, const std::string& rel){
    std::string r = urlDecode(rel);
    std::string path = base + r;
    std::vector<std::string> parts; std::string cur;
    for (size_t i=0;i<=path.size();i++){
        if (i==path.size()||path[i]=='/'){ if(cur=="."){} else if(cur==".."){ if(!parts.empty()) parts.pop_back(); } else if(!cur.empty()) parts.push_back(cur); cur.clear(); }
        else cur+=path[i];
    }
    std::string out; for(size_t i=0;i<parts.size();i++){ if(i) out+="/"; out+=parts[i]; }
    return out;
}

static bool isItemTag(const std::string& opf, size_t p) {
    char after = p + 5 < opf.size() ? opf[p + 5] : '>';
    return after==' '||after=='\t'||after=='\n'||after=='\r';
}

static std::string lowerStr(std::string s) { for (size_t j=0;j<s.size();j++) s[j]=(char)tolower((unsigned char)s[j]); return s; }

static std::string coverHref(const std::string& opf) {
    std::string cid;
    size_t mp=opf.find("name=\"cover\"");
    if(mp==std::string::npos) mp=opf.find("name='cover'");
    if(mp!=std::string::npos){
        size_t tag=opf.rfind("<meta",mp);
        if(tag!=std::string::npos) cid=attrVal(opf,tag,"content");
    }
    size_t p;
    if(!cid.empty()){
        p=0;
        while((p=opf.find("<item",p))!=std::string::npos){
            if(isItemTag(opf,p) && attrVal(opf,p,"id")==cid){ std::string h=attrVal(opf,p,"href"); if(!h.empty()) return h; }
            p+=5;
        }
    }
    p=0;
    while((p=opf.find("<item",p))!=std::string::npos){
        if(isItemTag(opf,p) && lowerStr(attrVal(opf,p,"properties")).find("cover-image")!=std::string::npos){ std::string h=attrVal(opf,p,"href"); if(!h.empty()) return h; }
        p+=5;
    }
    p=0;
    while((p=opf.find("<item",p))!=std::string::npos){
        if(isItemTag(opf,p)){
            std::string h=attrVal(opf,p,"href");
            if(!h.empty() && lowerStr(attrVal(opf,p,"media-type")).find("image")!=std::string::npos &&
               (lowerStr(attrVal(opf,p,"id")).find("cover")!=std::string::npos || lowerStr(h).find("cover")!=std::string::npos)) return h;
        }
        p+=5;
    }
    return "";
}

static std::string opfPathOf(const std::string& container) {
    size_t rf = 0;
    while ((rf = container.find("<rootfile", rf)) != std::string::npos) {
        char a = rf + 9 < container.size() ? container[rf + 9] : '>';
        if (a==' '||a=='\t'||a=='\n'||a=='\r'||a=='/'||a=='>') break;
        rf += 9;
    }
    if (rf == std::string::npos) return "";
    return attrVal(container, rf, "full-path");
}

static bool epubLoadOpf(Src& src, std::vector<ZipEntry>& zdir, std::string& opfPath, std::string& opf) {
    if (!zipReadDir(src, zdir)) { EPUB_LOG("[epub] FAIL zipReadDir\n"); return false; }
    const ZipEntry* ce = zipFind(zdir, std::string("META-INF/container.xml"));
    if (!ce) { EPUB_LOG("[epub] FAIL no container.xml\n"); return false; }
    {
        std::string container;
        if (!zipExtract(src, *ce, container)) { EPUB_LOG("[epub] FAIL extract container\n"); return false; }
        opfPath = opfPathOf(container);
    }
    if (opfPath.empty()) { EPUB_LOG("[epub] FAIL no opf path\n"); return false; }
    const ZipEntry* oe = zipFind(zdir, opfPath);
    if (!oe) { EPUB_LOG("[epub] FAIL opf not in zip\n"); return false; }
    if (!zipExtract(src, *oe, opf)) { EPUB_LOG("[epub] FAIL extract opf\n"); return false; }
    EPUB_LOG("[epub] opf %s (%u bytes)\n", opfPath.c_str(), (unsigned)opf.size());
    return true;
}

static int zipIndex(const std::vector<ZipEntry>& v, const std::string& name) {
    for (size_t i = 0; i < v.size(); i++) if (v[i].name == name) return (int)i;
    return -1;
}

static void resolveSpine(const std::string& opf, const std::string& opfDir, const std::vector<ZipEntry>& zdir, std::vector<int>& spineIdx) {
    std::vector<std::string> idrefs;
    {
        size_t ss = opf.find("<spine"), se = opf.find("</spine>"), p = ss;
        while (ss != std::string::npos && (p = opf.find("<itemref", p)) != std::string::npos && (se == std::string::npos || p < se)) {
            std::string idref = attrVal(opf, p, "idref");
            if (!idref.empty()) idrefs.push_back(idref);
            p += 8;
        }
    }
    spineIdx.assign(idrefs.size(), -1);
    size_t p = 0;
    while ((p = opf.find("<item", p)) != std::string::npos) {
        if (isItemTag(opf, p)) {
            std::string id = attrVal(opf, p, "id");
            if (!id.empty()) {
                for (size_t k = 0; k < idrefs.size(); k++) {
                    if (spineIdx[k] >= 0 || idrefs[k] != id) continue;
                    std::string href = attrVal(opf, p, "href");
                    if (!href.empty()) spineIdx[k] = zipIndex(zdir, joinPath(opfDir, href));
                    if (spineIdx[k] < 0) EPUB_LOG("[epub] miss spine file: %s\n", href.c_str());
                }
            }
        }
        p += 5;
    }
    EPUB_LOG("[epub] spine=%u\n", (unsigned)idrefs.size());
}
