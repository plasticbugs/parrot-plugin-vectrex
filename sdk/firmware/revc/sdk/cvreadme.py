#!/usr/bin/env python3
"""cvreadme -- a plugin's README.md as the cart shows it.

    cvreadme.py README.md [-o README.txt] [--width 30]

The menu's text pages are 30 characters wide and its font is ASCII, so a
plugin's README (GitHub-flavoured Markdown, as its author wrote it) is turned
into plain text once, when the plugin is built: mkcvp.py --readme does it
(cvcart_plugin(... README README.md)) and puts the text after the image in
the .CVP (docs/plugins.md, "The README"). Without -o the text is printed, to
see what the cart will show.

What comes out: printable ASCII (0x20-0x7E) and "\\n" only, no line longer
than the width, at most 16384 bytes.

    headings          in capitals, a blank line before and after
    paragraphs        reflowed to the width
    lists             "- " for -, * and +; "1. " numbered from the first
                      number; the text hangs under its first word, and a
                      list inside an item starts there too (2 further in
                      for a bullet's)
    links             their text, then the URL; a URL that doesn't fit the
                      rest of the line starts a line of its own, and one
                      longer than a line is cut at the width. A URL alone
                      (<https://...>, or bare) likewise. A link inside the
                      project (docs/x.md, #section) is its text alone
    inline code       its text
    code blocks       as they are (fenced or indented), each line cut at
                      the width
    emphasis          **, __, *, _ and ~~ dropped
    images            [image: alt]
    tables            a "header: value" line for each cell (an empty cell
                      has none), a blank line between rows
    block quotes      2 further in
    raw HTML          the tags dropped, their text kept; <br> ends a line
    rules             a line of -
    outside ASCII     curly quotes, dashes, the ellipsis and accented
                      letters as their ASCII look-alikes; anything else ?

Over 16384 bytes it is cut at a line's end, and its last line says so.
The same input always gives the same output (the plugin registry shows it
as it is). No dependencies.
"""
import html
import re
import sys
import unicodedata

WIDTH = 30                                 # the menu's text pages (sdplayer.c: tline)
MAX_BYTES = 16384                          # the .CVP header's limit
CUT_LINE = "[The README is cut here]"
NARROW = 8                                 # a list or a quote this narrow isn't indented further

TRANSLIT = {
    "\u2018": "'", "\u2019": "'", "\u201a": "'", "\u201b": "'", "\u2032": "'", "\u00b4": "'", "\u02bc": "'",
    "\u201c": '"', "\u201d": '"', "\u201e": '"', "\u201f": '"', "\u2033": '"', "\u00ab": '"', "\u00bb": '"',
    "\u2010": "-", "\u2011": "-", "\u2012": "-", "\u2013": "-", "\u2014": "--", "\u2015": "--", "\u2212": "-",
    "\u2026": "...", "\u00a0": " ", "\u2002": " ", "\u2003": " ", "\u2009": " ", "\u202f": " ", "\u3000": " ",
    "\u2022": "*", "\u00b7": "*", "\u00d7": "x", "\u00f7": "/", "\u00a9": "(c)", "\u00ae": "(R)", "\u2122": "(TM)",
    "\u2190": "<-", "\u2192": "->", "\u21d2": "=>", "\u2264": "<=", "\u2265": ">=", "\u2260": "!=", "\u00b1": "+/-",
    "\u00bd": "1/2", "\u00bc": "1/4", "\u00be": "3/4", "\u00b0": " deg", "\u00b5": "u", "\u03bc": "u",
    "\u00df": "ss", "\u00e6": "ae", "\u00c6": "AE", "\u00f8": "o", "\u00d8": "O", "\u0153": "oe", "\u0152": "OE",
    "\u0142": "l", "\u0141": "L", "\u0111": "d", "\u0110": "D", "\u00fe": "th", "\u00de": "Th", "\u00f0": "d",
    "\u00d0": "D", "\u0131": "i", "\u00a1": "!", "\u00bf": "?", "\u20ac": "EUR", "\u00a3": "GBP", "\u00a5": "JPY",
    "\u2713": "x", "\u2714": "x",
}
DROPPED = {"\u200b", "\u200c", "\u200d", "\u2060", "\ufeff", "\ufe0e", "\ufe0f", "\u00ad"}


