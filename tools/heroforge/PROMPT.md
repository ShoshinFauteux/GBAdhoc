# Hero card generation prompt (for Gemini image editing)

Feed the **box art image** plus the prompt below. Replace `{GAME}` with the
title. The constraints in §3 are not stylistic preferences — they are where
the emulator draws its text, measured from four failed mockups.

---

## The prompt

```
Extend and clean this Game Boy Advance box art into a widescreen wallpaper.

SOURCE
This is the front cover of {GAME}. Keep its artwork, characters, colour
palette and illustration style exactly as they are — this should read as the
same artist's work, extended, not a reinterpretation.

OUTPUT
A single image, 16:9 landscape, no borders, no frame, no drop shadow, edge to
edge. Target 480x272 pixels — assume it will be viewed small, so favour large
readable shapes over fine detail.

REMOVE COMPLETELY
- the game's title, logo and all wordmarks
- the silver "GAME BOY ADVANCE" spine down the left edge
- the ESRB / PEGI rating badge
- publisher and developer logos (Nintendo, Konami, THQ, etc.)
- "Link It Up!", "Player's Choice", seals, starbursts, any sticker or banner
- any text of any kind, in any language

EXTEND
The cover is roughly square; the output is widescreen. Paint new artwork
outward to the left and right in the same style, continuing the scene
naturally — landscape, sky, environment, atmosphere. Do not mirror or repeat
existing elements. Do not add new characters or objects that were not implied
by the original.

COMPOSITION — this is the important part
- Place the main subject in the RIGHT HALF of the frame, roughly 60-95% across.
- The LEFT 55% must be visually QUIET: open sky, mist, gradient, shadow,
  blurred distance. Low contrast, low detail, darker than the right. White
  text will be printed over this region and must stay readable. Nothing with
  hard edges or bright highlights belongs there.
- The BOTTOM 45 pixels (~17% of height) must also be calm and dark — a strip
  of text runs across it.
- Overall exposure slightly dark, as if lit for a dark UI. Rich colour is
  good; blown highlights and busy texture in the left half are not.

STYLE
Cinematic, atmospheric, painterly. Think a game's title screen background
rather than a product photo of a box.

DO NOT
No text. No borders or vignette frames. No collage or panel layout. No
UI mockups. No logos. No watermarks. Do not letterbox — fill the frame.
```

---

## Why each rule is there

| Rule | Failure it prevents |
|---|---|
| subject in the right half | the shell writes the title, size and list down the left |
| left 55% quiet | "Pokemon Emerald" over the POKÉMON wordmark was unreadable |
| bottom 45 px calm | `X play / L/R page / START settings` vanished on bright covers |
| remove the logo | the UI already prints the title as text two lines below it |
| slightly dark | the shell's text is white; nothing else darkens the art |
| no borders | it is a full-bleed background, not a card |

## Evaluating the result

Drop the images into a folder and run:

```
python tools/heroforge/preview.py <folder>
```

That composites each one under the real Marquee chrome — real fonts, real
palette, real coordinates from `shell_marquee()` — so a generated hero and a
`styles.panorama()` hero can be judged on the same screen rather than as
loose images. Legibility of the left column is the thing to look at; that is
what separated the good mockups from the bad ones.
