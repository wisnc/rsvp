import os

replacements = {
    # spaces / invisibles
    '\xa0': ' ', '\u2000': ' ', '\u2001': ' ', '\u2002': ' ', '\u2003': ' ',
    '\u2004': ' ', '\u2005': ' ', '\u2006': ' ', '\u2007': ' ', '\u2008': ' ',
    '\u2009': ' ', '\u200a': ' ', '\u202f': ' ', '\u205f': ' ', '\u3000': ' ',
    '\u200b': '', '\u200c': '', '\u200d': '', '\u2060': '', '\ufeff': '',
    '\u00ad': '',            # soft hyphen
    '\t': ' ',               # optional

    # single quotes / apostrophes
    '\u2018': "'", '\u2019': "'", '\u201a': "'", '\u201b': "'",
    '\u2032': "'", '\u2035': "'", '\u02b9': "'", '\u02bb': "'",
    '\u02bc': "'", '\u00b4': "'", '\u2039': "'", '\u203a': "'",

    # double quotes
    '\u201c': '"', '\u201d': '"', '\u201e': '"', '\u201f': '"',
    '\u00ab': '"', '\u00bb': '"', '\u2033': '"', '\u2036': '"', '\u3003': '"',

    # dashes / hyphens
    '\u2010': '-', '\u2011': '-', '\u2012': '-', '\u2013': '-', '\u2212': '-',
    '\u2014': '--', '\u2015': '--', '\u2e3a': '--', '\u2e3b': '---',

    # dots / bullets
    '\u2026': '...', '\u2025': '..', '\u00b7': '-', '\u2022': '-', '\u2023': '-',
    '\u2043': '-', '\u2219': '-', '\u25aa': '-', '\u25cf': '-', '\u25e6': '-',
    '\u2217': '*', '\u2020': '*', '\u2021': '*', '\u00b6': '', '\u00a7': '',

    # symbols
    '\u00b0': '', '\u00d7': 'x', '\u00f7': '/', '\u2044': '/',
    '\u00bc': '1/4', '\u00bd': '1/2', '\u00be': '3/4', '\u2153': '1/3', '\u2154': '2/3',
    '\u00a9': '(c)', '\u00ae': '(r)', '\u2122': '(tm)',
    '\u00a1': '!', '\u00bf': '?',
    '\u00a3': 'GBP', '\u20ac': 'EUR', '\u00a5': 'JPY', '\u00a2': 'c',  # currency, rough

    # combining accents (decomposed text)
    '\u0300': '', '\u0301': '', '\u0302': '', '\u0303': '', '\u0304': '',
    '\u0306': '', '\u0307': '', '\u0308': '', '\u030a': '', '\u030c': '',
    '\u0327': '', '\u0328': '',

    # accented latin — lowercase
    'à':'a','á':'a','â':'a','ã':'a','ä':'a','å':'a','ā':'a','ă':'a','ą':'a','æ':'ae',
    'ç':'c','ć':'c','ĉ':'c','ċ':'c','č':'c',
    'ď':'d','đ':'d','ð':'d',
    'è':'e','é':'e','ê':'e','ë':'e','ē':'e','ĕ':'e','ė':'e','ę':'e','ě':'e',
    'ĝ':'g','ğ':'g','ġ':'g','ģ':'g','ĥ':'h','ħ':'h',
    'ì':'i','í':'i','î':'i','ï':'i','ĩ':'i','ī':'i','ĭ':'i','į':'i','ı':'i',
    'ĵ':'j','ķ':'k',
    'ĺ':'l','ļ':'l','ľ':'l','ŀ':'l','ł':'l',
    'ñ':'n','ń':'n','ņ':'n','ň':'n','ŋ':'n',
    'ò':'o','ó':'o','ô':'o','õ':'o','ö':'o','ø':'o','ō':'o','ŏ':'o','ő':'o','œ':'oe',
    'ŕ':'r','ŗ':'r','ř':'r',
    'ś':'s','ŝ':'s','ş':'s','š':'s','ș':'s',
    'ţ':'t','ť':'t','ŧ':'t','ț':'t',
    'ù':'u','ú':'u','û':'u','ü':'u','ũ':'u','ū':'u','ŭ':'u','ů':'u','ű':'u','ų':'u',
    'ŵ':'w','ý':'y','ÿ':'y','ŷ':'y','ź':'z','ż':'z','ž':'z',
    'þ':'th','ß':'ss',

    # accented latin — uppercase
    'À':'A','Á':'A','Â':'A','Ã':'A','Ä':'A','Å':'A','Ā':'A','Ă':'A','Ą':'A','Æ':'AE',
    'Ç':'C','Ć':'C','Ĉ':'C','Ċ':'C','Č':'C','Ď':'D','Đ':'D','Ð':'D',
    'È':'E','É':'E','Ê':'E','Ë':'E','Ē':'E','Ĕ':'E','Ė':'E','Ę':'E','Ě':'E',
    'Ĝ':'G','Ğ':'G','Ġ':'G','Ģ':'G','Ĥ':'H','Ħ':'H',
    'Ì':'I','Í':'I','Î':'I','Ï':'I','Ĩ':'I','Ī':'I','Ĭ':'I','Į':'I','İ':'I',
    'Ĵ':'J','Ķ':'K','Ĺ':'L','Ļ':'L','Ľ':'L','Ŀ':'L','Ł':'L',
    'Ñ':'N','Ń':'N','Ņ':'N','Ň':'N','Ŋ':'N',
    'Ò':'O','Ó':'O','Ô':'O','Õ':'O','Ö':'O','Ø':'O','Ō':'O','Ŏ':'O','Ő':'O','Œ':'OE',
    'Ŕ':'R','Ŗ':'R','Ř':'R','Ś':'S','Ŝ':'S','Ş':'S','Š':'S','Ș':'S',
    'Ţ':'T','Ť':'T','Ŧ':'T','Ț':'T',
    'Ù':'U','Ú':'U','Û':'U','Ü':'U','Ũ':'U','Ū':'U','Ŭ':'U','Ů':'U','Ű':'U','Ų':'U',
    'Ŵ':'W','Ý':'Y','Ÿ':'Y','Ŷ':'Y','Ź':'Z','Ż':'Z','Ž':'Z','Þ':'Th',

    '|': ' ',                # your original
}

