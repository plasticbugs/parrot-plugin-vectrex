---
title: front matter is left out
---
# Sample Plugin — a “test” README

[![Build status](https://example.com/badge.svg)](https://example.com/ci)

A paragraph that is long enough to be reflowed to the cart's
thirty columns, with *emphasis*, **strong**, __also strong__,
_also emphasis_, ~~struck~~ and `inline_code()` in it. A
snake_case_name and 2*3*4 stay as they are, and \*escaped\* stars too.

Second paragraph…  
with a hard break, café déjà vu, a naïve Ærøskøbing, 日本 and an emoji 🎮.

Setext Heading
==============

Another one
-----------

## Controls ##

- Joystick: move
* Left fire: **B**
+ Right fire: A, which is a long enough item to need a second line
  - nested bullet one
  - nested bullet two that is also long enough to wrap around
    1. deep numbered
    2. deep numbered again

3. Third, the first of a numbered list that starts at three
4. Fourth
   continues here

   with a second paragraph in the item

10) Ten with a parenthesis
11) Eleven

- [ ] a task not done
- [x] a task done

### Links

See [the source](https://github.com/example/sample-plugin) for the code, the
[licence](LICENSE.md), [a section](#controls), [docs][ref], [ref] and
<https://example.org/auto>. Bare: https://example.org/a/very/long/path/that/does/not/fit/on/a/line/at/all.html.
Mail <someone@example.org> or [write](mailto:other@example.org).
[https://same.example/](https://same.example/)

[ref]: https://example.net/reference "A title"

## Code

```c
int main(void) {
    return cvk_start(&cfg);   // a comment that runs past the width
}
```

    indented code
        stays as it is, cut at the width too

~~~
tilde fence with *stars* and <b>tags</b> kept
~~~

## Images and HTML

![A screenshot](shot.png) and ![](empty.png)

<p align="center">
  <img src="logo.png" alt="The logo" width="200">
</p>

Text with <b>bold tags</b>, a<br>line break, &amp; an entity &lt;kept&gt;.

<!-- a comment
over two lines -->

<details>
<summary>More</summary>
Hidden text.
</details>

## Table

| Button | Does | Notes |
|:-------|:----:|------:|
| A | Start | |
| B | **Back** to the menu, which is long | `code` |
| `#` | Remove \| pipe | [link](https://example.com/t) |

> A block quote that is long enough to need wrapping at the width.
> - with a list
>
> > nested quote

***

---

Credits: © 2026 Someone™ — thanks → all.
