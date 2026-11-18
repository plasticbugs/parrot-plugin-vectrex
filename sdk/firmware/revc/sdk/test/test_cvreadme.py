#!/usr/bin/env python3
"""cvreadme.py's rules, one case each, and the sample README against its
kept output (readme_sample.md -> readme_sample.txt).

    python3 -I sdk/test/test_cvreadme.py

After a change to the converter that is meant to change the output:
    python3 -I sdk/cvreadme.py sdk/test/readme_sample.md -o sdk/test/readme_sample.txt
(the plugin registry shows the text as it is: say so to whoever keeps it).
"""
import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))
import cvreadme                                                       # noqa: E402
from cvreadme import convert                                          # noqa: E402


def read(name):
    with open(os.path.join(HERE, name), encoding="utf-8") as f:
        return f.read()


def L(md, **kw):
    return convert(md, **kw).split("\n")[:-1]


class Rules(unittest.TestCase):
    def test_heading(self):
        self.assertEqual(L("# My *Plugin*\nText"), ["MY PLUGIN", "", "Text"])
        self.assertEqual(L("Text\n## Two ##\nmore"), ["Text", "", "TWO", "", "more"])
        self.assertEqual(L("Title\n=====\n\nSub\n---\ntext"), ["TITLE", "", "SUB", "", "text"])
        self.assertEqual(L("# A heading that is longer than thirty characters"),
                         ["A HEADING THAT IS LONGER THAN", "THIRTY CHARACTERS"])

    def test_paragraph_reflow(self):
        self.assertEqual(L("one two\nthree   four\n\nfive"), ["one two three four", "", "five"])
        self.assertEqual(L("a" * 29 + " b c"), ["a" * 29, "b c"])
        self.assertEqual(L("a" * 30 + " b"), ["a" * 30, "b"])
        self.assertEqual(L("x" * 65), ["x" * 30, "x" * 30, "x" * 5])
        self.assertEqual(L("line one  \nline two\\\nline three"), ["line one", "line two", "line three"])

    def test_bullets(self):
        for mark in "-*+":
            self.assertEqual(L(f"{mark} one\n{mark} two is long enough to need a second line"),
                             ["- one", "- two is long enough to need a", "  second line"])
        self.assertEqual(L("- a\n\n- b"), ["- a", "", "- b"])

    def test_numbered(self):
        self.assertEqual(L("1. one\n1. two\n1. three"), ["1. one", "2. two", "3. three"])
        self.assertEqual(L("7. seven is long enough to need another line\n8. eight"),
                         ["7. seven is long enough to", "   need another line", "8. eight"])
        self.assertEqual(L("9) nine\n10) ten ten ten ten ten ten ten ten"),
                         ["9. nine", "10. ten ten ten ten ten ten", "    ten ten"])

    def test_nested(self):
        self.assertEqual(L("- a\n  - b\n    - c\n- d"), ["- a", "  - b", "    - c", "- d"])
        self.assertEqual(L("1. a\n   - b\n2. c"), ["1. a", "   - b", "2. c"])
        self.assertEqual(L("- a\n\n  second paragraph\n- b"), ["- a", "", "  second paragraph", "- b"])

    def test_links(self):
        self.assertEqual(L("[site](https://a.example/)"), ["site https://a.example/"])
        self.assertEqual(L("See [the docs](https://example.com/a/long/path) now"),
                         ["See the docs", "https://example.com/a/long/pat", "h now"])
        self.assertEqual(L("go to [it](https://a.example/x) ok"), ["go to it https://a.example/x", "ok"])
        self.assertEqual(L("<https://a.example/>"), ["https://a.example/"])
        self.assertEqual(L("bare https://a.example/b_c_d, then"), ["bare https://a.example/b_c_d,", "then"])
        self.assertEqual(L("[https://a.example](https://a.example/)"), ["https://a.example/"])
        self.assertEqual(L("[text][r] and [r]\n\n[r]: https://a.example"),
                         ["text https://a.example and r", "https://a.example"])
        self.assertEqual(L("[licence](LICENSE) [top](#top)"), ["licence top"])
        self.assertEqual(L("[mail](mailto:a@b.example) <c@d.example>"), ["mail a@b.example c@d.example"])

    def test_inline_code_and_emphasis(self):
        self.assertEqual(L("`a_b` **b** __c__ *d* _e_ ~~f~~ ***g***"), ["a_b b c d e f g"])
        self.assertEqual(L("snake_case_name 2*3*4 a * b \\*c\\*"), ["snake_case_name 2*3*4 a * b", "*c*"])
        self.assertEqual(L("``a ` b`` and `**kept**`"), ["a ` b and **kept**"])

    def test_code_blocks(self):
        self.assertEqual(L("```sh\n$ make   # **not** emphasis, and cut at the width\n\n  two\n```"),
                         ["$ make   # **not** emphasis, and cut at the width"[:30], "", "  two"])
        self.assertEqual(L("text\n\n    indented <b>\n        more\n\ntext"),
                         ["text", "", "indented <b>", "    more", "", "text"])
        self.assertEqual(L("~~~\n# not a heading\n[x]: https://not.a.definition\n~~~"),
                         ["# not a heading", "[x]: https://not.a.definition"])

    def test_images(self):
        self.assertEqual(L("![A shot](a.png) ![](b.png)"), ["[image: A shot] [image]"])
        self.assertEqual(L('<img src="x.png" alt="Logo">'), ["[image: Logo]"])
        self.assertEqual(L("[![Badge](b.svg)](https://ci.example/)"), ["[image: Badge]", "https://ci.example/"])

    def test_table(self):
        self.assertEqual(L("| Key | Does |\n|---|:-:|\n| A | Start |\n| B | |\n| `#` | a \\| b |"),
                         ["Key: A", "Does: Start", "", "Key: B", "", "Key: #", "Does: a | b"])
        self.assertEqual(L("Key | Does\n--- | ---\nA | A value long enough to wrap around"),
                         ["Key: A", "Does: A value long enough to", "  wrap around"])

    def test_quote(self):
        self.assertEqual(L("> quoted text that is long enough to wrap\n> - item\n\nafter"),
                         ["  quoted text that is long", "  enough to wrap", "", "  - item", "", "after"])
        self.assertEqual(L("> a\n> > b"), ["  a", "", "    b"])

    def test_html(self):
        self.assertEqual(L('<p align="center">\n<b>Bold</b> and <i>it</i><br>next\n</p>'), ["Bold and it", "next"])
        self.assertEqual(L("a <!-- gone --> b\n\n<!--\nall\ngone\n-->\nc"), ["a b", "", "c"])
        self.assertEqual(L("&lt;tag&gt; &amp; &copy; &#65;"), ["<tag> & (c) A"])

    def test_rule(self):
        for r in ("---", "***", "___", "- - -", " * * *"):
            self.assertEqual(L(f"a\n\n{r}\n\nb"), ["a", "", "-" * 30, "", "b"])

    def test_non_ascii(self):
        self.assertEqual(L("“quoted” ‘it’s’ a–b c—d e…"), ['"quoted" \'it\'s\' a-b c--d e...'])
        self.assertEqual(L("café naïve Ångström straße Łódź"), ["cafe naive Angstrom strasse", "Lodz"])
        self.assertEqual(L("日本 \U0001F3AE️ x​y"), ["?? ? xy"])
        self.assertEqual(L("```\né\t→\n```"), ["e ->"])

    def test_cap(self):
        text = convert("word " * 8000)
        self.assertLessEqual(len(text), 16384)
        self.assertTrue(text.endswith("\n\n" + cvreadme.CUT_LINE + "\n"))
        self.assertIsNone(cvreadme.check(text))
        short = convert("word " * 100)
        self.assertNotIn(cvreadme.CUT_LINE, short)
        exact = convert(("x" * 30 + "\n\n") * 600)            # 600 lines of 30 and 599 blank: 19199 bytes
        self.assertLessEqual(len(exact), 16384)
        self.assertGreater(len(exact), 16384 - 64)

    def test_always_fits(self):
        for width in (30, 20, 12):
            text = convert(read("readme_sample.md"), width)
            self.assertIsNone(cvreadme.check(text, width))
        self.assertEqual(convert(""), "")
        self.assertEqual(convert("\n\n# \n\n"), "")
        self.assertEqual(convert("a\r\nb\r\n"), "a b\n")

    def test_sample(self):
        md, want = read("readme_sample.md"), read("readme_sample.txt")
        self.assertEqual(convert(md), want)
        self.assertEqual(convert(md), convert(md))


if __name__ == "__main__":
    unittest.main()
