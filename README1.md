# rsvp 3

---

## epub support

drop an .epub straight into the book folder. no read.txt needed

```
/ebooks/
├─ BookName1/
│  └─ book.epub
├─ BookName2/
│  ├─ read.txt
│  └─ prog.txt
```

on first open the cardputer converts the epub to read.txt itself. a big book takes a while, the screen counts chapters while it works

the conversion leaves behind a few files next to the epub

```
read.txt      the book as plain text
prog.txt      progress in character count
.rsvp_meta    title, author, total length, cover flag, and chapter offsets
.rsvp_cover   cover image pulled from the epub, jpg or png
```

## info screen

enter or space starts reading. backspace or ` goes back to the list

covers wider than 768 or taller than 1080 show as "can't render cover art"

## hotkeys

`/` next word

`,` prvious word

`space`, `ctrl`, `G0` pause playback

`;` increase wpm

`.` decrease wpm

`Fn + /` next chapter

`Fn + ,` previous chapter

plain .txt books have no chapter data, so the same keys jump 2% of the book forward or back instead

## Version History / Changelog

### 3.1

- cyrillic support is back. cp1251 glyph table for the read screen and the info screen, epub conversion maps cyrillic to cp1251

- fixed large epubs. conversion finished writing the text and then ran out of ram on the cover step, which rebooted the device and left the folder half done. the zip directory is now streamed instead of loaded whole, the manifest is resolved without copying it and only spine entries are kept in memory. peak ram during conversion is about half of what it was

- a half converted folder (read.txt present, no .rsvp_meta) is converted again automatically. no need to delete the epub

- chapter skip for epub books with Fn + / and Fn + , . the same keys do 2% jumps on .txt books

- percent read next to every book on the menu

- CORE_DEBUG_LEVEL dropped to 0 in platformio.ini. binary goes from 597k to 574k and fits a 576k partition

- cover extraction result is remembered in .rsvp_meta so books without a cover don't reparse the epub on every open

- backspace exits on both the old and the new M5Cardputer keyboard library

### 3.0

- epub support. conversion on device, chapter offsets, cover extraction

- info screen with cover art, title, author, chapter and progress
