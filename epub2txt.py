import sys
import glob
from pathlib import Path

from ebooklib import epub, ITEM_DOCUMENT
from bs4 import BeautifulSoup


def epub_to_text(epub_path):
    book = epub.read_epub(epub_path)
    chunks = []

    for item in book.get_items_of_type(ITEM_DOCUMENT):
        soup = BeautifulSoup(item.get_content(), 'html.parser')
        text = soup.get_text(separator=' ', strip=True)
        if text:
            chunks.append(text)

    return '\n\n'.join(chunks)


def main():
    if len(sys.argv) > 1:
        files = [sys.argv[1]]
    else:
        files = glob.glob('*.epub')

    if not files:
        print('No .epub files found.')
        return

    for f in files:
        print(f'Converting: {f}')
        text = epub_to_text(f)
        out = Path(f).stem
        out_dir = Path(out)
        out_dir.mkdir(exist_ok=True)
        (out_dir / 'read.txt').write_text(text, encoding='utf-8')
        (out_dir / 'prog.txt').write_text('0\n', encoding='utf-8')
        print(f'  -> {out_dir}/read.txt ({len(text)} bytes)')


if __name__ == '__main__':
    main()
