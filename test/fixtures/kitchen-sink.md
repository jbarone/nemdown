---
title: Kitchen Sink
tags:
  - markdown
  - test
aliases: [sink, everything]
published: true
rating: 4.5
created: 2026-09-10
updated: 2026-09-10T14:30:00
empty:
---

# Kitchen Sink

Body text with **bold**, *italic*, ***both***, `inline code`, ~~struck~~,
==highlighted==, and an entity: &amp; &copy; &#8212; &#x2192;.

A [external link](https://example.com) and a [[wikilink]] and an
embed: ![[diagram.png]] and a sized embed ![[diagram.png|400]].

Nested emphasis: [a **bold [[link]]** inside](https://example.com).

## Lists

- level one
  - level two
    - level three
- [x] done task
- [ ] open task

1. first
9. ninth
10. tenth

## Quote and callout

> plain blockquote
> second line

> [!warning] Careful
> Callout body text.

> [!note]
> Title-less callout.

## Code

```c
int main(void) { printf("hi\n"); return 0; }
```

## Table

| Left | Center | Right |
|:-----|:------:|------:|
| a    | b      | c     |

## Math

Inline $x^2 + y^2$ and display:

$$\frac{a}{b} = \sum_{i=0}^{n} x_i$$

---

![alt text](image.png)