def ascii_only(s):
    """printable ASCII for every character: a look-alike, or ?"""
    out = []
    for ch in s:
        if " " <= ch <= "~" or ch == "\n":
            out.append(ch)
        elif ch == "\t":
            out.append(" ")
        elif ch in DROPPED or ch < " " or ch == "\x7f":
            pass
        elif ch in TRANSLIT:
            out.append(TRANSLIT[ch])
        else:
            base = "".join(c for c in unicodedata.normalize("NFKD", ch) if not unicodedata.combining(c))
            base = "".join(TRANSLIT.get(c, c) for c in base)
            out.append(base if base and all(" " <= c <= "~" for c in base) else "?")
    return "".join(out)


# ---- inline ------------------------------------------------------------------------
# What must come through untouched (a code span's text, a URL, an escaped
# character) is put aside as one private-use character and put back last.
PUA = 0xE000
ABS_URL = re.compile(r"(?i)^(https?://|ftp://|mailto:|www\.)")
BARE_URL = re.compile(r"(?i)\b(?:https?://|ftp://|www\.)[^\s<>]+")
HTML_TAG = re.compile(r"</?[A-Za-z][A-Za-z0-9-]*(?:\s[^<>]*)?/?>")
EMPHASIS = (
    re.compile(r"\*\*(?=[^\s*])(.+?)(?<=[^\s*])\*\*", re.S),
    re.compile(r"(?<![A-Za-z0-9])__(?=\S)(.+?)(?<=\S)__(?![A-Za-z0-9])", re.S),
    re.compile(r"~~(?=\S)(.+?)(?<=\S)~~", re.S),
    re.compile(r"(?<![A-Za-z0-9*])\*(?=[^\s*])(.+?)(?<=[^\s*])\*(?![A-Za-z0-9*])", re.S),
    re.compile(r"(?<![A-Za-z0-9_])_(?=[^\s_])(.+?)(?<=[^\s_])_(?![A-Za-z0-9_])", re.S),
)


class Inline:
    def __init__(self, refs):
        self.refs = refs
        self.kept = []

    def keep(self, s):
        self.kept.append(s)
        return chr(PUA + len(self.kept) - 1)

    def link(self, text, url):
        """a link as the cart shows it: the text, then where it goes"""
        url = url.strip()
        if url.startswith("<") and url.endswith(">"):
            url = url[1:-1]
        plain = self.restore(text).strip()
        if not ABS_URL.match(url):
            return text                    # inside the project: nothing to type in
        shown = url[7:] if url.lower().startswith("mailto:") else url
        bare = re.sub(r"(?i)^https?://", "", shown).rstrip("/")
        if not plain or re.sub(r"(?i)^https?://", "", plain).rstrip("/") == bare:
            return self.keep(shown)
        return text + " " + self.keep(shown)

    def restore(self, s):
        for _ in range(4):                 # (one kept inside another)
            if not any(PUA <= ord(c) < PUA + len(self.kept) for c in s):
                break
            s = "".join(self.kept[ord(c) - PUA] if PUA <= ord(c) < PUA + len(self.kept) else c for c in s)
        return s

    def run(self, s):
        s = "".join(c for c in s if not 0xE000 <= ord(c) <= 0xF8FF)
        s = re.sub(r"(`+)(.+?)\1(?!`)", lambda m: self.keep(m.group(2).strip()), s, flags=re.S)
        s = re.sub(r"\\([!-/:-@\[-`{-~])", lambda m: self.keep(m.group(1)), s)
        s = re.sub(r"<!--.*?-->", "", s, flags=re.S)
        # images, then links (a badge is an image inside a link)
        def image(m):
            alt = m.group(1).strip()
            return self.keep("[image: " + alt + "]" if alt else "[image]")
        s = re.sub(r"!\[([^\]]*)\]\([^)]*\)", image, s)
        s = re.sub(r"!\[([^\]]*)\]\[[^\]]*\]", image, s)
        s = re.sub(r"(?i)<img\b[^<>]*?\balt\s*=\s*\"([^\"]*)\"[^<>]*>", image, s)
        s = re.sub(r"(?i)<img\b[^<>]*>", lambda m: self.keep("[image]"), s)
        s = re.sub(r"\[([^\]]+)\]\(\s*(<[^>]*>|[^)\s]*)(?:\s+(?:\"[^\"]*\"|'[^']*'))?\s*\)",
                   lambda m: self.link(m.group(1), m.group(2)), s)
        def ref(m):
            key = ((m.group(2) if m.lastindex > 1 else "") or m.group(1)).strip().lower()
            return self.link(m.group(1), self.refs[key]) if key in self.refs else m.group(0)
        s = re.sub(r"\[([^\]]+)\]\[([^\]]*)\]", ref, s)
        s = re.sub(r"\[([^\]]+)\](?![\[(])", ref, s)
        s = re.sub(r"<((?:https?://|ftp://|mailto:)[^\s<>]+)>", lambda m: self.link("", m.group(1)), s)
        s = re.sub(r"<([A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,})>", lambda m: self.keep(m.group(1)), s)
        def bare(m):
            url = m.group(0)
            tail = ""
            while url and (url[-1] in ".,;:!?'\"*_~" or (url[-1] == ")" and url.count(")") > url.count("("))):
                tail = url[-1] + tail
                url = url[:-1]
            return self.keep(url) + tail
        s = BARE_URL.sub(bare, s)
        s = re.sub(r"(?i)<br\s*/?>", "\n", s)
        s = HTML_TAG.sub("", s)
        for _ in range(8):
            before = s
            for rx in EMPHASIS:
                s = rx.sub(r"\1", s)
            if s == before:
                break
        s = html.unescape(s)
        return ascii_only(self.restore(s))


