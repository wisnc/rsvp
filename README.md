# rsvp
---

Rapid Serial Visual Presentation - an ebook reader for the m5cardputer

---

home screen

<img src="rsvp1.jpg" width="500">

read screen

<img src="read.gif" width="500">

---

## Building

```
git clone https://github.com/wisnc/rsvp
cd rsvp
pio run
```

## Installing

Just flash it with any launcher. or from the M5Burner.

Check releases for the latest binary

or [shameless plug](https://github.com/wisnc/crub)

## How to use (old)

your SD must have the file structure for ebooks as

```
/ebooks/
├─ BookName1/
│  ├─ read.txt
│  └─ prog.txt
├─ BookName2/
│  ├─ read.txt
│  └─ prog.txt
├─ Sample_Book_A/
│  ├─ read.txt
│  └─ prog.txt
│
└─ascii-auto.py
```
read.txt should contain the entire book in text file. each word will be separated by either a newline or space
prog.txt should be 0 for no progress. the value refers to the amount of characters already read as progress

running ascii-auto.py through python or micropython `run('ascii-auto.py')` will automatically convert all your characters to ascii-printable and direct equivalent

easiest way to convert to .txt is through https://convertio.co/epub-txt/
otherwise use the epub2txt.py from the project

## epub support

drop an .epub straight into the book folder. rsvp will generate the .txt

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

## hotkeys

`/` next word

`,` prvious word

`space`, `ctrl`, `G0` pause playback

`;` increase wpm

`.` decrease wpm

`=` increase brightness

`-` decrease brightness

`p` display peripheral words

`Fn + /` next chapter

`Fn + ,` previous chapter

plain .txt books have no chapter data, so the same keys jump 2% of the book forward or back instead

## ASCII conversion

as you know, the default encoding or font of the cardputer is limited by ascii printable. so books that contain UTF-8 characters cannot be displayed, provided on this repo is a script that can easily convert all the characters in your text to alternative characters that can be displayed by the cardputer

```
cd ebooks
python ascii-auto.py
```

run this command in /ebooks/

this script checks out all directories beside it and converts all .txt

**fun fact!** you can run this python script on micropython within the cardputer. use a micropython firmware

## Version History / Changelog

### 3.1

- chunked extraction / conversion to prevent heap overflow

- cyrillic support return (accidentally removed it on 3.0)

- chapter navigation with fn arrowkeys

- added progress on menu


### 3.0

- epub support. conversion on device, chapter offsets, cover extraction

- info screen with cover art, title, author, chapter and progress


### 2.3

- added more keys for pause

### 2.2

- added peripheral words feature

### 2.1

- persistent settings
  
- start on pause

### 2.0

- massive change on the UI

- upload python tool

### 1.0

- Public release
- GitHub repository created