chunk_size = 4096
max_repl = max(len(k) for k in replacements)


def convert_file(path):
    tmp = path + '.ascii_tmp'
    try:
        with open(path, 'r', encoding='utf-8') as f_in, \
             open(tmp, 'w', encoding='utf-8') as f_out:
            leftover = ''
            while True:
                raw = f_in.read(chunk_size)
                buf = leftover + raw
                if not raw:
                    for old, new in replacements.items():
                        buf = buf.replace(old, new)
                    f_out.write(buf)
                    break
                safe = buf[:-max_repl]
                leftover = buf[-max_repl:]
                for old, new in replacements.items():
                    safe = safe.replace(old, new)
                f_out.write(safe)
        os.replace(tmp, path)
    except Exception:
        if os.path.exists(tmp):
            try:
                os.remove(tmp)
            except OSError:
                pass
        raise


try:
    base_dir = os.path.dirname(os.path.abspath(__file__))
except NameError:
    base_dir = os.getcwd()

print('base directory:', base_dir)

done = 0
failed = 0
for root, dirs, files in os.walk(base_dir):
    for name in files:
        if not name.lower().endswith('.txt'):
            continue
        path = os.path.join(root, name)
        rel = os.path.relpath(path, base_dir)
        try:
            convert_file(path)
            done += 1
            print('converted:', rel)
        except Exception as e:
            failed += 1
            print('skipped:  ', rel, '-', e)

print('done -- {} converted, {} skipped'.format(done, failed))