# ---- wrapping ----------------------------------------------------------------------
def wrap(text, width):
    """text as lines of at most width: words kept whole; one that doesn't fit
    the rest of a line starts the next, and one longer than a line is cut"""
    out = []
    for para in text.split("\n"):
        line = ""
        for word in para.split():
            if line and len(line) + 1 + len(word) <= width:
                line += " " + word
                continue
            if line:
                out.append(line)
            while len(word) > width:
                out.append(word[:width])
                word = word[width:]
            line = word
        out.append(line)
    while len(out) > 1 and not out[-1]:
        out.pop()
    return out


def hang(lines, first, rest):
    return [(first if i == 0 else rest) + ln if ln else "" for i, ln in enumerate(lines)]


# ---- blocks ------------------------------------------------------------------------
FENCE = re.compile(r"^( {0,3})(`{3,}|~{3,})(.*)$")
ATX = re.compile(r"^ {0,3}(#{1,6})(?:[ \t]+(.*?))?(?:[ \t]+#+)?[ \t]*$")
RULE = re.compile(r"^ {0,3}([-*_])(?:[ \t]*\1){2,}[ \t]*$")
QUOTE = re.compile(r"^ {0,3}>[ ]?(.*)$")
ITEM = re.compile(r"^( {0,3})([-*+]|\d{1,9}[.)])(?:([ \t]+)(.*)|[ \t]*)$")
TABLE_SEP = re.compile(r"^ {0,3}\|?[ \t]*:?-+:?[ \t]*(?:\|[ \t]*:?-+:?[ \t]*)*\|?[ \t]*$")
REF_DEF = re.compile(r"^ {0,3}\[([^\]]+)\]:[ \t]*(<[^>]*>|\S+)(?:[ \t]+(?:\"[^\"]*\"|'[^']*'|\([^)]*\)))?[ \t]*$")
SETEXT = re.compile(r"^ {0,3}(=+|-+)[ \t]*$")
HTML_ONLY = re.compile(r"^[ \t]*(?:</?[A-Za-z][A-Za-z0-9-]*(?:\s[^<>]*)?/?>[ \t]*)+$")


def indent_of(line):
    return len(line) - len(line.lstrip(" "))


def cells(line):
    line = line.strip()
    if line.startswith("|"):
        line = line[1:]
    if line.endswith("|") and not line.endswith("\\|"):
        line = line[:-1]
    return [c.replace("\\|", "|").strip() for c in re.split(r"(?<!\\)\|", line)]


class Converter:
    def __init__(self, refs, width):
        self.refs = refs
        self.width = width

    def inline(self, s):
        return Inline(self.refs).run(s)

    def starts_block(self, line, nxt=None):
        return bool(FENCE.match(line) or ATX.match(line) or RULE.match(line) or QUOTE.match(line) or
                    (ITEM.match(line) and ITEM.match(line).group(3)) or line.lstrip().startswith("<!--"))

    def blocks(self, lines, width, item=False):
        """Markdown lines -> a list of blocks: (lines, tight). tight: no
        blank line wanted before it (an item of a list without blank lines)"""
        out = []
        i, n = 0, len(lines)

        def add(block, tight=False):
            if block:
                out.append((block, tight))

        gap = False                        # a blank line before this block
        while i < n:
            line = lines[i]
            if not line.strip():
                i += 1
                gap = True
                continue
            after_gap, gap = gap, False
            m = FENCE.match(line)
            if m:
                lead, mark = len(m.group(1)), m.group(2)
                i += 1
                code = []
                while i < n:
                    c = FENCE.match(lines[i])
                    if c and c.group(2)[0] == mark[0] and len(c.group(2)) >= len(mark) and not c.group(3).strip():
                        i += 1
                        break
                    ln = lines[i]
                    code.append(ln[min(lead, indent_of(ln)):])
                    i += 1
                add([ascii_only(ln.rstrip())[:width].rstrip() for ln in code] or [""])
                continue
            if line.lstrip().startswith("<!--"):
                while i < n and "-->" not in lines[i]:
                    i += 1
                rest = lines[i].split("-->", 1)[1] if i < n else ""
                i += 1
                if rest.strip():
                    lines = lines[:i] + [rest] + lines[i:]
                    n += 1
                continue
            m = ATX.match(line)
            if m:
                add(wrap(self.inline(m.group(2) or "").upper(), width))
                i += 1
                continue
            if RULE.match(line):
                add(["-" * width])
                i += 1
                continue
            if QUOTE.match(line):
                inner = []
                while i < n and QUOTE.match(lines[i]):
                    inner.append(QUOTE.match(lines[i]).group(1))
                    i += 1
                if width - 2 < NARROW:     # (no room to indent)
                    add(self.render(inner, width))
                else:
                    add(hang(self.render(inner, width - 2), "  ", "  "))
                continue
            m = ITEM.match(line)
            if m:
                i = self.list_(lines, i, width, out, item and bool(out) and not after_gap)
                continue
            if "|" in line and i + 1 < n and "-" in lines[i + 1] and TABLE_SEP.match(lines[i + 1]) and \
                    ("|" in lines[i + 1] or len(cells(line)) == 1):
                head = [self.inline(c) for c in cells(line)]
                i += 2
                first = True
                while i < n and lines[i].strip() and "|" in lines[i]:
                    row = []
                    for k, c in enumerate(cells(lines[i])):
                        v = self.inline(c).strip()
                        h = head[k].strip() if k < len(head) else ""
                        if v:
                            row += hang(wrap(h + ": " + v if h else v, width - 2), "", "  ")
                    if row:
                        add(row, False)
                    first = False
                    i += 1
                if first:                  # a header and no rows: the header alone
                    add(wrap(" ".join(h for h in head if h), width))
                continue
            if indent_of(line.replace("\t", "    ")) >= 4:
                code = []
                while i < n and (not lines[i].strip() or indent_of(lines[i].replace("\t", "    ")) >= 4):
                    code.append(lines[i].replace("\t", "    ")[4:])
                    i += 1
                while code and not code[-1].strip():
                    code.pop()
                add([ascii_only(ln.rstrip())[:width].rstrip() for ln in code])
                continue
            if HTML_ONLY.match(line) and not re.search(r"(?i)<(img|br)\b", line):
                i += 1
                continue
            # a paragraph (or a heading underlined with = or -)
            para = [line]
            i += 1
            heading = False
            while i < n and lines[i].strip():
                if SETEXT.match(lines[i]):
                    heading = True
                    i += 1
                    break
                if self.starts_block(lines[i]):
                    break
                para.append(lines[i])
                i += 1
            text = ""
            for k, ln in enumerate(para):
                hard = k + 1 < len(para) and (ln.endswith("  ") or ln.rstrip().endswith("\\"))
                ln = ln.strip()
                if hard and ln.endswith("\\"):
                    ln = ln[:-1].rstrip()
                text += ln + ("\n" if hard else " ")
            text = self.inline(text.rstrip())
            add(wrap(text.upper() if heading else text, width))
        return out

    def list_(self, lines, i, width, out, tight_first=False):
        """the list starting at lines[i], into out; the line after it.
        tight_first: it follows its item's text with no blank line"""
        n = len(lines)
        first = ITEM.match(lines[i])
        ordered = first.group(2)[0].isdigit()
        number = int(first.group(2)[:-1]) if ordered else 0
        items = []                         # (marker, lines, a blank line before it)
        loose = False
        while i < n:
            m = ITEM.match(lines[i])
            if not m or m.group(2)[0].isdigit() != ordered or RULE.match(lines[i]) or \
                    (ordered and m.group(2)[-1] != first.group(2)[-1]):
                break
            marker_w = len(m.group(1)) + len(m.group(2))
            gap = len((m.group(3) or " ").replace("\t", "    "))
            content = marker_w + (gap if 1 <= gap <= 4 and m.group(4) else 1)
            body = [m.group(4) or ""]
            i += 1
            blank = False
            while i < n:
                ln = lines[i].replace("\t", "    ")
                if not ln.strip():
                    blank = True
                    body.append("")
                    i += 1
                    continue
                if indent_of(ln) >= content:
                    body.append(ln[content:])
                elif not blank and not self.starts_block(ln) and not SETEXT.match(ln):
                    body.append(ln.strip())   # a paragraph carried on without its indent
                else:
                    break
                blank = False
                i += 1
            while body and not body[-1].strip():
                body.pop()
                blank = True
            nxt = ITEM.match(lines[i]) if i < n else None
            if blank and nxt and nxt.group(2)[0].isdigit() == ordered and \
                    (not ordered or nxt.group(2)[-1] == first.group(2)[-1]):
                loose = True
            items.append(body)
        for k, body in enumerate(items):
            mark = f"{number + k}. " if ordered else "- "
            tight = not loose and (k > 0 or tight_first)
            if width - len(mark) < NARROW:  # (no room to indent: the marker on a line of its own)
                out.append(([mark.rstrip()] + self.render(body, width, item=True), tight))
                continue
            inner = self.render(body, width - len(mark), item=True)
            out.append((hang(inner or [""], mark, " " * len(mark)), tight))
        return i

    def render(self, lines, width, item=False):
        out = []
        for block, tight in self.blocks(lines, width, item):
            if out and not tight:
                out.append("")
            out += block
        return out


def convert(md, width=WIDTH, max_bytes=MAX_BYTES):
    """README.md's text -> what the cart shows (a str of ASCII and \\n)"""
    md = md.replace("\r\n", "\n").replace("\r", "\n")
    if md.startswith("\ufeff"):
        md = md[1:]
    lines = md.split("\n")
    if lines and lines[0].strip() == "---":            # front matter
        for k in range(1, len(lines)):
            if lines[k].strip() in ("---", "..."):
                lines = lines[k + 1:]
                break
    refs, body, fence = {}, [], None
    for ln in lines:                       # link definitions: anywhere but in a code block
        f = FENCE.match(ln)
        if f and fence is None:
            fence = f.group(2)[0]
        elif f and fence == f.group(2)[0] and not f.group(3).strip():
            fence = None
        elif fence is None:
            m = REF_DEF.match(ln)
            if m:
                refs.setdefault(m.group(1).strip().lower(), m.group(2))
                continue
        body.append(ln)
    out = Converter(refs, width).render(body, width)
    tidy = []
    for ln in out:
        ln = ln.rstrip()
        if not ln and (not tidy or not tidy[-1]):
            continue                       # one blank line at most, none at the top
        tidy.append(ln)
    while tidy and not tidy[-1]:
        tidy.pop()
    assert all(len(ln) <= width for ln in tidy), [ln for ln in tidy if len(ln) > width]
    text = "".join(ln + "\n" for ln in tidy)
    if len(text) > max_bytes:
        room = max_bytes - len(CUT_LINE[:width]) - 2
        kept = text[:room + 1]
        kept = kept[:kept.rfind("\n") + 1]
        text = kept + "\n" + CUT_LINE[:width] + "\n"
    assert len(text) <= max_bytes and all(c == "\n" or " " <= c <= "~" for c in text)
    return text


def check(text, width=WIDTH, max_bytes=MAX_BYTES):
    """why text isn't a README the cart can show, or None"""
    if len(text) > max_bytes:
        return f"{len(text)} bytes: {max_bytes} at most"
    for k, ln in enumerate(text.split("\n")):
        if len(ln) > width:
            return f"line {k + 1} is {len(ln)} characters: {width} at most"
        if any(not " " <= c <= "~" for c in ln):
            return f"line {k + 1} has a character outside printable ASCII"
    return None


def main():
    import argparse
    ap = argparse.ArgumentParser(description="A plugin's README.md as the cart shows it.")
    ap.add_argument("readme")
    ap.add_argument("-o", "--out", help="write the text here (default: print it)")
    ap.add_argument("--width", type=int, default=WIDTH)
    a = ap.parse_args()
    text = convert(open(a.readme, encoding="utf-8", errors="replace").read(), a.width)
    if a.out:
        with open(a.out, "w", encoding="ascii", newline="\n") as f:
            f.write(text)
    else:
        sys.stdout.write(text)


if __name__ == "__main__":
    main()
